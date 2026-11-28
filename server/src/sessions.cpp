#include "sessions.h"

#include <cctype>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <mutex>
#include <random>
#include <string>

#include <nlohmann/json.hpp>

#include "devcard/db.h"
#include "devcard/error.h"

#ifndef _WIN32
#include <sys/stat.h>
#endif

namespace devcard::detail {
namespace {

using nlohmann::json;

constexpr auto kSessionTtlSeconds = 12 * 3600;  // 12 小时，持久化用秒数方便序列化
constexpr const char* kSessionsFileName = "sessions.json";

}  // namespace

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

std::string random_token() {
  static const char hex[] = "0123456789abcdef";
  std::random_device device;
  std::string token;
  token.reserve(48);
  for (int i = 0; i < 24; ++i) token.push_back(hex[device() % 16]);
  return token;
}

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
#ifndef _WIN32
  // 会话令牌可直接复用：文件收紧到仅本人可读写，别按 umask 落成 0644。
  ::chmod(staging.c_str(), 0600);
#endif
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
                       const std::string& keep_token) {
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

AuditContext audit_context(ServerState& state, const httplib::Request& request) {
  return AuditContext{session_user(state, request), request.remote_addr};
}

// 独立于业务事务的审计（登录、恢复等）：写失败只告警，不影响主流程，
// 但不能静默——静默失败等于审计形同虚设。
void audit_event(ServerState& state, AuditEntry entry) {
  if (entry.actor.empty()) entry.actor = "system";
  try {
    state.db->write([&](Conn& conn) { record_audit(conn, entry); });
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

}  // namespace devcard::detail
