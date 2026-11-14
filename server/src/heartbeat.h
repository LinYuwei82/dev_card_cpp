#pragma once

#include <string>

namespace devcard {

class Db;

// 多实例第三层：库内心跳行。
// 锁文件（第 1 层）和库文件身份互斥体（第 2 层）都只能在本机/本会话内生效；
// 两个进程通过 hardlink、或 macOS 上两条不同路径指向同一个 .db 时，
// 只有库里的「谁在用、最后一次心跳是什么时候」能判出冲突。
//
// 判活口径：
//   - 同一台机器（host 相同）：PID 已不存在 → 视为崩溃残留，可立即接管；
//   - 跨机器：只看 heartbeat_at，超过 kStaleAfterSeconds 视为残留；
//   - 仍活跃 → claim 返回 false，调用方拒绝启动。
bool claim_heartbeat(Db& db, const std::string& instance_id, std::string* error);

// 定时刷新 heartbeat_at；若发现行里的 instance_id 已不是自己（被接管）返回 false。
bool refresh_heartbeat(Db& db, const std::string& instance_id);

// 恢复备份会连同旧心跳行一起还原，恢复后由当前实例无条件重写。
void force_heartbeat(Db& db, const std::string& instance_id);

// 绑定端口成功后回写监听地址，供第二个实例的报错使用。
void set_heartbeat_listen(Db& db, const std::string& instance_id,
                         const std::string& listen);

// 只删除属于自己的那一行，避免误删后来者的心跳。
void release_heartbeat(Db& db, const std::string& instance_id);

// 心跳周期与过期阈值（秒）。刷新间隔 × 3 = 过期时间。
constexpr int kHeartbeatIntervalSeconds = 15;
constexpr int kStaleAfterSeconds = 45;

}  // namespace devcard
