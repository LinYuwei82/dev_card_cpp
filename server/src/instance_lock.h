#pragma once

#include <filesystem>
#include <memory>
#include <string>

namespace devcard {

// 多实例防护（同一台机器两个 server 抢同一个库必须明确拒绝）：
// 1) 规范化数据目录 + devcard.lock 独占锁：junction/symlink/8.3/大小写差异
//    规范化后指向同一个锁文件；跨 Windows 会话也由文件句柄互斥。
// 2) 库文件身份互斥体（Windows）：两个不同目录里的 devicedb.db 是同一个文件
//    （hardlink）时，路径锁看不出来，用卷序列号 + 文件索引兜底。
// 任一层判定冲突即拒绝启动；main 把错误码 instance_in_use 映射为退出码 3。
class InstanceGuard {
 public:
  static std::unique_ptr<InstanceGuard> acquire(
      const std::filesystem::path& data_dir, std::string* error);
  ~InstanceGuard();

  InstanceGuard(const InstanceGuard&) = delete;
  InstanceGuard& operator=(const InstanceGuard&) = delete;

  // 数据库建好后调用；同一文件已被另一个路径的实例打开时返回 false。
  bool claim_database(const std::filesystem::path& db_file, std::string* error);

  // 监听地址确定后回写锁文件，供第二个实例报出「占用者：PID…监听…」。
  void set_owner_info(const std::string& info);

 private:
  InstanceGuard();

  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace devcard
