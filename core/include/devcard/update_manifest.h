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
#include <string>
#include <string_view>
#include <vector>

namespace devcard {

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

}  // namespace devcard
