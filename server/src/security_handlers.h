#pragma once

// 认证闸门与统一响应/异常处理。校验顺序是安全契约，见实现内注释；
// 单独成文件是为了让 server.cpp 只保留启动装配与句柄生命周期。

#include "server_state.h"

namespace devcard::detail {

void register_security_handlers(ServerState& state);

}  // namespace devcard::detail
