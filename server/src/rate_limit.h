#pragma once

// 登录/改密失败限流：双键（来源 IP / 用户名）各自独立计数，任一超限即拒。
// 内存实现，进程重启清零（不落库）。

#include <string>

#include <httplib.h>

#include "server_state.h"

namespace devcard::detail {

struct RateLimitDecision {
  bool locked = false;
  int retry_after = 0;
};

RateLimitDecision rate_limit_check(ServerState& state, const std::string& address,
                                   const std::string& user);
void rate_limit_record_failure(ServerState& state, const std::string& address,
                               const std::string& user);
void rate_limit_clear(ServerState& state, const std::string& address,
                      const std::string& user);

// 锁定期间一律 429，连密码正确也不放行；Retry-After 由响应头带出。
void throw_rate_limited(httplib::Response& response,
                        const RateLimitDecision& decision);

}  // namespace devcard::detail
