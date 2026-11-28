#pragma once

// 路由注册入口：server.cpp 的 start_server 只负责装配，具体端点在 routes_*.cpp。
// 各函数只做「把 handler 挂到 state.server 上」，业务仍在 core/service。

#include <cstdint>
#include <string>

#include "server_state.h"

namespace devcard::detail {

void register_session_routes(ServerState& state, std::int64_t schema,
                             std::string version);
void register_device_routes(ServerState& state);
void register_print_routes(ServerState& state);
void register_user_routes(ServerState& state);
void register_system_routes(ServerState& state);
void register_static_routes(ServerState& state);

}  // namespace devcard::detail
