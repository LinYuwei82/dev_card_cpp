#pragma once

// 请求来源与路径白名单判据。集中在这里，避免 Host 校验、来源 IP 限制
// 与免认证路径各写一份而漂移。

#include <string>

#include "server_state.h"

namespace devcard::detail {

bool is_loopback(const std::string& address);

// Host 白名单：本地窗口模式只认回环；网络模式再加入实际监听 IP、机器名
// 与配置的 allowed_hosts，其他 Host 返回 421，用于防 DNS rebinding。
bool host_allowed(const ServerState& state, std::string host);

// 免登录/免改密白名单：只允许下面两类，集中在这张表里判断。
// 更新检查与下载故意不在表内：网络模式下若未认证请求就能让服务端去拉
// 几十 MB 落盘，等于免费的内容缓存 + 磁盘写放大器；所以「检查个版本」
// 同样要求登录，强制改密前也不放行。以后不要往这里加更新相关路径。
bool is_bypass_path(const std::string& path);

}  // namespace devcard::detail
