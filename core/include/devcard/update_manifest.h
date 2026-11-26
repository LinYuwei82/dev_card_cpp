#pragma once

// 更新清单 Schema v2（manifest.json）的验签与解析。
// schema/签名规则见设计稿《更新清单 Schema v2》第三、四、六节：
//  - 整个 JSON 对象去掉 signature 字段 → 按键名排序（递归）、紧凑无空白、
//    UTF-8 编码，得到 canonical bytes；
//  - Ed25519（RFC 8032）公钥对 canonical bytes 验签，signature 为 64 字节的
//    base64 标准编码。
// 公钥与 key_id 一起编译进客户端；本文件不内置公钥——公钥属于客户端
// （server）的构建常量，core 保持可测试（测试传自己的公钥）。

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace devcard {

// ---- 版本号工具 ----

// 三段式版本号（SemVer 子集，major.minor.patch）。
struct ProductVersion {
  int major = 0;
  int minor = 0;
  int patch = 0;
};

bool parse_product_version(std::string_view text, ProductVersion& out);
int compare_product_version(const ProductVersion& a, const ProductVersion& b);
std::string format_product_version(const ProductVersion& version);

// 返回编译期注入的版本号字符串（VERSION.txt 内容）。
// CMake 通过 -DDEVCARD_VERSION="1.3.0" 注入；未注入时返回 "0.0.0"。
const char* current_product_version();

// 返回编译期注入的构建号。
// CMake 通过 -DDEVCARD_BUILD=<数值> 注入；未注入时返回 0。
std::uint64_t compiled_build_number();

// ---- 清单验签与解析 ----

// 校验清单 Ed25519 签名。raw_json 是清单的原始文本（字节），pubkey_hex 是
// 32 字节公钥的 hex（64 个十六进制字符）。失败时 *error 给出人话原因。
bool verify_manifest_signature(std::string_view raw_json,
                               std::string_view pubkey_hex,
                               std::string* error = nullptr);

struct ManifestArtifact {
  std::string role;    // setup（Windows 安装版）/ portable（绿色包）/ dmg（macOS）
  std::string url;
  std::string name;    // url 末段的文件名
  std::string sha256;  // 64 位小写十六进制
  std::uint64_t size = 0;
};

// 清单里 platforms.<本平台> 的段。
struct ManifestPlatform {
  std::string version;
  std::uint64_t build = 0;  // Windows 构建号；macOS 恒 0
  std::int64_t rollout_percent = 100;  // 灰度百分比（v2 清单，默认 100 = 全量）
  std::int64_t min_os = 0;              // 最低系统版本（可选）
  std::vector<ManifestArtifact> artifacts;
};

struct SignedManifest {
  bool ok = false;
  std::string error;  // !ok 时给人话原因
  ManifestPlatform platform;
};

// 验签 + 解析一次完成：签名不过直接失败（不给半可信的解析结果）；
// 通过后只取本平台段，未知字段与未知 artifacts key 一律忽略。
SignedManifest parse_signed_manifest(std::string_view raw_json,
                                     std::string_view pubkey_hex);

// ---- 更新判定共享原语（Windows updater_win bridge 使用） ----
// 注：曾有过一个 evaluate_manifest 汇总判定函数，但生产代码（updater_win 与
// server /api/version）各自内联了判定流程，无人调用且语义已漂移，已删除。

// 灰度分桶：SHA-256(install_id + ":" + build) 前 4 字节 mod 100。
int rollout_bucket(std::string_view install_id, std::uint64_t build);
bool rollout_allows(int percent, int bucket);

// 清单检查失败后的退避间隔（秒）：第 1 次 1 分钟，第 2 次 10 分钟，之后 1 小时。
std::int64_t check_retry_delay(int failures);

// 点分版本比较（如 10.0 < 11.0）；无法解析时按 0 处理。
int compare_dotted_version(std::string_view a, std::string_view b);

}  // namespace devcard
