#include "devcard/update_manifest.h"

#include <nlohmann/json.hpp>

#include <cstring>
#include <random>

#include "devcard/update_package.h"

extern "C" {
#include <tweetnacl/tweetnacl.h>
}

namespace devcard {
namespace {

constexpr std::size_t kSignatureBytes = 64;
constexpr std::size_t kPubkeyBytes = 32;
// 本平台在清单 platforms 下的键与可用的产物类型。
#if defined(_WIN32)
constexpr const char* kSelfPlatform = "windows";
constexpr const char* const kAllowedRoles[] = {"setup", "portable", "dmg"};
#else
constexpr const char* kSelfPlatform = "macos";
constexpr const char* const kAllowedRoles[] = {"dmg"};
#endif

int hex_nibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

bool decode_hex(std::string_view hex, unsigned char* out, std::size_t bytes) {
  if (hex.size() != bytes * 2) return false;
  for (std::size_t i = 0; i < bytes; ++i) {
    const int hi = hex_nibble(hex[i * 2]);
    const int lo = hex_nibble(hex[i * 2 + 1]);
    if (hi < 0 || lo < 0) return false;
    out[i] = static_cast<unsigned char>((hi << 4) | lo);
  }
  return true;
}

// 标准字母表 base64（发布端 base64.b64encode 带 '=' padding）；容忍内嵌空白，
// 拒绝其余一切字符。
bool decode_base64(std::string_view text, unsigned char* out,
                   std::size_t bytes) {
  static const char* const kAlphabet =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  int buffer = 0;
  int bits = 0;
  std::size_t written = 0;
  int padding = 0;
  for (const char c : text) {
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
    if (c == '=') {
      if (++padding > 2) return false;
      continue;
    }
    if (padding > 0) return false;  // padding 后不得再出现数据字符
    const char* found = std::strchr(kAlphabet, c);
    if (!found || c == '\0') return false;
    buffer = (buffer << 6) | static_cast<int>(found - kAlphabet);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      if (written >= bytes) return false;
      out[written++] = static_cast<unsigned char>((buffer >> bits) & 0xFF);
    }
  }
  // padding 数量由编码字节数唯一决定（64 字节 → 尾组 1 字节 → 2 个 '='）。
  const std::size_t expected_padding = (3 - bytes % 3) % 3;
  return written == bytes && padding == expected_padding;
}

// TweetNaCl 官方源码只声明不实现 randombytes（crypto_sign_keypair 生成密钥时
// 使用）。客户端正式路径只做验签（crypto_sign_open，不碰随机数），这里给一个
// <random> 实现兜底，供测试里生成 keypair 使用。
extern "C" void randombytes(unsigned char* out, unsigned long long count) {
  std::random_device device;
  while (count >= sizeof(unsigned int)) {
    const unsigned int value = device();
    std::memcpy(out, &value, sizeof(unsigned int));
    out += sizeof(unsigned int);
    count -= sizeof(unsigned int);
  }
  if (count > 0) {
    const unsigned int value = device();
    std::memcpy(out, &value, static_cast<std::size_t>(count));
  }
}

// canonical bytes：与发布端 manifest_v2.py 的 canonical_bytes() 必须逐字节一致。
// 用 nlohmann 实现：
//  - 对象默认以 std::map<string, json> 存储，键按字节序排（UTF-8 字节序与
//    Python sort_keys 的码点序一致），递归有序；
//  - dump() 默认无缩进、无分隔空白、ensure_ascii=false（非 ASCII 原样 UTF-8）；
//  - 整数原样输出（无小数点），转义仅 \" \\ \b \f \n \r \t 与 \u00xx（小写），
//    不转义 '/'——与 Python json.dumps(separators=(",", ":"), ensure_ascii=False)
//    的字符串转义规则相同。
// 签名是签在「去掉 signature 字段之后」的对象上。
bool canonical_bytes(std::string_view raw_json, std::string* canonical,
                     std::string* error) {
  nlohmann::json doc = nlohmann::json::parse(raw_json, nullptr, false);
  if (doc.is_discarded() || !doc.is_object()) {
    if (error) *error = "更新清单不是合法的 JSON 对象";
    return false;
  }
  if (!doc.contains("signature") || !doc["signature"].is_string()) {
    if (error) *error = "更新清单缺少 signature 签名字段";
    return false;
  }
  doc.erase("signature");
  *canonical = doc.dump();
  return true;
}

std::string signature_text(std::string_view raw_json, bool* present) {
  *present = false;
  nlohmann::json doc = nlohmann::json::parse(raw_json, nullptr, false);
  if (doc.is_discarded() || !doc.is_object() || !doc.contains("signature") ||
      !doc["signature"].is_string()) {
    return {};
  }
  *present = true;
  return doc["signature"].get<std::string>();
}

}  // namespace

bool verify_manifest_signature(std::string_view raw_json,
                               std::string_view pubkey_hex, std::string* error) {
  auto fail = [&](std::string message) {
    if (error) *error = std::move(message);
    return false;
  };
  bool present = false;
  const std::string signature_b64 = signature_text(raw_json, &present);
  if (!present) return fail("更新清单缺少 signature 签名字段");
  unsigned char sig[kSignatureBytes];
  if (!decode_base64(signature_b64, sig, kSignatureBytes)) {
    return fail("更新清单的 signature 签名不是有效的 base64（需 64 字节）");
  }
  unsigned char pk[kPubkeyBytes];
  if (!decode_hex(pubkey_hex, pk, kPubkeyBytes)) {
    return fail("内置公钥不是 64 位十六进制（客户端构建错误）");
  }
  std::string canonical;
  if (!canonical_bytes(raw_json, &canonical, error)) return false;
  // crypto_sign_open 吃的是「签名 || 消息」的 sm，返回 0 即通过。
  std::vector<unsigned char> sm(kSignatureBytes + canonical.size());
  std::memcpy(sm.data(), sig, kSignatureBytes);
  std::memcpy(sm.data() + kSignatureBytes, canonical.data(), canonical.size());
  std::vector<unsigned char> m(sm.size());
  unsigned long long mlen = 0;
  if (crypto_sign_open(m.data(), &mlen, sm.data(), sm.size(), pk) != 0) {
    return fail("更新清单签名验证失败（公钥不匹配或清单被改动）");
  }
  return true;
}

SignedManifest parse_signed_manifest(std::string_view raw_json,
                                     std::string_view pubkey_hex) {
  SignedManifest result;
  std::string error;
  if (!verify_manifest_signature(raw_json, pubkey_hex, &error)) {
    result.error = error.empty() ? "更新清单签名验证失败" : error;
    return result;
  }
  // 验签已通过，第二次 parse 拿结构化数据（清单只有几 KB，成本可忽略）。
  const nlohmann::json doc = nlohmann::json::parse(raw_json, nullptr, false);
  if (doc.is_discarded() || !doc.is_object()) {
    result.error = "更新清单不是合法的 JSON 对象";  // 理论不可达（验签已 parse 过）
    return result;
  }
  const auto version_field = doc.find("manifest_version");
  if (version_field == doc.end() || !version_field->is_number_integer() ||
      version_field->get<int>() != 2) {
    result.error = "更新清单不是 v2 格式（manifest_version 必须为 2）";
    return result;
  }
  const auto platforms = doc.find("platforms");
  if (platforms == doc.end() || !platforms->is_object()) {
    result.error = "更新清单缺少 platforms";
    return result;
  }
  const auto section = platforms->find(kSelfPlatform);
  if (section == platforms->end() || !section->is_object()) {
    result.error = std::string("更新清单缺少本平台（") + kSelfPlatform + "）条目";
    return result;
  }
  const auto version = section->find("version");
  if (version == section->end() || !version->is_string() ||
      version->get<std::string>().empty()) {
    result.error = "更新清单缺少本平台 version";
    return result;
  }
  result.platform.version = version->get<std::string>();
  const auto build = section->find("build");
  if (build != section->end() && build->is_number_integer()) {
    const long long value = build->get<long long>();
    if (value > 0) result.platform.build = static_cast<std::uint64_t>(value);
  }
  const auto artifacts = section->find("artifacts");
  if (artifacts != section->end() && artifacts->is_object()) {
    // 只认本平台的已知产物类型；未知 key 与未知字段一律忽略（schema 允许扩展）。
    for (const char* role : kAllowedRoles) {
      const auto entry = artifacts->find(role);
      if (entry == artifacts->end() || !entry->is_object()) continue;
      const auto url = entry->find("url");
      const auto size = entry->find("size");
      const auto sha256 = entry->find("sha256");
      if (url == entry->end() || !url->is_string() ||
          size == entry->end() || !size->is_number_integer() ||
          sha256 == entry->end() || !sha256->is_string()) {
        continue;
      }
      ManifestArtifact artifact;
      artifact.role = role;
      artifact.url = url->get<std::string>();
      const std::size_t slash = artifact.url.find_last_of('/');
      artifact.name = slash == std::string::npos
                          ? artifact.url
                          : artifact.url.substr(slash + 1);
      const long long bytes = size->get<long long>();
      artifact.sha256 = sha256->get<std::string>();
      // 任一关键字段不合格就跳过该产物：验签保证清单来自发布方，
      // 但字段仍可能写坏，此时宁缺毋滥（不给下载入口，版本提示照常）。
      if (artifact.name.empty() || bytes <= 0 ||
          !is_valid_sha256(artifact.sha256)) {
        continue;
      }
      artifact.size = static_cast<std::uint64_t>(bytes);
      result.platform.artifacts.push_back(std::move(artifact));
    }
  }
  result.ok = true;
  return result;
}

std::uint64_t compiled_build_number() {
#if defined(DEVCARD_BUILD_STAMP)
  return static_cast<std::uint64_t>(DEVCARD_BUILD_STAMP);
#else
  return 0;
#endif
}

}  // namespace devcard
