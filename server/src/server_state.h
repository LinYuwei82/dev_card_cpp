#pragma once

// server 内部状态容器：只放数据与 core 服务句柄，不放 helper 函数。
// 从 server.cpp 的匿名 namespace 提到独立头，供后续拆出的模块共享；
// 公共头 devcard/server.h 保持不暴露这些实现细节。
//
// 生命周期：ServerHandle::Impl 继承本结构，由 server.cpp 的 start_server 持有；
// 路由/鉴权等模块只拿引用，不拥有、不延长生命周期。

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <httplib.h>

#include "devcard/db.h"
#include "devcard/server.h"
#include "devcard/service/device_service.h"
#include "devcard/service/user_service.h"
#include "font_coverage.h"
#include "instance_lock.h"

namespace devcard::detail {

// 登录/改密失败计数的单个键（来源 IP 或用户名）。字段用 atomic，
// 计数与判定另由 ServerState::rate_limit_mutex 串成一次操作。
struct RateLimitEntry {
  std::atomic<int> failures{0};
  std::atomic<long long> window_start{0};  // steady 秒；0 表示窗口未开始
  std::atomic<long long> locked_until{0};  // steady 秒
};

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
};

}  // namespace devcard::detail
