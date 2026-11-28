#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>

namespace devcard {

// 自更新阶段一：只下载与校验，不做替换。
// 下载器可注入（离线测试用）；生产默认调用系统 curl。

// 单个更新包上限：GitHub Release 的 dmg/zip 目前都在 80 MB 以内，留足余量。
inline constexpr std::uint64_t kUpdateMaxBytes = 300ull * 1024 * 1024;

// 只允许 https，且 host 必须命中固定白名单；不接受 userinfo（@）与非默认端口。
// 这样「下载地址」不能通过 update.json 指到任意主机。
bool is_trusted_update_url(std::string_view url);

// 版本号只接受 [0-9A-Za-z._-]，1–40 个字符。
bool is_valid_update_version(std::string_view version);

// 字符串是否为 64 位十六进制（大小写不限）。
bool is_valid_sha256(std::string_view value);

// 取 basename，非法字符替换为 '_'，去掉结尾的点与空格；空 / "." / ".." 回落 update.bin。
std::string sanitize_update_file_name(std::string_view raw);

// 下载中的 part 路径：`<updates>/<sanitized>.part`。
// 服务端轮询进度与下载器落盘必须共用这一处，否则两边各算一次会出现
// 「curl 写 A、轮询读 B」→ 进度永远 0。文件名恒为 ASCII（sanitize 保证）。
std::filesystem::path update_part_path(const std::filesystem::path& updates_dir,
                                       std::string_view file_name);

struct UpdatePackageSpec {
  std::string version;    // 清单里的版本（用于落盘命名与状态展示）
  std::string file_name;  // 资产文件名（来自清单）
  std::string url;        // 资产下载地址（必须是白名单内的 https）
  std::string sha256;     // 清单给的 SHA256（十六进制）
  std::uint64_t size = 0; // 清单给的字节数
};

// 返回空串表示合法；否则是可读原因，可直接给用户看。
std::string validate_update_spec(const UpdatePackageSpec& spec);

// 与 validate_update_spec 对应的稳定错误码：
// endpoint_not_allowed / manifest_invalid；合法时返回空串。
std::string validate_update_spec_code(const UpdatePackageSpec& spec);

struct UpdateVerification {
  bool ok = false;
  // manifest_invalid / download_failed（大小或读盘）/ checksum_mismatch
  std::string code;
  std::string error;
  std::uint64_t actual_size = 0;
  std::string actual_sha256;
};

// 字节数与 SHA256 两项都校验，缺一不可。
UpdateVerification verify_update_file(const std::filesystem::path& file,
                                      std::uint64_t expected_size,
                                      std::string_view expected_sha256);

struct UpdateDownloadOutcome {
  bool ok = false;
  std::string path;
  // 稳定错误码：endpoint_not_allowed / manifest_invalid / download_failed /
  // checksum_mismatch。UI 只展示 error，HTTP 层用它选状态码。
  std::string error_code;
  std::string error;
  std::uint64_t bytes = 0;
  std::string sha256;
};

using UpdateFetcher = std::function<bool(const std::string& url,
                                         const std::filesystem::path& target,
                                         std::string* error,
                                         std::string* error_code)>;

// 生产下载器：系统 curl，固定参数、不经 shell、只允许 https（含重定向）。
// 命令行里只有 ASCII：工作目录设为 target 的父目录，`-o` 只用文件名。
// 目标文件名必须已 sanitize 成 ASCII：非 ASCII 时直接失败，不退回到固定临时名——
// 那样 curl 写的路径和被轮询的 part 路径不是同一个，进度会静默停在 0。
bool curl_fetch(const std::string& url, const std::filesystem::path& target,
                std::string* error, std::string* error_code);

// 下载 + 双校验 + 原子改名（`<name>.part` → `<name>`）。
// 任一步失败都删除残件，错误信息可直接展示；不抛异常。
UpdateDownloadOutcome download_update_package(
    const std::filesystem::path& updates_dir, const UpdatePackageSpec& spec,
    const UpdateFetcher& fetcher = curl_fetch);

// 把已校验的包从 updates/ 挪到用户选定位置：同卷走 rename（原子）；
// 跨卷退回「目标旁的 .part → 改名 → 删源」，任何一步失败都不留下残件，
// 源文件保持原样，错误可直接展示。
bool relocate_update_package(const std::filesystem::path& source,
                             const std::filesystem::path& target,
                             std::string* error);

}  // namespace devcard
