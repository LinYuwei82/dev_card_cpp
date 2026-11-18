#include "devcard/server.h"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "devcard/sha256.h"
#include "font_manifest.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdint>
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
#include "devcard/audit.h"
#include "devcard/db.h"
#include "devcard/error.h"
#include "devcard/image_meta.h"
#include "devcard/migrations.h"
#include "devcard/models.h"
#include "devcard/paths.h"
#include "devcard/service/device_service.h"
#include "devcard/service/user_service.h"
#include "devcard/settings.h"
#include "devcard/update_manifest.h"
#include "devcard/update_package.h"
#include "devcard/xlsx.h"
#include "apply_update.h"
#include "config.h"
#include "heartbeat.h"
#include "instance_lock.h"
#include "web_assets.h"
#include "font_coverage.h"

#ifdef _WIN32
#include <shellapi.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace devcard {
namespace {

using nlohmann::json;

constexpr auto kSessionTtlSeconds = 12 * 3600;  // 12 小时，持久化用秒数方便序列化

// 登录/改密失败计数的单个键（来源 IP 或用户名）。字段用 atomic，
// 计数与判定另由 ServerState::rate_limit_mutex 串成一次操作。
struct RateLimitEntry {
  std::atomic<int> failures{0};
  std::atomic<long long> window_start{0};  // steady 秒；0 表示窗口未开始
  std::atomic<long long> locked_until{0};  // steady 秒
};

long long steady_seconds() {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

struct Session {
  std::string user;
  std::int64_t last_seen_epoch;  // Unix epoch 秒，方便 JSON 持久化
};

struct ServerState {
  httplib::Server server;
  std::thread thread;
  std::unique_ptr<Db> db;
  std::unique_ptr<DeviceService> devices;
  std::unique_ptr<UserService> users;
  std::unique_ptr<InstanceGuard> instance_guard;
  std::filesystem::path data_dir;
  std::filesystem::path font_dir;
  // 开发态前端目录（--web-dir）：非空时静态资源直读磁盘，跳过内嵌自检。
  std::filesystem::path web_dir;
  std::map<std::string, std::string> fonts;
  FontCoverage coverage;
  std::string local_token;
  bool seeded = false;
  // 库内心跳（多实例第 3 层）：启动时抢占一行，运行期定时刷新。
  std::string instance_id;
  std::thread heartbeat_thread;
  std::atomic<bool> heartbeat_stop{false};
  // 网络模式（--server）：允许内网来源；本地窗口模式只接受回环。
  bool network = false;
  std::string bound_host;
  std::string machine_name;
  std::vector<std::string> allowed_hosts;

  std::mutex sessions_mutex;
  std::map<std::string, Session> sessions;

  // 登录/改密失败限流：内存实现，进程重启清零（不落库）。
  // 双键（来源 IP / 用户名）各自独立计数，任一超限即拒。
  RateLimitOptions rate_limit;
  std::mutex rate_limit_mutex;
  std::map<std::string, RateLimitEntry> rate_limit_by_ip;
  std::map<std::string, RateLimitEntry> rate_limit_by_user;

  // 自更新阶段一：同一时间只允许一个下载任务；状态由后台线程更新。
  struct UpdateDownload {
    std::mutex mutex;
    std::string state = "idle";  // idle / running / done / failed
    bool installing = false;
    std::uint64_t total = 0;
    std::uint64_t received = 0;
    std::filesystem::path part_path;
    std::string path;
    std::string sha256;
    std::string code;
    std::string error;
  };
  std::shared_ptr<UpdateDownload> update = std::make_shared<UpdateDownload>();

  // 自更新阶段二：安装接口回完 202 后置位，main 轮询到就退出，交由安装器接管。
  // 用 shared_ptr 是因为置位发生在 detached 线程里，不能碰已析构的 ServerState。
  std::shared_ptr<std::atomic<bool>> stop_requested =
      std::make_shared<std::atomic<bool>>(false);
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
          {"updated_at", device.updated_at},
          {"version", device.version},
          {"updated_by", device.updated_by}};
}

// 409 响应体：最新快照 + 冲突字段 + 版本号。
// 前端据此提示「刷新后重填」；P3 若以后开放管理员强制覆盖，
// 直接复用这里的 current/version 即可，不需要再改接口形状。
json conflict_json(const ConflictError& error) {
  const ConflictSnapshot& snapshot = error.snapshot();
  return {{"error", "conflict"},
          {"message", error.what()},
          {"fields", snapshot.fields},
          {"current", device_json(snapshot.current)},
          {"current_version", snapshot.current.version},
          {"updated_by", snapshot.current.updated_by},
          {"updated_at", snapshot.current.updated_at}};
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
  device.version = body.value("version", static_cast<std::int64_t>(0));
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
// v2 起统一为带 Ed25519 签名的 manifest.json（两平台同一份，只取本平台段）；
// 远端检查地址随之内嵌，间隔走默认配置——清单里不再有 check_* 字段。
constexpr std::string_view kUpdateManifestName = "manifest.json";
constexpr std::string_view kDefaultDownloadPage =
    "https://dl.xiaotu.online/";
// 清单 Ed25519 公钥（32 字节的 hex），与 key_id（2026-11）一起随客户端发布；
// 轮换密钥 = 新 key_id + 新公钥随下一个客户端版本编译进来。
constexpr std::string_view kUpdateSignPubkeyHex =
    "3f4d9fabc122113e426b98a8f8b2312cb194e406f770de61285e41c607d22403";
constexpr std::string_view kDefaultUpdateEndpoint =
    "https://dl.xiaotu.online/devcard/updates/stable/manifest.json";
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
  // 验签通过后本平台（Windows / macOS）的版本与构建号；验签失败时保持为空。
  std::string latest;
  std::uint64_t latest_build = 0;  // Windows 构建号；macOS 恒 0
  // v2 清单没有 download_url/notes 字段；保留输出结构，前端有 download_page 兜底。
  std::string download_url;
  std::string notes;
  std::string error;
  // v2 artifacts 转成 v1 资产表的形状（browser_download_url + digest
  // "sha256:<hex>"）原样透传前端：前端的资产解析与按钮逻辑不用动。
  // 命中本地清单时前端不会再拉远端，没有这份资产就无法判断
  // 「能不能下载 / 能不能安装」，按钮只能全灰。
  json assets = json::array();
  // M6：远端检查的开关、地址与频率。地址内嵌、间隔用默认值；
  // 清单验签失败时整体关闭检查——清单可能被篡改，宁可查不了也不冒进。
  bool check_enabled = true;
  std::string check_endpoint{std::string(kDefaultUpdateEndpoint)};
  long check_interval_hours = kDefaultUpdateIntervalHours;
};

UpdateManifest read_update_manifest(const std::filesystem::path& data_dir) {
  UpdateManifest result;
  std::ifstream in(data_dir / kUpdateManifestName, std::ios::binary);
  if (!in) return result;
  result.found = true;
  const std::string text((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());
  // v2：先验 Ed25519 签名，再解析。签名不过就放弃更新（不提示版本、
  // 不透传资产、关闭远端检查）——这是「先验清单签名（失败放弃更新），
  // 再校验产物 SHA256」链路的第一道闸。
  const SignedManifest parsed = parse_signed_manifest(text, kUpdateSignPubkeyHex);
  if (!parsed.ok) {
    result.error = parsed.error;
    result.check_enabled = false;
    return result;
  }
  result.latest = parsed.platform.version;
  result.latest_build = parsed.platform.build;
  for (const ManifestArtifact& artifact : parsed.platform.artifacts) {
    result.assets.push_back(json{
        {"name", artifact.name},
        {"browser_download_url", artifact.url},
        {"digest", "sha256:" + artifact.sha256},
        {"size", artifact.size},
        {"version", result.latest},
    });
  }
  std::vector<long> parts;
  if (result.latest.empty()) {
    result.error = "更新清单缺少本平台条目";
  } else if (!parse_version(result.latest, &parts)) {
    result.error = "更新清单中的版本号不合法";
    result.latest.clear();
    result.assets = json::array();
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

std::string hex_to_bytes(std::string_view hex) {
  std::string out;
  out.reserve(hex.size() / 2);
  for (std::size_t i = 0; i + 1 < hex.size(); i += 2) {
    out.push_back(
        static_cast<char>((hex_digit(hex[i]) << 4) | hex_digit(hex[i + 1])));
  }
  return out;
}

const std::string& logo_jpeg() {
  static const std::string bytes = hex_to_bytes(embedded_logo_jpg_hex());
  return bytes;
}

const std::string& favicon_ico() {
  static const std::string bytes = hex_to_bytes(embedded_favicon_ico_hex());
  return bytes;
}

namespace {

using StatusTable = std::map<std::string, int, std::less<>>;

const StatusTable& error_status_table() {
  // 单张表：错误码 → HTTP。新增码只加一行，不再套 if 链。
  // 凡是路由会抛的类型都必须在这里，缺一条就会把 4xx 渲染成 500。
  static const StatusTable kStatus = {
      {"duplicate", 409},
      {"last_admin", 409},
      {"self_delete", 409},
      {"no_user", 409},
      {"not_found", 404},
      {"legacy_db", 409},
      {"invalid_credentials", 401},
      {"unauthorized", 401},
      // 登录/改密失败限流：单独一项，不混进 401/403——客户端要能按 429
      // 读 Retry-After 决定等待，而不是把「密码错了」和「先别试了」混为一谈。
      {"too_many_requests", 429},
      {"locked", 423},
      {"forbidden", 403},
      {"must_change_password", 403},
      {"bad_request", 400},
      {"bad_version", 400},
      // 更新包落点必须是绝对路径：用户选了相对路径属于请求错误。
      {"invalid_path", 400},
      {"invalid_device", 400},
      {"invalid_user", 400},
      {"weak_password", 400},
      {"bad_field", 400},
      {"bad_json", 400},
      {"bad_header", 400},
      {"bad_xlsx", 400},
      {"bad_backup", 400},
      // 备份文件本身打不开：是用户选错了文件，不是服务端故障。
      {"backup_invalid", 400},
      {"old_password_required", 400},
      {"bad_logo", 400},
      {"logo_too_large", 400},
      {"bad_company_name", 400},
      {"logo_write", 400},
      // 更新（macOS 阶段一）
      {"endpoint_not_allowed", 400},
      {"no_update", 400},
      {"update_disabled", 403},
      {"update_busy", 409},
      // 下载 spec 与验签清单对不上：不是客户端的锅也不是远端的锅，
      // 400 让前端把「重新检查更新」的提示亮出来。
      {"not_in_manifest", 400},
      {"manifest_invalid", 502},
      {"download_failed", 502},
      {"checksum_mismatch", 502},
      // 自更新阶段二（仅 Windows 安装版）
      {"update_package_missing", 400},
      {"install_package_invalid", 400},
      {"install_busy", 409},
      {"install_failed", 500},
      {"install_unsupported", 501},
      // 路由可达，但属于服务端 / 磁盘故障：显式写出来，别靠「没登记就 500」兜着。
      {"backup_failed", 500},
      {"io_error", 500},
      {"db_error", 500},
      // core/src 里会抛、且能经路由走到的码：不登记就会落到 500 兜底，
      // 前端拿到的 error 字段会从具体码变成空，排障时看不出是哪一层。
      {"db_exec", 500},
      {"db_unexpected_row", 500},
      {"password_hash", 500},
      {"xlsx_write_failed", 500},
  };
  return kStatus;
}

}  // namespace

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

constexpr const char* kSessionsFileName = "sessions.json";

void persist_sessions(ServerState& state) {
  // 调用者必须已持 sessions_mutex。
  const auto path = state.data_dir / kSessionsFileName;
  json doc = json::array();
  const auto now = static_cast<std::int64_t>(std::time(nullptr));
  for (const auto& [token, sess] : state.sessions) {
    if (now - sess.last_seen_epoch > kSessionTtlSeconds) continue;  // 过期不落
    doc.push_back({{"token", token},
                   {"user", sess.user},
                   {"last_seen", sess.last_seen_epoch}});
  }
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  // 先写临时文件再替换：直接截断写，进程在写一半时被杀会留下半截 JSON，
  // 下次启动 load_sessions 只能整体丢弃（等于所有人被登出）。
  const auto temp = path;
  const auto staging = path.parent_path() / (path.filename().wstring() + L".tmp");
  {
    std::ofstream out(staging, std::ios::binary | std::ios::trunc);
    if (!out) return;
    out << doc.dump();
  }
  std::filesystem::rename(staging, temp, ec);
  if (ec) {
    std::filesystem::remove(temp, ec);
    std::filesystem::rename(staging, temp, ec);
    if (ec) std::filesystem::remove(staging, ec);
  }
}

void load_sessions(ServerState& state) {
  const auto path = state.data_dir / kSessionsFileName;
  std::ifstream in(path, std::ios::binary);
  if (!in) return;  // 首次启动：没有文件，空 session map
  const std::string text((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());
  const json doc = json::parse(text, nullptr, false);
  if (!doc.is_array()) return;
  const auto now = static_cast<std::int64_t>(std::time(nullptr));
  std::lock_guard<std::mutex> lock(state.sessions_mutex);
  for (const auto& entry : doc) {
    if (!entry.is_object()) continue;
    const std::string token = entry.value("token", std::string{});
    const std::string user = entry.value("user", std::string{});
    const auto last_seen = entry.value("last_seen", static_cast<std::int64_t>(0));
    if (token.empty() || user.empty() || last_seen <= 0) continue;
    if (now - last_seen > kSessionTtlSeconds) continue;
    state.sessions[token] = Session{user, last_seen};
  }
}

std::string session_user(ServerState& state, const httplib::Request& request) {
  const std::string token = cookie_value(request, "sid");
  if (token.empty()) return {};
  std::lock_guard<std::mutex> lock(state.sessions_mutex);
  auto it = state.sessions.find(token);
  if (it == state.sessions.end()) return {};
  const auto now_epoch = static_cast<std::int64_t>(std::time(nullptr));
  if (now_epoch - it->second.last_seen_epoch > kSessionTtlSeconds) {
    state.sessions.erase(it);
    return {};
  }
  it->second.last_seen_epoch = now_epoch;
  return it->second.user;
}

std::string create_session(ServerState& state, const std::string& user) {
  const std::string token = random_token();
  const auto now = static_cast<std::int64_t>(std::time(nullptr));
  std::lock_guard<std::mutex> lock(state.sessions_mutex);
  state.sessions[token] = Session{user, now};
  persist_sessions(state);
  return token;
}

void destroy_session(ServerState& state, const httplib::Request& request) {
  const std::string token = cookie_value(request, "sid");
  if (token.empty()) return;
  std::lock_guard<std::mutex> lock(state.sessions_mutex);
  state.sessions.erase(token);
  persist_sessions(state);
}

void drop_sessions_for(ServerState& state, const std::string& user,
                       const std::string& keep_token = {}) {
  bool changed = false;
  std::lock_guard<std::mutex> lock(state.sessions_mutex);
  for (auto it = state.sessions.begin(); it != state.sessions.end();) {
    if (it->second.user == user && it->first != keep_token) {
      it = state.sessions.erase(it);
      changed = true;
    } else {
      ++it;
    }
  }
  if (changed) persist_sessions(state);
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

struct RateLimitDecision {
  bool locked = false;
  int retry_after = 0;
};

// 回环地址不计：本机界面与壳的自动登录不该被自己的失败计数锁在门外。
bool rate_limit_exempt(const std::string& address) {
  return is_loopback(address);
}

void rate_limit_touch(std::map<std::string, RateLimitEntry>& table,
                      const std::string& key, const RateLimitOptions& options) {
  if (key.empty()) return;
  RateLimitEntry& entry = table[key];
  const long long now = steady_seconds();
  const long long start = entry.window_start.load();
  // 窗口过期就重新计数：5 次必须落在同一个窗口内才累计。
  if (start == 0 || now - start >= options.window_seconds) {
    entry.window_start.store(now);
    entry.failures.store(0);
  }
  const int failures = entry.failures.fetch_add(1) + 1;
  if (failures >= options.max_failures) {
    entry.locked_until.store(now + options.lock_seconds);
    entry.failures.store(0);
    entry.window_start.store(now);
  }
}

// 键空间只增不减会随来源 IP 数量无限涨；超过上限时清掉已解锁且窗口过期的项。
void rate_limit_prune(std::map<std::string, RateLimitEntry>& table,
                      const RateLimitOptions& options) {
  if (table.size() <= 1024) return;
  const long long now = steady_seconds();
  for (auto item = table.begin(); item != table.end();) {
    const long long start = item->second.window_start.load();
    const bool unlocked = item->second.locked_until.load() <= now;
    const bool expired = start == 0 || now - start >= options.window_seconds;
    if (unlocked && expired) {
      item = table.erase(item);
    } else {
      ++item;
    }
  }
}

RateLimitDecision rate_limit_check(ServerState& state, const std::string& address,
                                   const std::string& user) {
  RateLimitDecision decision;
  // 本机地址整体不受限：壳与本地界面不能被自己的失败计数锁在门外。
  if (!state.rate_limit.enabled || rate_limit_exempt(address)) return decision;
  const long long now = steady_seconds();
  const auto inspect = [&](const std::map<std::string, RateLimitEntry>& table,
                           const std::string& key) {
    if (key.empty()) return;
    const auto found = table.find(key);
    if (found == table.end()) return;
    const long long until = found->second.locked_until.load();
    if (until <= now) return;
    decision.locked = true;
    const int wait = static_cast<int>(until - now);
    if (wait > decision.retry_after) decision.retry_after = wait;
  };
  std::lock_guard<std::mutex> lock(state.rate_limit_mutex);
  if (!rate_limit_exempt(address)) inspect(state.rate_limit_by_ip, address);
  inspect(state.rate_limit_by_user, user);
  return decision;
}

void rate_limit_record_failure(ServerState& state, const std::string& address,
                               const std::string& user) {
  if (!state.rate_limit.enabled || rate_limit_exempt(address)) return;
  std::lock_guard<std::mutex> lock(state.rate_limit_mutex);
  rate_limit_touch(state.rate_limit_by_ip, address, state.rate_limit);
  rate_limit_touch(state.rate_limit_by_user, user, state.rate_limit);
  rate_limit_prune(state.rate_limit_by_ip, state.rate_limit);
  rate_limit_prune(state.rate_limit_by_user, state.rate_limit);
}

void rate_limit_clear(ServerState& state, const std::string& address,
                      const std::string& user) {
  if (!state.rate_limit.enabled || rate_limit_exempt(address)) return;
  std::lock_guard<std::mutex> lock(state.rate_limit_mutex);
  if (!address.empty()) state.rate_limit_by_ip.erase(address);
  if (!user.empty()) state.rate_limit_by_user.erase(user);
}

// 锁定期间一律 429，连密码正确也不放行；Retry-After 由响应头带出。
void throw_rate_limited(httplib::Response& response,
                        const RateLimitDecision& decision) {
  const int retry_after = decision.retry_after > 0 ? decision.retry_after : 1;
  response.set_header("Retry-After", std::to_string(retry_after));
  throw AppError("too_many_requests",
                 "失败次数过多，请 " + std::to_string(retry_after) + " 秒后重试");
}

std::string lowercase(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

// Host 白名单：本地窗口模式只认回环；网络模式再加入实际监听 IP、机器名
// 与配置的 allowed_hosts，其他 Host 返回 421，用于防 DNS rebinding。
bool host_allowed(const ServerState& state, std::string host) {
  if (host.empty()) return false;
  std::string name = lowercase(host);
  if (!name.empty() && name.front() == '[') {
    const std::size_t end = name.find(']');
    if (end != std::string::npos) name = name.substr(1, end - 1);
  } else {
    const std::size_t colon = name.find(':');
    if (colon != std::string::npos) name = name.substr(0, colon);
  }
  if (name == "127.0.0.1" || name == "localhost" || name == "::1") return true;
  if (!state.network) return false;
  if (!state.bound_host.empty() && name == lowercase(state.bound_host)) return true;
  if (!state.machine_name.empty() && name == lowercase(state.machine_name)) {
    return true;
  }
  for (const std::string& allowed : state.allowed_hosts) {
    if (name == lowercase(allowed)) return true;
  }
  return false;
}

// 免登录/免改密白名单：只允许下面两类，集中在这张表里判断。
// 更新检查与下载故意不在表内：网络模式下若未认证请求就能让服务端去拉
// 几十 MB 落盘，等于免费的内容缓存 + 磁盘写放大器；所以「检查个版本」
// 同样要求登录，强制改密前也不放行。以后不要往这里加更新相关路径。
constexpr const char* kBypassExactPaths[] = {"/api/health"};
constexpr const char* kBypassPrefixes[] = {"/api/session/"};

bool is_bypass_path(const std::string& path) {
  for (const char* exact : kBypassExactPaths) {
    if (path == exact) return true;
  }
  for (const char* prefix : kBypassPrefixes) {
    if (path.rfind(prefix, 0) == 0) return true;
  }
  return false;
}

unsigned long process_id() {
#ifdef _WIN32
  return GetCurrentProcessId();
#else
  return static_cast<unsigned long>(getpid());
#endif
}

AuditContext audit_context(ServerState& state, const httplib::Request& request) {
  return AuditContext{session_user(state, request), request.remote_addr};
}

// 独立于业务事务的审计（登录、恢复等）：写失败只告警，不影响主流程，
// 但不能静默——静默失败等于审计形同虚设。
void audit_event(ServerState& state, AuditEntry entry) {
  if (entry.actor.empty()) entry.actor = "system";
  try {
    state.db->write([&](sqlite3* conn) { record_audit(conn, entry); });
  } catch (const std::exception& error) {
    std::cerr << "警告：审计写入失败：" << error.what() << std::endl;
  }
}

User current_user(ServerState& state, const httplib::Request& request) {
  const std::string name = session_user(state, request);
  if (name.empty()) throw AppError("unauthorized", "未登录或会话已过期");
  try {
    return state.users->get(name);
  } catch (const AppError& error) {
    // 账号已被删除、但会话还没到期：按未登录处理，别把 not_found 当 404 透给前端。
    if (error.code() == "not_found") {
      throw AppError("unauthorized", "账号已不存在，请重新登录");
    }
    throw;
  }
}

User require_admin(ServerState& state, const httplib::Request& request) {
  User user = current_user(state, request);
  if (user.role != "admin") throw AppError("forbidden", "需要管理员权限");
  return user;
}

// 路径里的 id 只由正则保证是数字，长度不受限：std::stoll 会抛 out_of_range，
// 落到 std::exception 分支就成了 500，还会把 "stoll" 原样透给客户端。
std::int64_t parse_path_id(const std::string& text) {
  std::size_t consumed = 0;
  std::int64_t value = 0;
  try {
    value = std::stoll(text, &consumed);
  } catch (const std::exception&) {
    throw AppError("not_found", "记录不存在");
  }
  if (consumed != text.size()) {
    throw AppError("not_found", "记录不存在");
  }
  return value;
}

}  // namespace

int http_status_for(std::string_view code) {
  const StatusTable& table = error_status_table();
  const auto found = table.find(code);
  return found == table.end() ? 500 : found->second;
}

bool http_status_known(std::string_view code) {
  const StatusTable& table = error_status_table();
  return table.find(code) != table.end();
}

const std::vector<std::string>& route_error_codes() {
  // 与各路由里 throw AppError(...) 的 code 对应。新增路由错误时，这里和
  // error_status_table 要一起补，否则 tests 里的「清单覆盖」用例会红。
  static const std::vector<std::string> kCodes = {
      "duplicate",        "last_admin",       "self_delete",
      "no_user",          "not_found",        "legacy_db",
      "invalid_credentials", "unauthorized",  "locked",
      "too_many_requests",
      "forbidden",        "must_change_password",
      "invalid_device",   "invalid_user",     "weak_password",
      "bad_field",        "bad_json",         "bad_header",
      "bad_xlsx",         "bad_backup",
      "backup_invalid",   "old_password_required",
      "bad_logo",         "logo_too_large",   "bad_company_name",
      "logo_write",       "endpoint_not_allowed",
      "no_update",        "update_disabled",  "update_busy",
      "not_in_manifest",
      "manifest_invalid", "download_failed",  "checksum_mismatch",
      "update_package_missing", "install_package_invalid",
      "install_busy",     "install_failed",   "install_unsupported",
      "backup_failed",    "io_error",         "db_error",
      "db_exec",          "db_unexpected_row", "password_hash",
      "xlsx_write_failed", "bad_version",
      "invalid_path",
      "bad_request",
  };
  return kCodes;
}

int status_for(const AppError& error) { return http_status_for(error.code()); }

const std::vector<std::string>& mapped_error_codes() {
  static const std::vector<std::string> kCodes = [] {
    std::vector<std::string> codes;
    for (const auto& item : error_status_table()) codes.push_back(item.first);
    return codes;
  }();
  return kCodes;
}

struct ServerHandle::Impl : ServerState {};

ServerHandle::ServerHandle() : impl_(std::make_unique<Impl>()) {}
ServerHandle::~ServerHandle() {
  if (!impl_) return;
  impl_->heartbeat_stop = true;
  if (impl_->heartbeat_thread.joinable()) impl_->heartbeat_thread.join();
  if (impl_->server.is_running()) impl_->server.stop();
  if (impl_->thread.joinable()) impl_->thread.join();
  // 正常析构时删掉自己那一行；异常/强杀留下的行由下次启动按 PID/超时接管。
  if (impl_->db && !impl_->instance_id.empty()) {
    try {
      release_heartbeat(*impl_->db, impl_->instance_id);
    } catch (const std::exception& error) {
      std::cerr << "警告：释放库内心跳失败：" << error.what() << std::endl;
    }
  }
}

const std::string& ServerHandle::local_token() const { return impl_->local_token; }

bool ServerHandle::stop_requested() const {
  return impl_ && impl_->stop_requested->load();
}

std::unique_ptr<ServerHandle> start_server(const ServerOptions& options) {
  // 内嵌前端自检：C2026 截断后页面仍能打开，只是内容少了几个字符，光靠肉眼看不出。
  // 这里比对的是「直读 web/ 源文件」的字节数与 SHA256，不是生成出来的 .cpp。
  // --web-dir 开发态不用内嵌资源，自然不查；但仍要求目录里有 index.html，
  // 免得服务起来了才发现首屏 404。
  if (options.web_dir.empty()) {
    const std::string asset_error = embedded_assets_error();
    if (!asset_error.empty()) {
      throw AppError("web_assets", "内嵌前端与源文件不一致：" + asset_error);
    }
  } else if (!std::filesystem::is_regular_file(options.web_dir / "index.html")) {
    throw AppError("bad_config",
                   "--web-dir 下找不到 index.html：" +
                       path_to_utf8(options.web_dir));
  }

  auto handle = std::unique_ptr<ServerHandle>(new ServerHandle());
  ServerState& state = *handle->impl_;
  state.data_dir = options.data_dir;
  state.web_dir = options.web_dir;
  state.network = options.network;
  state.allowed_hosts = options.allowed_hosts;
  state.rate_limit = options.rate_limit;
  state.bound_host = options.host;
  state.machine_name = local_host_name();

  std::error_code ec;
  std::filesystem::create_directories(options.data_dir, ec);
  if (ec) {
    throw AppError("data_dir", "无法创建数据目录: " + ec.message());
  }
  // 上次中断留下的 .part / curl 日志不能参与校验，也不能越积越多。
  cleanup_update_partials(options.data_dir / "updates");

  // 恢复上次持久化的会话：进程重启后 WebView2 cookie 还在，server 进程内存空了。
  load_sessions(state);

  // 多实例防护：同一数据目录（规范化后）只能有一个 server 持有写权。
  // 必须在打开数据库之前拿到锁，否则两个进程会先各自打开库再互相覆盖。
  std::string guard_error;
  state.instance_guard = InstanceGuard::acquire(options.data_dir, &guard_error);
  if (!state.instance_guard) {
    throw AppError("instance_in_use", guard_error);
  }

  state.db = std::make_unique<Db>(options.data_dir / "devicedb.db");
  migrate(*state.db);

  // 第三层防护：库内心跳。锁文件被删除、或两条路径 hardlink 到同一个库时，
  // 只有这一行能判出「还有人在用」。放在库文件身份互斥体之前，
  // 这样验收（删锁文件后再起一个）会走到心跳这条判据上。
  state.instance_id = random_token().substr(0, 16);
  std::string heartbeat_error;
  if (!claim_heartbeat(*state.db, state.instance_id, &heartbeat_error)) {
    throw AppError("instance_in_use", heartbeat_error);
  }

  if (!state.instance_guard->claim_database(options.data_dir / "devicedb.db",
                                            &guard_error)) {
    throw AppError("instance_in_use", guard_error);
  }

  const SeedResult seed = seed_default_users(*state.db, options.data_dir);
  state.seeded = seed.seeded;
  if (seed.seeded) {
    std::cout << "首次启动：已生成初始账号，密码写入 "
              << path_to_utf8(seed.password_file) << "（首次登录必须修改）"
              << std::endl;
    for (const auto& [name, password] : seed.accounts) {
      std::cout << "  用户名: " << name << "  初始密码: " << password << std::endl;
    }
  }

  state.devices = std::make_unique<DeviceService>(*state.db);
  state.users = std::make_unique<UserService>(*state.db);
  state.local_token = random_token();
  state.server.set_payload_max_length(8 * 1024 * 1024);
#ifdef _WIN32
  // Windows 的 SO_REUSEADDR 允许第二个进程绑同一端口，两边会随机抢连接；
  // 换成 SO_EXCLUSIVEADDRUSE，让端口冲突在 bind 阶段就明确失败。
  state.server.set_socket_options([](::socket_t sock) {
    int disabled = 0;
    int enabled = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&disabled), sizeof(disabled));
    setsockopt(sock, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
               reinterpret_cast<const char*>(&enabled), sizeof(enabled));
  });
#endif

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
                     "缺少随包字体 " + path_to_utf8(path) +
                         "：打印与预览必须用同一份子集，缺字体时不做降级，直接中止启动");
    }
    const std::string actual = sha256_hex(bytes);
    if (bytes.size() != spec.bytes || actual != spec.sha256) {
      throw AppError("font_corrupt",
                     "字体校验失败 " + path_to_utf8(path) + "：期望 " +
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
                            {"mode", state.network ? "network" : "local"}});
  });

  // 版本检查：只读更新清单并比较版本，返回下载页；不下载、不替换。
  state.server.Get("/api/version", [&state, schema, version](
                                       const httplib::Request&,
                                       httplib::Response& response) {
    const UpdateManifest manifest = read_update_manifest(state.data_dir);
    // 本平台的最新版本与构建号来自验签通过的清单；build 只在同版本时参与
    // 判定（见下方 available）。
    const std::string latest = manifest.latest;
    // macOS 只比版本号；Windows 同版本再看构建号（沿用小兔规则：远端 build
    // 必须大于本机才有更新）。本机构建号当前没有可靠来源（VERSION.txt 只有
    // x.y.z，版本资源第 4 段恒 0），按 0 处理——0 视为「缺失」，缺失即跳过
    // build 比较：同版本号一律不再提示。否则同版本重发包会形成「装完还是
    // 同一个版本号 → 再提示 → 再装」的死循环；等客户端有了构建号来源，
    // 把 available 放开为 `cmp == 0 && local_build > 0 &&
    // manifest.latest_build > local_build` 即可。
    const bool available = !latest.empty() && compare_version(latest, version) > 0;
    set_json(response,
             json{{"version", version},
                  {"schema_version", schema},
                  {"latest", latest.empty() ? json(nullptr) : json(latest)},
                  {"update_available", available},
                  {"download_url", manifest.download_url},
                  {"download_page", download_page()},
                  {"notes", manifest.notes},
                  {"assets", manifest.assets},
                  // M6：远端检查的开关来自同一份 manifest.json（地址内嵌，
                  // 间隔用默认配置），前端不写死地址。
                  {"check", json{{"enabled", manifest.check_enabled},
                                 {"endpoint", manifest.check_endpoint},
                                 {"interval_hours", manifest.check_interval_hours}}},
                  {"manifest", manifest.found
                                   ? json(path_to_utf8(state.data_dir /
                                                       kUpdateManifestName))
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
                      const std::string username = body.value("username", "");
                      const std::string password = body.value("password", "");
                      // 先查锁定再验密码：锁定期内即使密码正确也拒，
                      // 否则攻击者只要撞对一次就能把计数清零。
                      const RateLimitDecision limited =
                          rate_limit_check(state, request.remote_addr, username);
                      if (limited.locked) {
                        AuditEntry blocked;
                        blocked.actor = username.empty() ? "anonymous" : username;
                        blocked.actor_ip = request.remote_addr;
                        blocked.action = "login_blocked";
                        blocked.entity = "user";
                        blocked.entity_id = username;
                        blocked.summary = "失败次数过多，429 拒绝（" +
                                          std::to_string(limited.retry_after) +
                                          " 秒后可重试）";
                        audit_event(state, blocked);
                        throw_rate_limited(response, limited);
                      }
                      User user;
                      try {
                        user = state.users->authenticate(username, password);
                      } catch (const AppError& error) {
                        rate_limit_record_failure(state, request.remote_addr,
                                                 username);
                        // 失败也记：撞库在审计里必须能看出来自哪个 IP。
                        AuditEntry entry;
                        entry.actor = username.empty() ? "anonymous" : username;
                        entry.actor_ip = request.remote_addr;
                        entry.action = "login_failed";
                        entry.entity = "user";
                        entry.entity_id = username;
                        entry.summary = error.what();
                        audit_event(state, entry);
                        throw;
                      }
                      // 登录成功清零：同一键的正常使用不该被历史失败拖累。
                      rate_limit_clear(state, request.remote_addr, username);
                      set_session_cookie(response,
                                         create_session(state, user.user_name));
                      AuditEntry entry;
                      entry.actor = user.user_name;
                      entry.actor_ip = request.remote_addr;
                      entry.action = "login";
                      entry.entity = "user";
                      entry.entity_id = user.user_name;
                      entry.summary = "登录成功";
                      audit_event(state, entry);
                      set_json(response, json{{"user", user_json(user)}});
                    });

  // 自更新阶段一：下载 + 双校验（SHA256 与字节数），不做替换。
  // 下载器走系统 curl：TLS 与证书校验由系统提供，服务端只负责白名单、
  // 落盘到数据目录 updates/、以及校验；任何失败都删残件并回到「只提示」。
  state.server.Post("/api/update/download",
                    [&state, version](const httplib::Request& request,
                                      httplib::Response& response) {
                      require_admin(state, request);
                      // 前端不给入口不算闸门：检查关闭时服务端也要拒绝。
                      const UpdateManifest manifest =
                          read_update_manifest(state.data_dir);
                      if (!manifest.check_enabled) {
                        throw AppError("update_disabled",
                                       "更新检查已关闭，不提供下载");
                      }
                      const json body = parse_body(request.body);
                      UpdatePackageSpec spec;
                      spec.version = body.value("version", "");
                      spec.file_name = body.value("file_name", "");
                      spec.url = body.value("url", "");
                      spec.sha256 = body.value("sha256", "");
                      spec.size =
                          body.value("size", static_cast<std::uint64_t>(0));
                      const std::string invalid = validate_update_spec(spec);
                      if (!invalid.empty()) {
                        throw AppError(validate_update_spec_code(spec), invalid);
                      }
                      // v2：下载用的 url/sha256/size 必须与验签通过的清单一致。
                      // 否则验签只拦得住「本地提示」，拦不住拿着来路不明的
                      // spec 直接 POST。清单验签失败时 check_enabled 已为
                      // false（上面就拒绝了）；数据目录没有清单文件时保持
                      // 旧行为，由白名单与版本检查兜底。
                      if (manifest.found) {
                        bool matched = false;
                        for (const json& item : manifest.assets) {
                          matched =
                              item.value("browser_download_url", "") == spec.url &&
                              item.value("digest", "") == "sha256:" + spec.sha256 &&
                              item.value("size", std::uint64_t{0}) == spec.size;
                          if (matched) break;
                        }
                        if (!matched) {
                          throw AppError("not_in_manifest",
                                         "下载信息与更新清单不一致（清单可能已更新，请重新检查更新）");
                        }
                      }
                      // 没有比当前版本更新的版本时，不允许借下载接口落盘。
                      // （Windows 同版本的新构建号要等客户端有了本机构建号
                      // 来源才会放行——见 /api/version 里 build 比较的说明。）
                      if (compare_version(spec.version, version) <= 0) {
                        throw AppError("no_update", "没有可用的新版本");
                      }
                      // 用户在壳层 NSSavePanel 里选的完整落点（可选）。给了就先下到
                      // updates/ 校验，再整体挪到该路径，避免命令行出现非 ASCII 参数。
                      std::filesystem::path save_path;
                      const std::string save_path_text =
                          body.value("save_path", "");
                      if (!save_path_text.empty()) {
                        save_path = path_from_utf8(save_path_text);
                        if (!save_path.is_absolute()) {
                          throw AppError("invalid_path",
                                         "保存位置必须是绝对路径");
                        }
                      }

                      const std::filesystem::path updates_dir =
                          state.data_dir / "updates";
                      // 与下载器共用同一处计算：轮询读的 part 必须就是 curl 写的那个。
                      const std::filesystem::path part =
                          update_part_path(updates_dir, spec.file_name);
                      const std::shared_ptr<ServerState::UpdateDownload> download =
                          state.update;
                      {
                        std::lock_guard<std::mutex> lock(download->mutex);
                        if (download->state == "running") {
                          throw AppError("update_busy",
                                         "已有更新包正在下载，请稍候");
                        }
                        download->state = "running";
                        download->total = spec.size;
                        download->received = 0;
                        download->part_path = part;
                        download->path.clear();
                        download->sha256.clear();
                        download->code.clear();
                        download->error.clear();
                      }
                      std::thread([download, updates_dir, spec, save_path]() {
                        UpdateDownloadOutcome outcome =
                            download_update_package(updates_dir, spec);
                        // 校验通过后再搬到用户选的位置；搬失败就整体失败、不留残件，
                        // 状态落到 failed 而不是卡在 running。
                        if (outcome.ok && !save_path.empty()) {
                          std::string move_error;
                          if (!relocate_update_package(
                                  path_from_utf8(outcome.path), save_path,
                                  &move_error)) {
                            outcome.ok = false;
                            outcome.error_code = "save_failed";
                            outcome.error = move_error;
                          } else {
                            outcome.path = path_to_utf8(save_path);
                          }
                        }
                        std::lock_guard<std::mutex> lock(download->mutex);
                        download->part_path.clear();
                        if (outcome.ok) {
                          download->state = "done";
                          download->path = outcome.path;
                          download->sha256 = outcome.sha256;
                          download->received = outcome.bytes;
                          download->total = outcome.bytes;
                        } else {
                          download->state = "failed";
                          download->code = outcome.error_code;
                          download->error = outcome.error;
                        }
                      }).detach();
                      set_json(response,
                               json{{"state", "running"}, {"total", spec.size}},
                               202);
                    });

  state.server.Get("/api/update/download",
                   [&state](const httplib::Request& request,
                            httplib::Response& response) {
                     require_admin(state, request);
                     const std::shared_ptr<ServerState::UpdateDownload> download =
                         state.update;
                     std::lock_guard<std::mutex> lock(download->mutex);
                     std::uint64_t received = download->received;
                     if (download->state == "running" &&
                         !download->part_path.empty()) {
                       std::error_code ec;
                       const auto size =
                           std::filesystem::file_size(download->part_path, ec);
                       if (!ec) received = static_cast<std::uint64_t>(size);
                     }
                     set_json(response,
                              json{{"state", download->state},
                                   {"received", received},
                                   {"total", download->total},
                                   {"path", download->path},
                                   {"sha256", download->sha256},
                                   {"code", download->code},
                                   {"error", download->error}});
                   });

  // 自更新阶段二（仅 Windows 安装版）：把已下载并校验过的 Inno 安装包交给
  // 独立的协助进程去装。本进程先回 202，约 300ms 后停服退出；安装器负责
  // 关掉还占着程序文件的实例。退出码与版本变化由新进程从 install-result.json 回读。
  state.server.Post("/api/update/install",
                    [&state](const httplib::Request& request,
                             httplib::Response& response) {
                      require_admin(state, request);
#ifndef _WIN32
                      // mac 侧只有下载（阶段一），替换与重启未实现。
                      throw AppError("install_unsupported",
                                     "自动安装目前只有 Windows 安装版支持");
#else
                      if (!is_installed_layout()) {
                        throw AppError(
                            "install_unsupported",
                            "绿色版不提供自动安装：请退出后手动替换程序目录。");
                      }
                      const json body = parse_body(request.body);
                      const std::filesystem::path updates_dir =
                          state.data_dir / "updates";
                      std::filesystem::path package;
                      std::string expected_sha;
                      std::uint64_t expected_size = 0;
                      const std::string requested =
                          body.value("path", std::string{});
                      if (!requested.empty()) {
                        // 显式指定：仅用于重试已落盘的包；仍要自带 SHA256 与字节数复核。
                        package = path_from_utf8(requested);
                        expected_sha = body.value("sha256", std::string{});
                        expected_size =
                            body.value("size", static_cast<std::uint64_t>(0));
                        if (!is_valid_sha256(expected_sha) ||
                            expected_size == 0) {
                          throw AppError("manifest_invalid",
                                         "指定安装包时必须同时给出 sha256 与字节数");
                        }
                      } else {
                        std::lock_guard<std::mutex> lock(state.update->mutex);
                        if (state.update->state != "done") {
                          throw AppError("update_package_missing",
                                         "还没有已下载并校验的更新包");
                        }
                        package = path_from_utf8(state.update->path);
                        expected_sha = state.update->sha256;
                        expected_size = state.update->total;
                      }

                      std::error_code ec;
                      const std::filesystem::path base =
                          std::filesystem::weakly_canonical(updates_dir, ec);
                      if (ec || base.empty()) {
                        throw AppError("install_failed",
                                       "数据目录 updates/ 路径不可用");
                      }
                      const std::filesystem::path full =
                          std::filesystem::weakly_canonical(package, ec);
                      if (ec || full.empty()) {
                        throw AppError("update_package_missing",
                                       "安装包路径不可用：" +
                                           path_to_utf8(package));
                      }
                      // 只认 updates/ 下的 .exe：这个接口不能变成「跑任意程序」。
                      const std::filesystem::path relative =
                          full.lexically_relative(base);
                      if (relative.empty() ||
                          path_to_utf8(relative).rfind("..", 0) == 0) {
                        throw AppError("install_package_invalid",
                                       "安装包必须位于数据目录 updates/ 下");
                      }
                      if (full.extension() != L".exe") {
                        throw AppError("install_package_invalid",
                                       "只接受 .exe 安装包");
                      }
                      if (!std::filesystem::is_regular_file(full, ec)) {
                        throw AppError("update_package_missing",
                                       "安装包不存在：" + path_to_utf8(full));
                      }
                      // 触发时再校验一次：下载完成到点安装之间文件可能已被换掉。
                      const UpdateVerification checked =
                          verify_update_file(full, expected_size, expected_sha);
                      if (!checked.ok) {
                        throw AppError(checked.code, checked.error);
                      }

                      {
                        std::lock_guard<std::mutex> lock(state.update->mutex);
                        if (state.update->installing) {
                          throw AppError("install_busy", "已在安装中，请稍候");
                        }
                        state.update->installing = true;
                      }

                      // 协助器用 devcard-server.exe（它的 main 解析 --apply-update），
                      // 而不是当前的壳层 DevCard.exe（不处理 --apply-update 参数）。
                      const std::filesystem::path helper =
                          updates_dir / "devcard-updater.exe";
                      const std::filesystem::path server_exe =
                          current_executable().parent_path() / L"devcard-server.exe";
                      std::filesystem::copy_file(
                          server_exe, helper,
                          std::filesystem::copy_options::overwrite_existing, ec);
                      if (ec) {
                        throw AppError("install_failed",
                                       "无法准备更新协助程序：" + ec.message());
                      }
                      // 上一次的结果不能留到这一次，否则界面会把旧结果当成本次结果。
                      std::filesystem::remove(updates_dir / "install-result.json",
                                              ec);
                      // 先落 installing 再起协助器：没这一步，协助器被杀后
                      // 磁盘上没有任何「升级曾经开始过」的证据。
                      {
                        std::string state_error;
                        if (!write_apply_state(state.data_dir,
                                               ApplyState::Installing,
                                               "安装接口已受理，等待协助器接管",
                                               &state_error)) {
                          throw AppError("install_failed",
                                         "无法写入安装状态：" + state_error);
                        }
                      }
                      ApplyUpdateOptions options;
                      options.setup = full;
                      options.wait_pid = current_process_id();
                      options.result = updates_dir / "install-result.json";
                      options.relaunch = current_executable();
                      options.relaunch_args = current_arguments();
                      spawn_apply_update(helper, options);

                      set_json(response,
                               json{{"state", "installing"},
                                    {"setup", path_to_utf8(full)}},
                               202);
                      // 202 写回浏览器后，只置 stop_requested 让本进程退出：
                      //   - CLI server mode：main.cpp 轮询该标志，退出后析构 ServerHandle 停掉监听
                      //   - shell 模式：main.cpp 的 watcher 轮询该标志 → PostMessageW(WM_CLOSE)
                      // 这里不要再碰 state.server（原实现延迟 300ms 调 server.stop()，
                      // 而脱离线程捕获的是 state 引用：进程若先退出，就是对已销毁对象调成员函数）。
                      const std::shared_ptr<std::atomic<bool>> stop =
                          state.stop_requested;
                      std::thread([stop]() {
                        std::this_thread::sleep_for(
                            std::chrono::milliseconds(300));
                        stop->store(true);
                      }).detach();
#endif
                    });

  // 安装结果：新进程启动后由界面读回，显示「安装器退出码 X｜版本 A → B」。
  state.server.Get("/api/update/install-result",
                   [&state](const httplib::Request& request,
                            httplib::Response& response) {
                     require_admin(state, request);
                     const std::filesystem::path file =
                         state.data_dir / "updates" / "install-result.json";
                     std::error_code ec;
                     if (!std::filesystem::is_regular_file(file, ec)) {
                       // 没有结果文件时看状态文件：installing 且超阈值即中断，
                       // 由「启动时还在 installing」推出来，不落第五个状态。
                       const ApplyStateSnapshot snapshot =
                           read_apply_state(state.data_dir);
                       if (apply_state_interrupted(snapshot)) {
                         set_json(response,
                                  json{{"state", "interrupted"},
                                       {"detail", "升级被中断：协助器未完成安装"},
                                       {"updated_at", snapshot.updated_at},
                                       {"age_seconds", snapshot.age_seconds}});
                         return;
                       }
                       if (snapshot.present && snapshot.state == "installing") {
                         set_json(response, json{{"state", "installing"},
                                                 {"updated_at", snapshot.updated_at}});
                         return;
                       }
                       set_json(response, json{{"state", "none"}});
                       return;
                     }
                     std::ifstream in(file, std::ios::binary);
                     if (!in) {
                       throw AppError("io_error", "无法读取安装结果文件");
                     }
                     const std::string text(
                         (std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());
                     const json doc = json::parse(text, nullptr, false);
                     if (doc.is_discarded() || !doc.is_object()) {
                       throw AppError("io_error", "安装结果文件不是合法 JSON");
                     }
                     json out = doc;
                     out["state"] = "done";
                     set_json(response, out);
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
    set_json(response, json{{"user", user_json(user)},
                            {"version", version},
                            {"schema_version", schema},
                            {"data_dir", path_to_utf8(state.data_dir)},
                            {"company_name", company_name(*state.db)},
                            {"company_name_print", company_print_name(*state.db)},
                            {"company_name_is_default",
                             get_setting(*state.db, "print_company_name").empty()},
                            {"company_name_max_width", kCompanyNameMaxWidth},
                            {"company_logo_url", "/company-logo"},
                            {"company_logo_updated_at",
                             get_setting(*state.db, "company_logo_updated_at")}});
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
                      const ImportReport report = state.devices->import_rows(
                          rows, audit_context(state, request));
                      set_json(response, json{{"imported", report.imported},
                                              {"skipped_empty", report.skipped_empty},
                                              {"skipped_invalid", report.skipped_invalid},
                                              {"invalid_rows", report.invalid_rows},
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
                     const auto id = parse_path_id(request.matches[1].str());
                     set_json(response, device_json(state.devices->get(id)));
                   });

  state.server.Post("/api/devices", [&state](const httplib::Request& request,
                                             httplib::Response& response) {
    Device device = device_from_json(parse_body(request.body));
    set_json(response,
             device_json(state.devices->create(std::move(device),
                                               audit_context(state, request))),
             201);
  });

  state.server.Put(R"(/api/devices/(\d+))",
                   [&state](const httplib::Request& request,
                            httplib::Response& response) {
                     const auto id = parse_path_id(request.matches[1].str());
                     Device device = device_from_json(parse_body(request.body));
                     try {
                       set_json(response,
                                device_json(state.devices->update(
                                    id, std::move(device),
                                    audit_context(state, request))));
                     } catch (const ConflictError& error) {
                       set_json(response, conflict_json(error), 409);
                     }
                   });

  // P3：管理员强制覆盖。普通用户 403（role 校验在服务端，不依赖按钮隐藏）；
  // 覆盖的受害者版本与 updated_by/updated_at 由 core 写入 force_overwrite 审计。
  state.server.Post(R"(/api/devices/(\d+)/force)",
                    [&state](const httplib::Request& request,
                             httplib::Response& response) {
                      const User actor = require_admin(state, request);
                      const auto id = parse_path_id(request.matches[1].str());
                      Device device = device_from_json(parse_body(request.body));
                      const AuditContext context{actor.user_name,
                                                 request.remote_addr};
                      set_json(response,
                               device_json(state.devices->force_update(
                                   id, std::move(device), context)));
                    });

  state.server.Delete(R"(/api/devices/(\d+))",
                      [&state](const httplib::Request& request,
                               httplib::Response& response) {
                        const std::string version_text =
                            request.get_param_value("version");
                        if (version_text.empty()) {
                          throw AppError(
                              "bad_version",
                              "删除需要带 ?version=N：请先刷新列表，"
                              "避免用过期页面删掉别人刚改过的记录");
                        }
                        std::int64_t version = 0;
                        try {
                          version = std::stoll(version_text);
                        } catch (...) {
                          throw AppError("bad_version",
                                         "version 必须是整数：" + version_text);
                        }
                        try {
                          state.devices->remove(
                              parse_path_id(request.matches[1].str()), version,
                              audit_context(state, request));
                          set_json(response, json{{"ok", true}});
                        } catch (const ConflictError& error) {
                          set_json(response, conflict_json(error), 409);
                        }
                      });

  // 合并重复：删除三字段完全相同的多余记录（每组保留最早一条）。
  // 删除是管理员动作且不可撤销，因此要求显式确认串，避免误触。
  state.server.Post("/api/devices/dedupe",
                    [&state](const httplib::Request& request,
                             httplib::Response& response) {
                      const User actor = require_admin(state, request);
                      const json body = parse_body(request.body);
                      if (body.value("confirm", "") != "MERGE_DUPLICATES") {
                        throw AppError(
                            "bad_request",
                            "合并重复需要确认：请求体带 "
                            "{\"confirm\":\"MERGE_DUPLICATES\"}");
                      }
                      const DedupeReport report = state.devices->dedupe(
                          AuditContext{actor.user_name, request.remote_addr});
                      set_json(response, json{{"removed", report.removed},
                                              {"remaining", report.remaining}});
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
    drop_sessions_for(state, name);
    set_json(response, json{{"ok", true}});
  });

  state.server.Post("/api/users/password",
                    [&state](const httplib::Request& request,
                             httplib::Response& response) {
                      const User actor = current_user(state, request);
                      // 改密同样双键计数：猜旧密码与撞登录密码是同一类攻击。
                      const RateLimitDecision limited =
                          rate_limit_check(state, request.remote_addr,
                                           actor.user_name);
                      if (limited.locked) throw_rate_limited(response, limited);
                      std::string target;
                      try {
                        const json body = parse_body(request.body);
                        target = body.value("user_name", "");
                        if (target.empty()) target = actor.user_name;
                        const std::string new_password = body.value("new_password", "");
                        if (new_password.empty()) {
                          throw AppError("bad_json", "new_password 不能为空");
                        }
                        if (target == actor.user_name) {
                          // 是否免旧密码只由服务端状态决定：本次是首次登录或管理员
                          // 重置后的强制改密时，用户本来就不掌握旧密码。
                          if (actor.must_change_password) {
                            state.users->complete_forced_change(actor.user_name,
                                                                new_password);
                          } else {
                            const std::string old_password =
                                body.value("old_password", "");
                            if (old_password.empty()) {
                              throw AppError("old_password_required", "请输入原密码");
                            }
                            state.users->change_password(actor.user_name, old_password,
                                                         new_password);
                          }
                        } else {
                          if (actor.role != "admin") {
                            throw AppError("forbidden", "需要管理员权限");
                          }
                          state.users->reset_password(target, new_password);
                        }
                        if (!state.users->any_must_change()) {
                          remove_seed_password_file(state.data_dir);
                        }
                        // 改密 / 重置后旧会话一律作废；改自己的密码时保留当前这条，
                        // 免得刚改完就把自己踢回登录页。
                        const std::string keep =
                            target == actor.user_name
                                ? cookie_value(request, "sid")
                                : std::string{};
                        drop_sessions_for(state, target, keep);
                        set_json(response, json{{"ok", true}, {"user_name", target}});
                      } catch (const AppError&) {
                        // 只有冲着操作者自己来的失败才计数：管理员重置别人的密码失败
                        // （比如用户名打错）不该把管理员自己锁在门外。
                        if (target.empty() || target == actor.user_name) {
                          rate_limit_record_failure(state, request.remote_addr,
                                                    actor.user_name);
                        }
                        throw;
                      }
                      rate_limit_clear(state, request.remote_addr, actor.user_name);
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
                       throw AppError(
                           "bad_company_name",
                           "公司名称最多 16 个字符（卡面按 13 字宽打印）");
                     }
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
                      const User actor = require_admin(state, request);
                      const json body = parse_body(request.body);
                      if (body.value("confirm", "") != "RESTORE") {
                        throw AppError("bad_json", "恢复操作需要 confirm=RESTORE");
                      }
                      const std::string name = body.value("name", "");
                      restore_backup(*state.db, state.data_dir, name);
                      // 备份里带着导出时的心跳行，恢复后必须由当前实例重写，
                      // 否则定时刷新会因 instance_id 不匹配而停摆。
                      force_heartbeat(*state.db, state.instance_id);
                      {
                        std::lock_guard<std::mutex> lock(state.sessions_mutex);
                        state.sessions.clear();
                        persist_sessions(state);
                      }
                      AuditEntry entry;
                      entry.actor = actor.user_name;
                      entry.actor_ip = request.remote_addr;
                      entry.action = "restore";
                      entry.entity = "system";
                      entry.summary = "从备份恢复：" + name;
                      audit_event(state, entry);
                      clear_session_cookie(response);
                      set_json(response, json{{"ok", true}, {"reload", true}});
                    });

  // ==== P1 新增：打开数据目录（整段插入，rebase 时整段保留）====
  // 路径只取服务端自己判定的 state.data_dir，不接受前端传参；
  // 仅管理员可用，避免普通用户借服务端拉起资源管理器。
  state.server.Post("/api/system/open-data-dir",
                    [&state](const httplib::Request& request,
                             httplib::Response& response) {
                      require_admin(state, request);
#ifdef _WIN32
                      const std::wstring dir = state.data_dir.wstring();
                      const HINSTANCE result =
                          ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr,
                                        nullptr, SW_SHOWNORMAL);
                      if (reinterpret_cast<std::intptr_t>(result) <= 32) {
                        throw AppError("bad_request",
                                       "无法打开数据目录：" +
                                           path_to_utf8(state.data_dir));
                      }
                      set_json(response,
                               json{{"ok", true},
                                    {"path", path_to_utf8(state.data_dir)}});
#else
                      throw AppError("bad_request",
                                     "打开数据目录仅在 Windows 版本提供");
#endif
                    });
  // ==== P1 新增段结束 ====

  // 审计查询：仅管理员。user 角色一律 403。
  state.server.Get("/api/audit", [&state](const httplib::Request& request,
                                          httplib::Response& response) {
    require_admin(state, request);
    const int limit = int_param(request, "limit", 200);
    json items = json::array();
    for (const AuditEntry& entry : list_audit(*state.db, limit)) {
      items.push_back({{"id", entry.id},
                       {"at", entry.at},
                       {"actor", entry.actor},
                       {"actor_ip", entry.actor_ip},
                       {"action", entry.action},
                       {"entity", entry.entity},
                       {"entity_id", entry.entity_id},
                       {"summary", entry.summary},
                       {"before", entry.before_json},
                       {"after", entry.after_json}});
    }
    set_json(response, json{{"items", items}});
  });

  state.server.Get("/", [&state](const httplib::Request&, httplib::Response& response) {
    // 开发态（--web-dir）每次请求都重读磁盘，改完刷新即生效。
    std::string html = state.web_dir.empty()
                           ? std::string(embedded_index_html())
                           : read_file_bytes(state.web_dir / "index.html");
    if (html.empty()) {
      throw AppError("not_found", "前端文件不存在：index.html");
    }
    // 本地窗口模式注入一次性令牌供壳自动登录；网络模式绝不注入：
    // 否则任何能打开首页的人都能拿到本地自动登录令牌。
    if (!state.network) {
      const std::string meta = "<meta name=\"devcard-local-token\" content=\"" +
                               state.local_token + "\">";
      const std::size_t head = html.find("</head>");
      if (head != std::string::npos) html.insert(head, meta + "\n");
    }
    set_asset(response, html, "text/html; charset=utf-8");
  });
  state.server.Get("/styles.css", [&state](const httplib::Request&,
                                           httplib::Response& response) {
    std::string css = state.web_dir.empty() ? std::string(embedded_styles_css())
                                            : read_file_bytes(state.web_dir / "styles.css");
    if (css.empty()) throw AppError("not_found", "前端文件不存在：styles.css");
    set_asset(response, css, "text/css; charset=utf-8");
  });
  state.server.Get("/app.js", [&state](const httplib::Request&,
                                       httplib::Response& response) {
    std::string js = state.web_dir.empty() ? std::string(embedded_app_js())
                                           : read_file_bytes(state.web_dir / "app.js");
    if (js.empty()) throw AppError("not_found", "前端文件不存在：app.js");
    set_asset(response, js, "application/javascript; charset=utf-8");
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
  // 浏览器标签图标：开发态（--web-dir）优先读磁盘，打包后回落内嵌。
  state.server.Get("/favicon.ico",
                   [&state](const httplib::Request&,
                            httplib::Response& response) {
                     std::string bytes;
                     if (!state.web_dir.empty()) {
                       bytes = read_file_bytes(state.web_dir / "favicon.ico");
                     }
                     if (bytes.empty()) bytes = favicon_ico();
                     if (bytes.empty()) {
                       set_json(response,
                                json{{"error", "not_found"},
                                     {"message", "favicon.ico 不存在"}},
                                404);
                       return;
                     }
                     response.status = 200;
                     response.set_header("Content-Type", "image/x-icon");
                     response.set_header("Cache-Control", "no-store");
                     response.body = std::move(bytes);
                   });

  // Web 图标与 manifest：开发态（--web-dir）优先读磁盘，打包后回落到内嵌。
  // 这些路径都在 /api/ 之外，认证中间件不介入。
  for (std::size_t i = 0; i < embedded_static_asset_count(); ++i) {
    // 按值捕获指针：asset 是循环内的引用变量，捕获它的引用会在下一轮迭代后悬垂。
    const StaticAsset* asset = &embedded_static_assets()[i];
    state.server.Get(
        asset->path,
        [&state, asset](const httplib::Request&, httplib::Response& response) {
          std::string bytes;
          if (!state.web_dir.empty()) {
            // asset->path 以 / 开头，直接拼会覆盖 web_dir，必须先去掉。
            bytes = read_file_bytes(state.web_dir / (asset->path + 1));
          }
          if (bytes.empty()) bytes = hex_to_bytes(asset->hex);
          if (bytes.empty()) {
            set_json(response,
                     json{{"error", "not_found"},
                          {"message", std::string(asset->path) + " 不存在"}},
                     404);
            return;
          }
          response.status = 200;
          response.set_header("Content-Type", asset->mime);
          response.set_header("Cache-Control", "public, max-age=86400");
          response.body = std::move(bytes);
        });
  }

  // 认证与 CSRF 兜底：/api/health 与 /api/session/* 免认证。
  state.server.set_pre_routing_handler(
      [&state](const httplib::Request& request, httplib::Response& response) {
        // 校验顺序写死，不要交换：
        //   1 Host 白名单 → 2 来源 IP → 3 路径白名单 → 4 CSRF（非 GET）
        //   → 5 登录 → 6 must_change_password → 7 角色（在各路由的
        //   require_admin 里，晚于本 handler 执行）。
        // 先放行再查 Host，任何一条白名单路径都会顺带绕过 Host 校验；
        // 这类漏洞不报错、也不写日志，只会静默存在。
        if (!host_allowed(state, request.get_header_value("Host"))) {
          set_json(response,
                   json{{"error", "bad_host"},
                        {"message", "Host 头不在允许列表内（防 DNS rebinding）：" +
                                        request.get_header_value("Host")}},
                   421);
          return httplib::Server::HandlerResponse::Handled;
        }
        // 纵深防御：网络模式只接受回环与 RFC1918 来源，公网来源直接 403。
        // 这不能替代防火墙，只是别把库暴露在公网网卡上。
        if (state.network && !is_loopback(request.remote_addr) &&
            !is_private_ipv4(request.remote_addr)) {
          set_json(response,
                   json{{"error", "forbidden_source"},
                        {"message",
                         "网络版只接受本机与内网（10/172.16–31/192.168）来源，"
                         "当前来源 IP：" +
                             request.remote_addr}},
                   403);
          return httplib::Server::HandlerResponse::Handled;
        }
        if (request.path.rfind("/api/", 0) != 0) {
          return httplib::Server::HandlerResponse::Unhandled;
        }
        // 路径白名单只读上面那张表，不要在这里另写判断。
        if (is_bypass_path(request.path)) {
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
                        {"message", "首次登录必须修改密码"}},
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

  // 阶段二判据：握手标记写在数据库与字体初始化之后、HTTP 开始服务之前。
  // 写失败必须让启动失败——静默跳过会让「新版本启动即崩」这条判据假绿。
  {
    std::string marker_error;
    if (!write_app_started_marker(options.data_dir, DEVCARD_VERSION,
                                  &marker_error)) {
      throw AppError("app_started_marker",
                     "无法写入启动标记 app-started.marker：" + marker_error);
    }
  }

  if (!state.server.bind_to_port(options.host, options.port)) {
    throw AppError(
        "port_in_use",
        "无法监听 " + options.host + ":" + std::to_string(options.port) +
            "（端口被占用）。请换端口：--port " +
            std::to_string(options.port + 1) +
            "，或查看占用：netstat -ano | findstr :" +
            std::to_string(options.port));
  }
  handle->port_ = options.port;
  // 锁文件回写监听地址：第二个实例失败时能直接报出占用者在哪个地址上。
  state.instance_guard->set_owner_info(
      "PID " + std::to_string(process_id()) + "，启动于 " + now_text() +
      "，监听 " + options.host + ":" + std::to_string(options.port));
  // 心跳行同步监听地址，失败时第二个实例的报错能指出占用者。
  set_heartbeat_listen(*state.db, state.instance_id,
                       options.host + ":" + std::to_string(options.port));
  state.heartbeat_thread = std::thread([&state]() {
    while (!state.heartbeat_stop) {
      for (int i = 0; i < kHeartbeatIntervalSeconds; ++i) {
        if (state.heartbeat_stop) return;
        std::this_thread::sleep_for(std::chrono::seconds(1));
      }
      try {
        // 返回 false 说明行已被别的实例接管，自己停止刷新即可。
        if (!refresh_heartbeat(*state.db, state.instance_id)) return;
      } catch (const std::exception& error) {
        // 心跳写失败不终止服务；下一轮重试，连续失败会在下次启动时暴露。
        std::cerr << "警告：心跳写入失败：" << error.what() << std::endl;
      }
    }
  });
  state.thread = std::thread([&state] { state.server.listen_after_bind(); });
  return handle;
}

}  // namespace devcard
