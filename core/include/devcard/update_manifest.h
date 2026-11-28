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

// 清单验签公钥（Ed25519，32 字节 hex）：全仓唯一一份（门禁 check_update_layering
// 盯住单点）。私钥只存在于发布端签名流程（tools/release），任何客户端只配验签。
inline constexpr const char* kUpdateManifestPubkeyHex =
    "3f4d9fabc122113e426b98a8f8b2312cb194e406f770de61285e41c607d22403";

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
  // 强制更新与发行说明（v2 可选字段，缺省不强制、无说明页）。
  // 语义与发布端 manifest_v2.py、win/mac 旧桥逐字段对齐：收口后全仓仅此一份。
  bool mandatory = false;
  std::string mandatory_reason;
  std::string mandatory_deadline;
  std::string notes_url;
};

struct SignedManifest {
  bool ok = false;
  std::string error;  // !ok 时给人话原因
  std::string channel;  // 顶层 channel（v2 在清单根上）
  ManifestPlatform platform;
};

// 验签 + 解析一次完成：签名不过直接失败（不给半可信的解析结果）；
// 通过后只取本平台段，未知字段与未知 artifacts key 一律忽略。
SignedManifest parse_signed_manifest(std::string_view raw_json,
                                     std::string_view pubkey_hex);

// ---- 更新判定共享原语（检测层唯一判据源，core UpdateEngine 使用） ----
// 注：曾有过一个 evaluate_manifest 汇总判定函数，但生产代码（updater_win 与
// server /api/version）各自内联了判定流程，无人调用且语义已漂移，已删除。
// 本次分层重构把判定以 evaluate_update 的形态收回 core，且这次真的有唯一调用方。

// 灰度分桶：SHA-256(install_id + ":" + build) 前 4 字节 mod 100。
int rollout_bucket(std::string_view install_id, std::uint64_t build);
bool rollout_allows(int percent, int bucket);

// 清单检查失败后的退避间隔（秒）：第 1 次 1 分钟，第 2 次 10 分钟，之后 1 小时。
std::int64_t check_retry_delay(int failures);

// 点分版本比较（如 10.0 < 11.0）；无法解析时按 0 处理。
int compare_dotted_version(std::string_view a, std::string_view b);

// 强制更新期限解析（确定性 UTC）：接受 "YYYY-MM-DD[T ]HH:MM[:SS]"，
// 可带 "Z" 或 "±HH[:MM]" 偏移；无时区后缀按 UTC。解析失败返回 false，
// 调用方必须按「已到期」处理（fail-closed，与 1.3.6 修前的浏览器分歧说再见）。
bool parse_update_deadline_utc(std::string_view raw, std::int64_t& epoch_seconds);

// 本机更新状态（判定输入）。version/build 来自编译期注入；install_id 用于
// 灰度分桶（同一台机器稳定，落盘复用）；skipped_* 记录「跳过此版本」窗口。
struct LocalUpdateState {
  std::string version;            // 本机版本号（VERSION.txt 编译期值）
  std::uint64_t build = 0;        // 本机构建号；0 = 未注入（开发构建）
  std::string channel;            // 本机订阅通道；空 = 不比通道
  std::string install_id;         // 灰度分桶 ID
  std::string skipped_version;    // 用户跳过并仍生效中的版本
  std::int64_t skip_until_epoch = 0;
  std::int64_t now_epoch = 0;     // 当前 UTC 秒
};

struct UpdateDecision {
  enum class Verdict {
    NoUpdate,        // 无更新（含已是最新、通道不符、清单字段非法）
    RolloutBlocked,  // 有更新但灰度未命中
    Skipped,         // 有更新但落在「跳过此版本」窗口内
    Available,       // 有更新，可提示/下载/安装
  };
  Verdict verdict = Verdict::NoUpdate;
  std::string reason;  // 细分原因（up_to_date/channel_mismatch/rollout/skip…）
  std::string version;
  std::uint64_t build = 0;
  ManifestArtifact artifact;  // 按平台选中的产物（win=setup，mac=dmg）
  bool has_artifact = false;  // Available 但 false 时只提示版本、不给下载
  bool mandatory = false;           // 清单声明了强制更新
  bool mandatory_active = false;    // 期限已到（含无期限/期限非法 fail-closed）
  std::string mandatory_reason;
  std::string mandatory_deadline;
  std::int64_t mandatory_deadline_epoch = 0;  // 0 = 无期限
  std::string notes_url;
};

// 唯一一份更新判定：channel → 版本/build 双键 → mandatory 直通灰度与跳过
// （raw mandatory 标志即豁免，不等到期）→ 灰度 → 跳过 → 选产物。
// 历史上 win 壳与 server 曾各有一份内联判定（语义漂移的根源），v1.4.0 起
// 收口为唯一调用方 core UpdateEngine，win/mac 壳只经引擎拿结论（门禁防回潮）。
UpdateDecision evaluate_update(const SignedManifest& manifest,
                               const LocalUpdateState& local);

}  // namespace devcard
