// ---------------------------------------------------------------- BUILD_STR
// guard.py 从绿色包 / setup.exe 的 DevCard.exe 里搜这个静态字符串，
// 与清单 build 比对，防止清单 build 大于编译值导致客户端反复提示同一版本。
// 单独成 TU（P1 从 main.cpp 拆出），main.cpp 只管流程。
// CMake 侧 cmake/devcard_version.cmake 的 devcard_add_build_stamp() 注入两个宏：
//   DEVCARD_BUILD=<seq>      — 整数（core 里 static_cast<uint64_t> 用）
//   DEVCARD_BUILD_STR="<seq>" — 字符串（这里 kBuildStamp 拼接用）
#ifndef DEVCARD_BUILD_STR
#define STRINGIFY_(x) #x
#define STRINGIFY(x) STRINGIFY_(x)
#define DEVCARD_BUILD_STR STRINGIFY(DEVCARD_BUILD)
#endif
namespace {
constexpr const char kBuildStamp[] = "DEVCARD_BUILD=" DEVCARD_BUILD_STR;
constexpr const char *const kBuildStampRef = kBuildStamp;  // 防 LTO 丢弃
}  // namespace
