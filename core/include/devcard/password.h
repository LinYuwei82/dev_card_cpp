#pragma once

#include <string>
#include <string_view>

namespace devcard {

// Argon2id：m=64MiB, t=3, p=1，返回 PHC 编码串（含盐与参数）。
std::string hash_password(std::string_view plain);

// 校验口令；编码串为空或格式不合法时返回 false，不抛异常。
bool verify_password(std::string_view encoded, std::string_view plain);

// 生成一次性随机口令（12 位，去掉易混淆字符）。
std::string random_password(std::size_t length = 12);

}  // namespace devcard
