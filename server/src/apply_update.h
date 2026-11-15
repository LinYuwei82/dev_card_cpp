#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace devcard {

// 自更新阶段二（仅 Windows 安装版）：替换动作交给独立协助进程，
// 不在运行中的进程里自替换 exe。协助器就是 devcard-server.exe 自己的副本，
// 通过 --apply-update 进入，不新增可执行目标。
struct ApplyUpdateOptions {
  std::filesystem::path setup;     // 已双重校验通过的 Inno 安装包
  unsigned long wait_pid = 0;      // 等这个进程退出后再开始安装（避免文件占用）
  std::filesystem::path result;    // 安装结果 JSON（原子写）
  std::filesystem::path relaunch;  // 装完/装失败后各拉起一次（同一路径）
  // 原进程的参数：新版本必须按原来的部署方式回来（--server / --port / --data-dir），
  // 否则重启后会落到默认数据目录，等于换了一份台账。
  std::vector<std::string> relaunch_args;
};

// 当前进程可执行文件全路径；取不到返回空。
std::filesystem::path current_executable();

// 当前进程参数（不含 argv[0]，UTF-8）。
std::vector<std::string> current_arguments();

// 当前进程 PID；非 Windows 返回 0。
unsigned long current_process_id();

// 读 PE 版本资源里的 ProductVersion（不是 VERSION.txt 的旁路副本，
// 而是编译期由 version.rc.in 从 VERSION.txt 生成的那份资源）；读不到返回空串。
std::string read_product_version(const std::filesystem::path& exe);

// 以分离进程启动协助器。失败抛 AppError("install_failed", ...)。
void spawn_apply_update(const std::filesystem::path& helper,
                        const ApplyUpdateOptions& options);

// 协助器主体：等父进程退出 → 记录升级前版本 → 跑安装器并取退出码
// → 记录升级后版本 → 原子写结果 JSON → 拉起目标程序。
// 返回进程退出码（安装成败写在 options.result 里，不靠本进程退出码表达）。
int run_apply_update(const ApplyUpdateOptions& options);

// 阶段二判据：apply-state.json 只记四态。interrupted 刻意不落盘——
// 它是「启动时读到 installing 且超过阈值」推出来的，不是第五个状态。
enum class ApplyState { Installing, Verifying, Done, Failed };

constexpr int kApplyInterruptSeconds = 300;

const char* apply_state_name(ApplyState state);

// 写在 <数据目录>/updates/apply-state.json。失败返回 false 并填 error，
// 调用方必须把失败当失败（静默吞掉会让「启动即崩」这条判据假绿）。
bool write_apply_state(const std::filesystem::path& data_dir, ApplyState state,
                       const std::string& detail, std::string* error);

struct ApplyStateSnapshot {
  bool present = false;
  std::string state;
  std::string detail;
  std::string updated_at;
  long long age_seconds = 0;
};

ApplyStateSnapshot read_apply_state(const std::filesystem::path& data_dir);

// state 仍是 installing 且超过阈值 → 判为中断（协助器被杀 / 机器重启）。
bool apply_state_interrupted(const ApplyStateSnapshot& snapshot,
                             int threshold_seconds = kApplyInterruptSeconds);

// 握手标记：数据库与字体初始化完成之后、HTTP 开始服务之前写。
// 写失败必须让启动失败，否则「新版本启动即崩」判不出来。
bool write_app_started_marker(const std::filesystem::path& data_dir,
                              const std::string& version, std::string* error);

}  // namespace devcard
