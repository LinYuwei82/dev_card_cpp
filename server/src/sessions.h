#pragma once

// 会话与鉴权：Cookie 解析、会话落盘/加载、当前用户与管理员校验、
// 审计上下文与独立审计写入。HTTP 鉴权闸门本身仍在 server.cpp 的
// pre_routing_handler 里，这里只提供它需要的查询能力。

#include <string>

#include <httplib.h>

#include "devcard/audit.h"
#include "devcard/models.h"
#include "server_state.h"

namespace devcard::detail {

std::string random_token();

// 从 Cookie 头里取指定名字的值；没有返回空串。
std::string cookie_value(const httplib::Request& request, const char* name);

void persist_sessions(ServerState& state);
void load_sessions(ServerState& state);

std::string session_user(ServerState& state, const httplib::Request& request);
std::string create_session(ServerState& state, const std::string& user);
void destroy_session(ServerState& state, const httplib::Request& request);
void drop_sessions_for(ServerState& state, const std::string& user,
                       const std::string& keep_token = {});
void set_session_cookie(httplib::Response& response, const std::string& token);
void clear_session_cookie(httplib::Response& response);

AuditContext audit_context(ServerState& state, const httplib::Request& request);
void audit_event(ServerState& state, AuditEntry entry);

User current_user(ServerState& state, const httplib::Request& request);
User require_admin(ServerState& state, const httplib::Request& request);

}  // namespace devcard::detail
