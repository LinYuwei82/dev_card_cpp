#include "devcard/server.h"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "devcard/sha256.h"
#include "font_manifest.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "devcard/backup.h"
#include "devcard/db.h"
#include "devcard/error.h"
#include "devcard/image_meta.h"
#include "devcard/legacy.h"
#include "devcard/migrations.h"
#include "devcard/models.h"
#include "devcard/service/device_service.h"
#include "devcard/service/user_service.h"
#include "devcard/settings.h"
#include "devcard/xlsx.h"
#include "web_assets.h"
#include "font_coverage.h"

namespace devcard {
namespace {

using nlohmann::json;

constexpr auto kSessionTtl = std::chrono::hours(12);

struct Session {
  std::string user;
  std::chrono::steady_clock::time_point last_seen;
};

struct ServerState {
  httplib::Server server;
  std::thread thread;
  std::unique_ptr<Db> db;
  std::unique_ptr<DeviceService> devices;
  std::unique_ptr<UserService> users;
  std::filesystem::path data_dir;
  std::filesystem::path font_dir;
  std::map<std::string, std::string> fonts;
  FontCoverage coverage;
  std::string local_token;
  LegacyImportReport legacy;
  bool seeded = false;

  std::mutex sessions_mutex;
  std::map<std::string, Session> sessions;
};

std::string json_body(const json& value) { return value.dump(); }

void set_json(httplib::Response& response, const std::string& body, int status = 200) {
  response.status = status;
  response.set_header("Content-Type", "application/json; charset=utf-8");
  response.set_header("Cache-Control", "no-store");
  response.body = body;
}

void set_json(httplib::Response& response, const json& value, int status = 200) {
  set_json(response, json_body(value), status);
}

void set_asset(httplib::Response& response, const std::string& body, const char* type) {
  response.status = 200;
  response.set_header("Content-Type", type);
  response.set_header("Cache-Control", "no-store");
  response.body = body;
}

json device_json(const Device& device) {
  return {{"id", device.id},
          {"dev_name", device.dev_name},
          {"location", device.location},
          {"control_range", device.control_range},
          {"phone", device.phone},
          {"created_at", device.created_at},
          {"updated_at", device.updated_at}};
}

json user_json(const User& user) {
  return {{"user_name", user.user_name},
          {"display_name", user.display_name},
          {"role", user.role},
          {"must_change_password", user.must_change_password},
          {"locked_until", user.locked_until},
          {"created_at", user.created_at},
          {"updated_at", user.updated_at}};
}

Device device_from_json(const json& body) {
  Device device;
  device.dev_name = body.value("dev_name", "");
  device.location = body.value("location", "");
  device.control_range = body.value("control_range", "");
  device.phone = body.value("phone", "");
  return device;
}

json parse_body(const std::string& body) {
  try {
    return json::parse(body.empty() ? "{}" : body);
  } catch (const json::exception&) {
    throw AppError("bad_json", "请求体不是合法 JSON");
  }
}

int int_param(const httplib::Request& request, const char* name, int fallback) {
  const std::string value = request.get_param_value(name);
  if (value.empty()) return fallback;
  try {
    return std::stoi(value);
  } catch (...) {
    return fallback;
  }
}

DeviceQuery query_from(const httplib::Request& request) {
  DeviceQuery query;
  query.keyword = request.get_param_value("keyword");
  query.field = request.get_param_value("field");
  query.name = request.get_param_value("name");
  query.location = request.get_param_value("location");
  query.page = int_param(request, "page", 0);
  query.page_size = int_param(request, "page_size", 20);
  query.sort = request.get_param_value("sort");
  if (query.sort.empty()) query.sort = "id";
  query.desc = request.get_param_value("desc") == "1";
  return query;
}

std::string trim_copy(const std::string& value) {
  const std::size_t begin = value.find_first_not_of(" \t\r\n");
  if (begin == std::string::npos) return {};
  const std::size_t end = value.find_last_not_of(" \t\r\n");
  return value.substr(begin, end - begin + 1);
}

// 按 UTF-8 起始字节计数，一个汉字/符号算一个字。
std::size_t utf8_length(const std::string& value) {
  std::size_t count = 0;
  for (unsigned char ch : value) {
    if ((ch & 0xC0) != 0x80) ++count;
  }
  return count;
}

// 版本检查（M5/M6）：只读取数据目录里的更新清单并比较版本，不下载、不替换。
// 清单由发布方投放；缺失或非法时接口照常返回，只是不给「有新版本」。
// M6 起清单还承载「远端检查」的三个开关：是否联网查、查哪个地址、最快多久查一次。
constexpr std::string_view kUpdateManifestName = "update.json";
constexpr std::string_view kDefaultDownloadPage =
    "https://github.com/LinYuwei82/dev_card_cpp/releases";
constexpr std::string_view kDefaultUpdateEndpoint =
    "https://api.github.com/repos/LinYuwei82/dev_card_cpp/releases/latest";
constexpr long kDefaultUpdateIntervalHours = 24;

bool parse_version(const std::string& text, std::vector<long>* parts) {
  if (text.empty()) return false;
  std::vector<long> out;
  long value = 0;
  bool has_digit = false;
  for (const char ch : text) {
    if (ch >= '0' && ch <= '9') {
      value = value * 10 + (ch - '0');
      if (value > 100000000L) return false;
      has_digit = true;
    } else if (ch == '.') {
      if (!has_digit) return false;
      out.push_back(value);
      value = 0;
      has_digit = false;
    } else {
      return false;
    }
  }
  if (!has_digit) return false;
  out.push_back(value);
  *parts = out;
  return true;
}

// 逐段按数值比较，缺的段按 0 处理（1.2 == 1.2.0）。非法版本号视为相等。
int compare_version(const std::string& left, const std::string& right) {
  std::vector<long> a;
  std::vector<long> b;
  if (!parse_version(left, &a) || !parse_version(right, &b)) return 0;
  const std::size_t count = std::max(a.size(), b.size());
  for (std::size_t i = 0; i < count; ++i) {
    const long l = i < a.size() ? a[i] : 0;
    const long r = i < b.size() ? b[i] : 0;
    if (l != r) return l > r ? 1 : -1;
  }
  return 0;
}

struct UpdateManifest {
  bool found = false;
  std::string latest;
  std::string download_url;
  std::string notes;
  std::string error;
  // M6：远端检查的开关、地址与频率。清单不写就取默认值；
  // 地址不是 http/https 时直接关掉检查，宁可查不了也不发一个奇怪的请求。
  bool check_enabled = true;
  std::string check_endpoint{std::string(kDefaultUpdateEndpoint)};
  long check_interval_hours = kDefaultUpdateIntervalHours;
};

bool is_http_url(const std::string& value) {
  return value.rfind("http://", 0) == 0 || value.rfind("https://", 0) == 0;
}

UpdateManifest read_update_manifest(const std::filesystem::path& data_dir) {
  UpdateManifest result;
  std::ifstream in(data_dir / kUpdateManifestName, std::ios::binary);
  if (!in) return result;
  result.found = true;
  const std::string text((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());
  const json doc = json::parse(text, nullptr, false);
  if (doc.is_discarded() || !doc.is_object()) {
    result.error = "更新清单不是合法的 JSON 对象";
    return result;
  }
  result.latest = trim_copy(doc.value("latest", std::string{}));
  result.download_url = trim_copy(doc.value("download_url", std::string{}));
  result.notes = trim_copy(doc.value("notes", std::string{}));
  if (doc.contains("check_enabled") && doc["check_enabled"].is_boolean()) {
    result.check_enabled = doc["check_enabled"].get<bool>();
  }
  const std::string endpoint = trim_copy(doc.value("check_endpoint", std::string{}));
  if (!endpoint.empty()) result.check_endpoint = endpoint;
  if (!is_http_url(result.check_endpoint)) result.check_enabled = false;
  if (doc.contains("check_interval_hours") && doc["check_interval_hours"].is_number_integer()) {
    result.check_interval_hours = std::max<long>(1, doc["check_interval_hours"].get<long>());
  }
  std::vector<long> parts;
  if (result.latest.empty()) {
    result.error = "更新清单缺少 latest";
  } else if (!parse_version(result.latest, &parts)) {
    result.error = "更新清单中的 latest 不是合法版本号";
  }
  return result;
}

std::string download_page() {
  if (const char* env = std::getenv("DEVCARD_DOWNLOAD_PAGE"); env && *env) {
    return env;
  }
  return std::string(kDefaultDownloadPage);
}

constexpr std::string_view kLogoSettingKey = "company_logo_path";

// 只接受服务端自己写的相对路径，防路径穿越。
bool safe_logo_relative(const std::string& path) {
  return !path.empty() && path.find("..") == std::string::npos &&
         path.front() != '/' && path.front() != '\\';
}

std::string read_file_bytes(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return {};
  return std::string(std::istreambuf_iterator<char>(in),
                     std::istreambuf_iterator<char>());
}

int hex_digit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return 0;
}

const std::string& logo_jpeg() {  static const std::string bytes = [] {
    const std::string hex = embedded_logo_jpg_hex();
    std::string out;
    out.reserve(hex.size() / 2);
    for (std::size_t i = 0; i + 1 < hex.size(); i += 2) {
      out.push_back(static_cast<char>((hex_digit(hex[i]) << 4) | hex_digit(hex[i + 1])));
    }
    return out;
  }();
  return bytes;
}

int status_for(const AppError& error) {
  const std::string& code = error.code();
  if (code == "duplicate" || code == "last_admin" || code == "self_delete" ||
      code == "no_user")
    return 409;
  if (code == "not_found") return 404;
  if (code == "legacy_db") return 409;
  if (code == "invalid_credentials") return 401;
  if (code == "locked") return 423;
  if (code == "forbidden" || code == "must_change_password") return 403;
  if (code == "invalid_device" || code == "invalid_user" ||
      code == "weak_password" || code == "bad_field" || code == "bad_json" ||
      code == "bad_header" || code == "bad_xlsx" || code == "not_legacy" ||
      code == "bad_backup" || code == "old_password_required" ||
      code == "bad_logo" || code == "logo_too_large" ||
      code == "bad_company_name" || code == "logo_write")
    return 400;
  return 500;
}

std::string random_token() {
  static const char hex[] = "0123456789abcdef";
  std::random_device device;
  std::string token;
  token.reserve(48);
  for (int i = 0; i < 24; ++i) token.push_back(hex[device() % 16]);
  return token;
}

std::string cookie_value(const httplib::Request& request, const char* name) {
  const std::string header = request.get_header_value("Cookie");
  const std::string prefix = std::string(name) + "=";
  std::size_t start = 0;
  while (start < header.size()) {
    std::size_t end = header.find(';', start);
    if (end == std::string::npos) end = header.size();
    std::string part = header.substr(start, end - start);
    while (!part.empty() && std::isspace(static_cast<unsigned char>(part.front()))) {
      part.erase(part.begin());
    }
    if (part.rfind(prefix, 0) == 0) return part.substr(prefix.size());
    start = end + 1;
  }
  return {};
}

std::string session_user(ServerState& state, const httplib::Request& request) {
  const std::string token = cookie_value(request, "sid");
  if (token.empty()) return {};
  std::lock_guard<std::mutex> lock(state.sessions_mutex);
  auto it = state.sessions.find(token);
  if (it == state.sessions.end()) return {};
  const auto now = std::chrono::steady_clock::now();
  if (now - it->second.last_seen > kSessionTtl) {
    state.sessions.erase(it);
    return {};
  }
  it->second.last_seen = now;
  return it->second.user;
}

std::string create_session(ServerState& state, const std::string& user) {
  const std::string token = random_token();
  std::lock_guard<std::mutex> lock(state.sessions_mutex);
  state.sessions[token] = Session{user, std::chrono::steady_clock::now()};
  return token;
}

void destroy_session(ServerState& state, const httplib::Request& request) {
  const std::string token = cookie_value(request, "sid");
  if (token.empty()) return;
  std::lock_guard<std::mutex> lock(state.sessions_mutex);
  state.sessions.erase(token);
}

void set_session_cookie(httplib::Response& response, const std::string& token) {
  response.set_header("Set-Cookie",
                      "sid=" + token +
                          "; Path=/; HttpOnly; SameSite=Lax; Max-Age=43200");
}

void clear_session_cookie(httplib::Response& response) {
  response.set_header("Set-Cookie", "sid=; Path=/; HttpOnly; SameSite=Lax; Max-Age=0");
}

bool is_loopback(const std::string& address) {
  return address == "127.0.0.1" || address == "::1" ||
         address == "::ffff:127.0.0.1";
}

bool host_allowed(std::string host) {
  if (host.empty()) return false;
  std::transform(host.begin(), host.end(), host.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  std::string name = host;
  if (!name.empty() && name.front() == '[') {
    const std::size_t end = name.find(']');
    if (end != std::string::npos) name = name.substr(1, end - 1);
  } else {
    const std::size_t colon = name.find(':');
    if (colon != std::string::npos) name = name.substr(0, colon);
  }
  return name == "127.0.0.1" || name == "localhost" || name == "::1";
}

User current_user(ServerState& state, const httplib::Request& request) {
  const std::string name = session_user(state, request);
  if (name.empty()) throw AppError("unauthorized", "未登录或会话已过期");
  return state.users->get(name);
}

User require_admin(ServerState& state, const httplib::Request& request) {
  User user = current_user(state, request);
  if (user.role != "admin") throw AppError("forbidden", "需要管理员权限");
  return user;
}

}  // namespace

struct ServerHandle::Impl : ServerState {};

ServerHandle::ServerHandle() : impl_(std::make_unique<Impl>()) {}
ServerHandle::~ServerHandle() {
  if (impl_ && impl_->server.is_running()) impl_->server.stop();
  if (impl_ && impl_->thread.joinable()) impl_->thread.join();
}

const std::string& ServerHandle::local_token() const { return impl_->local_token; }

std::unique_ptr<ServerHandle> start_server(const ServerOptions& options) {
  // 内嵌前端自检：C2026 截断后页面仍能打开，只是内容少了几个字符，光靠肉眼看不出。
  // 这里比对的是「直读 web/ 源文件」的字节数与 SHA256，不是生成出来的 .cpp。
  const std::string asset_error = embedded_assets_error();
  if (!asset_error.empty()) {
    throw AppError("web_assets", "内嵌前端与源文件不一致：" + asset_error);
  }

  auto handle = std::unique_ptr<ServerHandle>(new ServerHandle());
  ServerState& state = *handle->impl_;
  state.data_dir = options.data_dir;

  std::error_code ec;
  std::filesystem::create_directories(options.data_dir, ec);
  if (ec) {
    throw AppError("data_dir", "无法创建数据目录: " + ec.message());
  }

  // 旧库自动探测：先备份、再只读导入，成功后旧库改名 *.migrated-<时间戳>。
  state.legacy = auto_import_if_needed(options.data_dir);

  state.db = std::make_unique<Db>(options.data_dir / "devicedb.db");
  migrate(*state.db);

  const SeedResult seed = seed_default_users(*state.db, options.data_dir);
  state.seeded = seed.seeded;
  if (seed.seeded) {
    std::cout << "首次启动：已生成初始账号，口令写入 " << seed.password_file
              << "（首次登录必须修改）" << std::endl;
    for (const auto& [name, password] : seed.accounts) {
      std::cout << "  用户名: " << name << "  初始口令: " << password << std::endl;
    }
  }

  state.devices = std::make_unique<DeviceService>(*state.db);
  state.users = std::make_unique<UserService>(*state.db);
  state.local_token = random_token();
  state.server.set_payload_max_length(8 * 1024 * 1024);

  // 子集字体随包分发，启动时读进内存并逐份校验。
  // 校验值是构建期从 web/fonts/ 读出来的（cmake/font_manifest.cmake），不是「服务器自己报自己的大小」——
  // 后者两边一起错也一致，等于没检查。
  // 缺一份或对不上就中止启动：一个能录台账却永远打不出卡的版本，比启动即失败更糟，
  // 用户会先录一批数据，再发现打不了。
  state.font_dir = default_font_dir();
  for (const auto& spec : kEmbeddedFonts) {
    const auto path = state.font_dir / spec.name;
    std::string bytes = read_file_bytes(path);
    if (bytes.empty()) {
      throw AppError("font_missing",
                     "缺少随包字体 " + path.string() +
                         "：打印与预览必须用同一份子集，缺字体时不做降级，直接中止启动");
    }
    const std::string actual = sha256_hex(bytes);
    if (bytes.size() != spec.bytes || actual != spec.sha256) {
      throw AppError("font_corrupt",
                     "字体校验失败 " + path.string() + "：期望 " +
                         std::to_string(spec.bytes) + " 字节 / SHA256 " + spec.sha256 +
                         "，实得 " + std::to_string(bytes.size()) + " 字节 / SHA256 " + actual +
                         "（多为重新子集后未重建，或文件被截断）");
    }
    state.fonts[spec.name] = std::move(bytes);
  }
  // 「哪些字能印」的唯一判据取自随包字体的 cmap，不做手写清单：
  // 换字体或换字符集时，它自动跟着变。
  const auto regular = state.fonts.find("NotoSansSC-Regular.subset.ttf");
  if (regular != state.fonts.end()) {
    state.coverage = FontCoverage::parse(regular->second);
    if (!state.coverage.error().empty()) {
      std::cerr << "警告：解析子集字体 cmap 失败：" << state.coverage.error() << std::endl;
    }
  }

  const auto schema = state.db->schema_version();
  const std::string version = DEVCARD_VERSION;

  state.server.Get("/api/health", [&state, schema, version](
                                     const httplib::Request&,
                                     httplib::Response& response) {
    set_json(response, json{{"status", "ok"},
                            {"version", version},
                            {"schema_version", schema},
                            {"mode", "local"},
                            {"legacy_imported", state.legacy.performed}});
  });

  // 版本检查：只读更新清单并比较版本，返回下载页；不下载、不替换。
  state.server.Get("/api/version", [&state, schema, version](
                                       const httplib::Request&,
                                       httplib::Response& response) {
    const UpdateManifest manifest = read_update_manifest(state.data_dir);
    const bool available = !manifest.latest.empty() &&
                           compare_version(manifest.latest, version) > 0;
    set_json(response,
             json{{"version", version},
                  {"schema_version", schema},
                  {"latest", manifest.latest.empty() ? json(nullptr)
                                                     : json(manifest.latest)},
                  {"update_available", available},
                  {"download_url", manifest.download_url},
                  {"download_page", download_page()},
                  {"notes", manifest.notes},
                  // M6：远端检查的三个开关都来自同一份 update.json，前端不再写死地址。
                  {"check", json{{"enabled", manifest.check_enabled},
                                 {"endpoint", manifest.check_endpoint},
                                 {"interval_hours", manifest.check_interval_hours}}},
                  {"manifest", manifest.found
                                   ? json((state.data_dir / kUpdateManifestName)
                                              .string())
                                   : json(nullptr)},
                  {"checked_at", now_text()},
                  {"error", manifest.error.empty() ? json(nullptr)
                                                   : json(manifest.error)}});
  });

  state.server.Post("/api/session/local",
                    [&state](const httplib::Request& request,
                             httplib::Response& response) {
                      if (!is_loopback(request.remote_addr)) {
                        throw AppError("forbidden", "本地会话只能从本机建立");
                      }
                      const json body = parse_body(request.body);
                      const std::string token = body.value("token", "");
                      if (token != state.local_token) {
                        throw AppError("invalid_credentials", "本地会话令牌无效");
                      }
                      const std::string name = state.users->local_user();
                      if (name.empty()) {
                        throw AppError("no_user",
                                       "没有可用账号，请执行 --set-password <用户名>");
                      }
                      const User user = state.users->get(name);
                      set_session_cookie(response, create_session(state, name));
                      set_json(response, json{{"user", user_json(user)}});
                    });

  state.server.Post("/api/session/login",
                    [&state](const httplib::Request& request,
                             httplib::Response& response) {
                      const json body = parse_body(request.body);
                      const User user = state.users->authenticate(
                          body.value("username", ""), body.value("password", ""));
                      set_session_cookie(response,
                                         create_session(state, user.user_name));
                      set_json(response, json{{"user", user_json(user)}});
                    });

  state.server.Post("/api/session/logout",
                    [&state](const httplib::Request& request,
                             httplib::Response& response) {
                      destroy_session(state, request);
                      clear_session_cookie(response);
                      set_json(response, json{{"ok", true}});
                    });

  state.server.Get("/api/bootstrap", [&state, schema, version](
                                         const httplib::Request& request,
                                         httplib::Response& response) {
    const User user = current_user(state, request);
    json legacy = nullptr;
    if (state.legacy.performed) {
      legacy = {{"source", state.legacy.source},
                {"backup", state.legacy.backup},
                {"archived_as", state.legacy.archived_as},
                {"devices_total", state.legacy.devices_total},
                {"devices_imported", state.legacy.devices_imported},
                {"devices_skipped", state.legacy.devices_skipped},
                {"users_total", state.legacy.users_total},
                {"users_imported", state.legacy.users_imported},
                {"users_skipped", state.legacy.users_skipped},
                {"warnings", state.legacy.warnings}};
    }
    set_json(response, json{{"user", user_json(user)},
                            {"version", version},
                            {"schema_version", schema},
                            {"data_dir", state.data_dir.string()},
                            {"company_name", company_name(*state.db)},
                            {"company_name_print", company_print_name(*state.db)},
                            {"company_name_is_default",
                             get_setting(*state.db, "print_company_name").empty()},
                            {"company_name_max_width", kCompanyNameMaxWidth},
                            {"company_logo_url", "/company-logo"},
                            {"company_logo_updated_at",
                             get_setting(*state.db, "company_logo_updated_at")},
                            {"legacy", legacy}});
  });

  state.server.Get("/api/devices", [&state](const httplib::Request& request,
                                            httplib::Response& response) {
    const DevicePage page = state.devices->list(query_from(request));
    json items = json::array();
    for (const Device& device : page.items) items.push_back(device_json(device));
    set_json(response, json{{"items", items},
                            {"total", page.total},
                            {"total_all", page.total_all},
                            {"page", page.page},
                            {"page_size", page.page_size}});
  });

  state.server.Get("/api/devices/options",
                   [&state](const httplib::Request&, httplib::Response& response) {
                     const DeviceOptions values = state.devices->options();
                     set_json(response, json{{"names", values.names},
                                             {"locations", values.locations}});
                   });

  state.server.Post("/api/devices/import",
                    [&state](const httplib::Request& request,
                             httplib::Response& response) {
                      const auto rows = xlsx::read(request.body);
                      const ImportReport report = state.devices->import_rows(rows);
                      set_json(response, json{{"imported", report.imported},
                                              {"skipped_empty", report.skipped_empty},
                                              {"duplicates", report.duplicates},
                                              {"truncated", report.truncated}});
                    });

  // 导入模板：第一个工作表只放表头（与导入校验同一份常量），
  // 第二个工作表放填写说明与假号码示例，避免模板与校验各写一份。
  state.server.Get("/api/devices/template",
                   [](const httplib::Request&, httplib::Response& response) {
                     xlsx::Sheet data;
                     data.name = "设备台账";
                     data.rows.push_back(DeviceService::expected_header());

                     xlsx::Sheet help;
                     help.name = "填写说明";
                     help.rows = {
                         {"列名", "含义与要求"},
                         {"设备名称", "必填；导入时超过 10 个字符会被截断"},
                         {"位置", "必填；导入时超过 10 个字符会被截断"},
                         {"控制范围", "必填；导入时超过 10 个字符会被截断"},
                         {"维护电话", "必填；仅数字、空格、短横线；导入时超过 11 个字符会被截断"},
                         {},
                         {"示例（假号码，请勿直接使用）",
                          "13800000001（卡片上显示为 138 0000 0001）"},
                         {},
                         {"说明",
                          "第一个工作表只有表头，可直接上传，导入 0 行；四列全空的行会跳过，"
                          "重复记录照常导入并提示。"},
                     };

                     response.status = 200;
                     response.set_header(
                         "Content-Type",
                         "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet");
                     response.set_header(
                         "Content-Disposition",
                         "attachment; filename=\"devices-template.xlsx\"; "
                         "filename*=UTF-8''%E8%AE%BE%E5%A4%87%E5%AF%BC%E5%85%A5"
                         "%E6%A8%A1%E6%9D%BF.xlsx");
                     response.set_header("Cache-Control", "no-store");
                     response.body = xlsx::write_workbook({data, help});
                   });

  state.server.Get("/api/devices/export",
                   [&state](const httplib::Request&, httplib::Response& response) {
                     const std::string body =
                         xlsx::write(state.devices->export_rows());
                     response.status = 200;
                     response.set_header(
                         "Content-Type",
                         "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet");
                     response.set_header(
                         "Content-Disposition",
                         "attachment; filename=\"devices.xlsx\"; "
                         "filename*=UTF-8''%E8%AE%BE%E5%A4%87%E5%8F%B0%E8%B4%A6.xlsx");
                     response.set_header("Cache-Control", "no-store");
                     response.body = body;
                   });

  // 打印字体覆盖范围：前端据此在录入与预览时提示「这些字不在打印字体范围内」。
  // 清单来自随包字体 cmap，不是手写表格，换字体会自动跟着变。
  state.server.Get("/api/fonts/coverage",
                   [&state](const httplib::Request&, httplib::Response& response) {
                     json ranges = json::array();
                     for (const auto& range : state.coverage.ranges()) {
                       ranges.push_back({range.first, range.second});
                     }
                     // 字体的字节数与 SHA256 一并下发：前端不再自己写死这两个数，
                     // 否则重新子集后忘了同步，导出会以「字节数不符」永久中止。
                     json fonts = json::array();
                     for (const auto& spec : kEmbeddedFonts) {
                       fonts.push_back({{"name", spec.name},
                                        {"url", spec.url},
                                        {"bytes", spec.bytes},
                                        {"sha256", spec.sha256}});
                     }
                     set_json(response,
                              json{{"available", !state.coverage.empty()},
                                   {"count", state.coverage.count()},
                                   {"ranges", ranges},
                                   {"fonts", fonts}});
                   });

  // 打印数据快照：与台账共用筛选条件，一次取全量（不分页），供前端排版。
  // 前端导出 PDF 与直接打印复用同一份返回，不再二次请求。
  state.server.Get("/api/print/cards", [&state](const httplib::Request& request,
                            httplib::Response& response) {
                     DeviceQuery query = query_from(request);
                     query.page = 0;
                     query.page_size = 1000;
                     query.sort = "id";
                     query.desc = false;

                     std::vector<Device> items;
                     std::int64_t total = 0;
                     bool truncated = false;
                     while (true) {
                       const DevicePage page = state.devices->list(query);
                       if (query.page == 0) total = page.total;
                       items.insert(items.end(), page.items.begin(), page.items.end());
                       if (page.items.empty() ||
                           static_cast<std::int64_t>(items.size()) >= total) {
                         break;
                       }
                       if (items.size() >= 10000) {
                         truncated = true;
                         break;
                       }
                       ++query.page;
                     }

                     const std::string ids_text = request.get_param_value("ids");
                     if (!ids_text.empty()) {
                       std::set<std::int64_t> wanted;
                       std::size_t start = 0;
                       while (start <= ids_text.size()) {
                         const std::size_t comma = ids_text.find(',', start);
                         const std::string part = ids_text.substr(
                             start, comma == std::string::npos
                                        ? std::string::npos
                                        : comma - start);
                         try {
                           if (!part.empty()) wanted.insert(std::stoll(part));
                         } catch (...) {
                           throw AppError("bad_json", "ids 必须是逗号分隔的数字");
                         }
                         if (comma == std::string::npos) break;
                         start = comma + 1;
                       }
                       items.erase(
                           std::remove_if(items.begin(), items.end(),
                                          [&](const Device& device) {
                                            return wanted.count(device.id) == 0;
                                          }),
                           items.end());
                       total = static_cast<std::int64_t>(items.size());
                     }

                     json array = json::array();
                     for (const Device& device : items) {
                       array.push_back(device_json(device));
                     }
                     set_json(response, json{{"items", array},
                                             {"total", total},
                                             {"truncated", truncated},
                                             {"company_name", company_print_name(*state.db)}});
                   });

  state.server.Get(R"(/api/devices/(\d+))",
                   [&state](const httplib::Request& request,
                            httplib::Response& response) {
                     const auto id = std::stoll(request.matches[1].str());
                     set_json(response, device_json(state.devices->get(id)));
                   });

  state.server.Post("/api/devices", [&state](const httplib::Request& request,
                                             httplib::Response& response) {
    Device device = device_from_json(parse_body(request.body));
    set_json(response, device_json(state.devices->create(std::move(device))), 201);
  });

  state.server.Put(R"(/api/devices/(\d+))",
                   [&state](const httplib::Request& request,
                            httplib::Response& response) {
                     const auto id = std::stoll(request.matches[1].str());
                     Device device = device_from_json(parse_body(request.body));
                     set_json(response,
                              device_json(state.devices->update(id, std::move(device))));
                   });

  state.server.Delete(R"(/api/devices/(\d+))",
                      [&state](const httplib::Request& request,
                               httplib::Response& response) {
                        state.devices->remove(std::stoll(request.matches[1].str()));
                        set_json(response, json{{"ok", true}});
                      });

  state.server.Get("/api/users", [&state](const httplib::Request& request,
                                          httplib::Response& response) {
    require_admin(state, request);
    json items = json::array();
    for (const User& user : state.users->list()) items.push_back(user_json(user));
    set_json(response, json{{"items", items}});
  });

  state.server.Post("/api/users", [&state](const httplib::Request& request,
                                           httplib::Response& response) {
    require_admin(state, request);
    const json body = parse_body(request.body);
    const User user = state.users->create(
        body.value("user_name", ""), body.value("password", ""),
        body.value("display_name", ""), body.value("role", "user"));
    set_json(response, json{{"user", user_json(user)}}, 201);
  });

  state.server.Put("/api/users", [&state](const httplib::Request& request,
                                          httplib::Response& response) {
    require_admin(state, request);
    const json body = parse_body(request.body);
    const User user = state.users->update_profile(
        body.value("user_name", ""), body.value("display_name", ""),
        body.value("role", "user"));
    set_json(response, json{{"user", user_json(user)}});
  });

  state.server.Delete("/api/users", [&state](const httplib::Request& request,
                                             httplib::Response& response) {
    const User actor = require_admin(state, request);
    const std::string name = request.get_param_value("name");
    state.users->remove(name, actor.user_name);
    set_json(response, json{{"ok", true}});
  });

  state.server.Post("/api/users/password",
                    [&state](const httplib::Request& request,
                             httplib::Response& response) {
                      const User actor = current_user(state, request);
                      const json body = parse_body(request.body);
                      std::string target = body.value("user_name", "");
                      if (target.empty()) target = actor.user_name;
                      const std::string new_password = body.value("new_password", "");
                      if (new_password.empty()) {
                        throw AppError("bad_json", "new_password 不能为空");
                      }
                      if (target == actor.user_name) {
                        const std::string old_password =
                            body.value("old_password", "");
                        if (old_password.empty()) {
                          throw AppError("old_password_required", "请输入原口令");
                        }
                        state.users->change_password(actor.user_name, old_password,
                                                     new_password);
                      } else {
                        if (actor.role != "admin") {
                          throw AppError("forbidden", "需要管理员权限");
                        }
                        state.users->reset_password(target, new_password);
                      }
                      if (!state.users->any_must_change()) {
                        remove_seed_password_file(state.data_dir);
                      }
                      set_json(response, json{{"ok", true}, {"user_name", target}});
                    });

  // 打印卡片头部公司名：仅管理员可改；空值表示恢复默认。
  state.server.Put("/api/system/company",
                   [&state](const httplib::Request& request,
                            httplib::Response& response) {
                     require_admin(state, request);
                     const json body = json::parse(request.body, nullptr, false);
                     if (body.is_discarded() || !body.is_object()) {
                       throw AppError("bad_json", "请求体必须是 JSON 对象");
                     }
                     const std::string value =
                         trim_copy(body.value("company_name", std::string{}));
                     if (utf8_length(value) > kCompanyNameInputMaxLength) {
                       throw AppError("bad_company_name",
                                      "公司名称过长（最多 60 个字符）");                     }
                     state.db->write([&](sqlite3* conn) {
                       set_setting(conn, "print_company_name", value);
                     });
                     set_json(response,
                              json{{"company_name", company_name(*state.db)},
                                   {"company_name_print",
                                    company_print_name(*state.db)},
                                   {"company_name_width",
                                    company_name_width(company_name(*state.db))},
                                   {"company_name_max_width", kCompanyNameMaxWidth},
                                   {"is_default",
                                    get_setting(*state.db, "print_company_name")
                                        .empty()}});
                   });

  // 自定义图标：只收 PNG/JPEG，文件落在数据目录 assets/ 下，app_setting 只存相对路径。
  state.server.Post("/api/system/company-logo",
                    [&state](const httplib::Request& request,
                             httplib::Response& response) {
                      require_admin(state, request);
                      constexpr std::size_t kMaxBytes = 2 * 1024 * 1024;
                      if (request.body.empty()) {
                        throw AppError("bad_logo", "请选择要上传的图片");
                      }
                      if (request.body.size() > kMaxBytes) {
                        throw AppError("logo_too_large", "图标不能超过 2 MB");
                      }
                      const ImageInfo info = inspect_image(request.body);
                      if (info.format == ImageFormat::kUnknown) {
                        throw AppError("bad_logo",
                                       "只接受 PNG 或 JPEG 图片（不支持 SVG）");
                      }
                      const double ratio =
                          static_cast<double>(info.width) / info.height;
                      const bool aspect_warning =
                          ratio < kLogoMinAspect || ratio > kLogoMaxAspect;

                      const bool png = info.format == ImageFormat::kPng;
                      const std::string relative =
                          std::string("assets/company-logo.") + (png ? "png" : "jpg");
                      std::error_code ec;
                      std::filesystem::create_directories(state.data_dir / "assets",
                                                          ec);
                      {
                        std::ofstream out(state.data_dir / relative,
                                          std::ios::binary | std::ios::trunc);
                        if (!out) {
                          throw AppError("logo_write",
                                         "图标写入失败，请检查数据目录权限");
                        }
                        out.write(request.body.data(),
                                  static_cast<std::streamsize>(request.body.size()));
                      }
                      // 换格式时清掉旧文件，避免两份并存。
                      std::filesystem::remove(
                          state.data_dir /
                              (std::string("assets/company-logo.") +
                               (png ? "jpg" : "png")),
                          ec);
                      state.db->write([&](sqlite3* conn) {
                        set_setting(conn, kLogoSettingKey, relative);
                        set_setting(conn, "company_logo_updated_at", now_text());
                      });

                      const bool low_resolution =
                          info.width < kLogoMinPixels ||
                          info.height < kLogoMinPixels;
                      std::string message = "图标已更新";
                      if (low_resolution) {
                        message += "；分辨率低于 104×104 像素，300 dpi 打印可能发虚";
                      }
                      if (aspect_warning) {
                        message +=
                            "；图片偏扁或偏窄，缩放后会显得很小，建议换成接近方形的图";
                      }
                      set_json(response,
                               json{{"logo_url", "/company-logo"},
                                    {"format", png ? "png" : "jpeg"},
                                    {"width", info.width},
                                    {"height", info.height},
                                    {"low_resolution", low_resolution},
                                    {"aspect_warning", aspect_warning},
                                    {"updated_at",
                                     get_setting(*state.db,
                                                 "company_logo_updated_at")},
                                    {"message", message}});
                    });

  state.server.Delete("/api/system/company-logo",
                      [&state](const httplib::Request& request,
                               httplib::Response& response) {
                        require_admin(state, request);
                        state.db->write([&](sqlite3* conn) {
                          set_setting(conn, kLogoSettingKey, "");
                          set_setting(conn, "company_logo_updated_at", "");
                        });
                        std::error_code ec;
                        std::filesystem::remove(
                            state.data_dir / "assets/company-logo.png", ec);
                        std::filesystem::remove(
                            state.data_dir / "assets/company-logo.jpg", ec);
                        set_json(response, json{{"logo_url", "/company-logo"},
                                                {"is_default", true},
                                                {"updated_at", ""}});
                      });

  state.server.Get("/api/system/backups",
                   [&state](const httplib::Request& request,
                            httplib::Response& response) {
                     require_admin(state, request);
                     json items = json::array();
                     for (const BackupEntry& entry : list_backups(state.data_dir)) {
                       items.push_back(json{{"name", entry.name},
                                            {"bytes", entry.bytes},
                                            {"created_at", entry.created_at}});
                     }
                     set_json(response, json{{"items", items}});
                   });

  state.server.Post("/api/system/backup",
                    [&state](const httplib::Request& request,
                             httplib::Response& response) {
                      require_admin(state, request);
                      const BackupEntry entry =
                          create_backup(*state.db, state.data_dir, "manual");
                      set_json(response, json{{"name", entry.name},
                                              {"bytes", entry.bytes},
                                              {"created_at", entry.created_at}}, 201);
                    });

  state.server.Post("/api/system/restore",
                    [&state](const httplib::Request& request,
                             httplib::Response& response) {
                      require_admin(state, request);
                      const json body = parse_body(request.body);
                      if (body.value("confirm", "") != "RESTORE") {
                        throw AppError("bad_json", "恢复操作需要 confirm=RESTORE");
                      }
                      restore_backup(*state.db, state.data_dir,
                                     body.value("name", ""));
                      {
                        std::lock_guard<std::mutex> lock(state.sessions_mutex);
                        state.sessions.clear();
                      }
                      clear_session_cookie(response);
                      set_json(response, json{{"ok", true}, {"reload", true}});
                    });

  state.server.Get("/", [&state](const httplib::Request&, httplib::Response& response) {
    std::string html = embedded_index_html();
    const std::string meta = "<meta name=\"devcard-local-token\" content=\"" +
                             state.local_token + "\">";
    const std::size_t head = html.find("</head>");
    if (head != std::string::npos) html.insert(head, meta + "\n");
    set_asset(response, html, "text/html; charset=utf-8");
  });
  state.server.Get("/styles.css", [](const httplib::Request&, httplib::Response& response) {
    set_asset(response, embedded_styles_css(), "text/css; charset=utf-8");
  });
  state.server.Get("/app.js", [](const httplib::Request&, httplib::Response& response) {
    set_asset(response, embedded_app_js(), "application/javascript; charset=utf-8");
  });
  // 子集字体：@font-face 从这里取，界面与打印共用同一份。
  // 只认白名单里的两个文件名，字体目录不整体开放成静态目录。
  state.server.Get(R"(/fonts/([A-Za-z0-9._-]+))",
                   [&state](const httplib::Request& request,
                            httplib::Response& response) {
                     const std::string name = request.matches[1].str();
                     const auto it = state.fonts.find(name);
                     if (it == state.fonts.end()) {
                       set_json(response,
                                json{{"error", "not_found"},
                                     {"message", "字体不存在：" + name}},
                                404);
                       return;
                     }
                     response.status = 200;
                     response.set_header("Content-Type", "font/ttf");
                     // 导出前的门禁资源：它必须在每次导出时被真正取到并校验，
                     // 缓存一天等于把「取不到就中止」这条判据拆掉。本地回环传 4.56 MB 是几毫秒。
                     response.set_header("Cache-Control", "no-store");
                     response.body = it->second;
                   });
  // 打印图标：优先数据目录下的自定义文件，缺失时回落内嵌默认 logo。
  state.server.Get("/company-logo",
                   [&state](const httplib::Request&, httplib::Response& response) {
                     const std::string relative =
                         get_setting(*state.db, kLogoSettingKey);
                     if (safe_logo_relative(relative)) {
                       const std::filesystem::path path = state.data_dir / relative;
                       std::string bytes = read_file_bytes(path);
                       if (!bytes.empty()) {
                         const bool png = path.extension() == ".png";
                         response.status = 200;
                         response.set_header("Content-Type",
                                             png ? "image/png" : "image/jpeg");
                         response.set_header("Cache-Control", "no-store");
                         response.body = std::move(bytes);
                         return;
                       }
                     }
                     response.status = 200;
                     response.set_header("Content-Type", "image/jpeg");
                     response.set_header("Cache-Control", "no-store");
                     response.body = logo_jpeg();
                   });
  state.server.Get("/logo.jpg", [](const httplib::Request&, httplib::Response& response) {
    response.status = 200;
    response.set_header("Content-Type", "image/jpeg");
    response.set_header("Cache-Control", "public, max-age=86400");
    response.body = logo_jpeg();
  });

  // 认证与 CSRF 兜底：/api/health 与 /api/session/* 免认证。
  state.server.set_pre_routing_handler(
      [&state](const httplib::Request& request, httplib::Response& response) {
        if (!host_allowed(request.get_header_value("Host"))) {
          set_json(response,
                   json{{"error", "bad_host"},
                        {"message", "Host 头必须是 127.0.0.1 或 localhost"}},
                   421);
          return httplib::Server::HandlerResponse::Handled;
        }
        if (request.path.rfind("/api/", 0) != 0) {
          return httplib::Server::HandlerResponse::Unhandled;
        }
        if (request.path == "/api/health" ||
            request.path.rfind("/api/session/", 0) == 0) {
          return httplib::Server::HandlerResponse::Unhandled;
        }
        if (request.method != "GET" &&
            request.get_header_value("X-Requested-With") != "DevCard") {
          set_json(response,
                   json{{"error", "csrf"},
                        {"message", "缺少 X-Requested-With: DevCard"}},
                   403);
          return httplib::Server::HandlerResponse::Handled;
        }
        const std::string name = session_user(state, request);
        if (name.empty()) {
          set_json(response,
                   json{{"error", "unauthorized"}, {"message", "未登录或会话已过期"}},
                   401);
          return httplib::Server::HandlerResponse::Handled;
        }
        User user;
        try {
          user = state.users->get(name);
        } catch (const AppError&) {
          set_json(response,
                   json{{"error", "unauthorized"}, {"message", "账号不存在"}}, 401);
          return httplib::Server::HandlerResponse::Handled;
        }
        const bool password_paths =
            request.path == "/api/bootstrap" ||
            request.path == "/api/users/password";
        if (user.must_change_password && !password_paths) {
          set_json(response,
                   json{{"error", "must_change_password"},
                        {"message", "首次登录必须修改口令"}},
                   403);
          return httplib::Server::HandlerResponse::Handled;
        }
        return httplib::Server::HandlerResponse::Unhandled;
      });

  state.server.set_post_routing_handler(
      [](const httplib::Request&, httplib::Response& response) {
        response.set_header("X-Content-Type-Options", "nosniff");
        response.set_header("Referrer-Policy", "no-referrer");
        if (response.get_header_value("Content-Type").rfind("text/html", 0) == 0) {
          // connect-src 必须显式放行：更新检查是页面侧 fetch 跑在壳里，
          // 而 default-src 'self' 会把跨域连通性一并禁掉。WebKit 对 CSP 拦截
          // 只报 "Load failed"，看起来像 DNS/TLS 故障，实际请求根本没发出去。
          // 检查地址可由运维配置（含内网 http 清单），所以按 scheme 放行而不写死域名；
          // 脚本仍然只允许 'self'，外部页面注入不进来。
          response.set_header(
              "Content-Security-Policy",
              "default-src 'self'; connect-src 'self' https: http:; "
              "img-src 'self' data:; style-src 'self'");
        }
      });

  state.server.set_error_handler(
      [](const httplib::Request&, httplib::Response& response) {
        if (response.status == 404 && response.body.empty()) {
          set_json(response, json{{"error", "not_found"}, {"message", "接口不存在"}},
                   404);
        }
      });

  state.server.set_exception_handler([](const httplib::Request&,
                                        httplib::Response& response,
                                        std::exception_ptr error) {
    try {
      std::rethrow_exception(error);
    } catch (const AppError& app_error) {
      set_json(response,
               json{{"error", app_error.code()}, {"message", app_error.what()}},
               status_for(app_error));
    } catch (const std::exception& other) {
      set_json(response, json{{"error", "internal"}, {"message", other.what()}}, 500);
    } catch (...) {
      set_json(response, json{{"error", "internal"}, {"message", "未知错误"}}, 500);
    }
  });

  if (!state.server.bind_to_port(options.host, options.port)) {
    throw AppError("port_in_use",
                   "无法监听 " + options.host + ":" + std::to_string(options.port));
  }
  handle->port_ = options.port;
  state.thread = std::thread([&state] { state.server.listen_after_bind(); });
  return handle;
}

}  // namespace devcard
