#include "rate_limit.h"

#include <chrono>
#include <map>
#include <mutex>
#include <string>

#include "devcard/error.h"
#include "devcard/server.h"
#include "guards.h"

namespace devcard::detail {
namespace {

long long steady_seconds() {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

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

}  // namespace

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

void throw_rate_limited(httplib::Response& response,
                        const RateLimitDecision& decision) {
  const int retry_after = decision.retry_after > 0 ? decision.retry_after : 1;
  response.set_header("Retry-After", std::to_string(retry_after));
  throw AppError("too_many_requests",
                 "失败次数过多，请 " + std::to_string(retry_after) + " 秒后重试");
}

}  // namespace devcard::detail
