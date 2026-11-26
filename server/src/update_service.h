#pragma once

// 自动更新编排层（全仓唯一状态机）。
//
// 分层职责：
//   - 检测判据一律来自 core evaluate_update（唯一份），本类只负责
//     「何时查、失败怎么退避、结果存哪、给谁读」；
//   - 平台安装不在本类：win 走 /api/update/install → apply_update 协助器，
//     mac 走壳层 Sparkle 桥，本类只给出「可安装」的判定与产物。
//
// 状态落 <数据目录>/update-state.json（单写者 + 原子替换）。重启后先读回，
// 界面立刻有上次结果，不必等下一轮检查。

#include <atomic>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

#include "devcard/update_manifest.h"

namespace devcard {

struct UpdateServiceConfig {
  std::string endpoint;    // 远端 manifest.json 地址（必须是可信 https）
  std::string pubkey_hex;  // 清单 Ed25519 公钥（编译期常量传入）
  std::string channel = "stable";
  std::filesystem::path data_dir;
  // 拉清单文本的注入点（测试喂 fixture 用）；生产默认 curl 到临时文件再读回。
  std::function<bool(const std::string& url, std::string& body,
                     std::string& error)>
      fetcher;
};

class UpdateService {
 public:
  explicit UpdateService(UpdateServiceConfig config);
  ~UpdateService();
  UpdateService(const UpdateService&) = delete;
  UpdateService& operator=(const UpdateService&) = delete;

  // 起后台循环线程：30s 后首查，成功 24h±2h，失败按 core check_retry_delay 退避。
  // 重复调用不会再起线程。
  void start();
  void stop();

  // 立即检查一次（HTTP 线程同步执行；与后台循环用同一把检查锁串行）。
  // 返回本次检查后的 state_json()。
  nlohmann::json check_now();

  nlohmann::json state_json() const;

  // 开关与「跳过此版本」（hours 会被夹在 1–720）。
  void set_options(bool auto_check, bool prefetch);
  void skip_version(const std::string& version, std::int64_t hours);

  // 当前判定快照：/api/version 派生与下载路由取产物用。
  UpdateDecision decision() const;
  bool has_decision() const;
  std::string install_id() const;
  // 「跳过此版本」窗口原值（是否过期由 core evaluate_update 判）：
  // /api/version 的本地清单分支也要受跳过约束，与远端判定同口径。
  std::string skipped_version() const;
  std::int64_t skip_until_epoch() const;

 private:
  void loop();
  void run_check();
  void persist_locked() const;  // 调用方必须持有 mutex_
  std::string next_install_id() const;

  UpdateServiceConfig config_;
  mutable std::mutex mutex_;  // 保护下面所有字段
  std::mutex check_mutex_;    // 串行化「检查」本身（HTTP 线程与循环线程并发）

  std::string install_id_;
  bool auto_check_ = true;
  bool prefetch_ = false;
  std::int64_t last_check_epoch_ = 0;
  std::int64_t next_check_epoch_ = 0;
  int consecutive_failures_ = 0;
  std::string skipped_version_;
  std::int64_t skip_until_epoch_ = 0;
  bool has_decision_ = false;
  UpdateDecision decision_;
  std::string last_error_;

  std::thread thread_;
  std::atomic<bool> stop_{false};
};

}  // namespace devcard
