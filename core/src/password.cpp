#include "devcard/password.h"

#include <argon2.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <random>
#include <vector>

#include "devcard/error.h"

namespace devcard {
namespace {

constexpr std::uint32_t kTimeCost = 3;
constexpr std::uint32_t kMemoryKib = 64 * 1024;
constexpr std::uint32_t kParallelism = 1;
constexpr std::uint32_t kSaltBytes = 16;
constexpr std::uint32_t kHashBytes = 32;
constexpr char kPasswordAlphabet[] =
    "abcdefghijkmnpqrstuvwxyz23456789ABCDEFGHJKLMNPQRSTUVWXYZ";

std::string random_bytes(std::size_t size) {
  std::random_device device;
  std::string bytes(size, '\0');
  for (char& byte : bytes) byte = static_cast<char>(device() & 0xFF);
  return bytes;
}

}  // namespace

std::string hash_password(std::string_view plain) {
  if (plain.empty()) throw AppError("weak_password", "口令不能为空");
  const std::string salt = random_bytes(kSaltBytes);
  const std::size_t capacity =
      argon2_encodedlen(kTimeCost, kMemoryKib, kParallelism, kSaltBytes,
                        kHashBytes, Argon2_id) +
      1;
  std::vector<char> encoded(capacity, '\0');
  const int rc = argon2id_hash_encoded(
      kTimeCost, kMemoryKib, kParallelism, plain.data(), plain.size(),
      salt.data(), salt.size(), kHashBytes, encoded.data(), encoded.size());
  if (rc != ARGON2_OK) {
    throw AppError("password_hash", argon2_error_message(rc));
  }
  return std::string(encoded.data());
}

bool verify_password(std::string_view encoded, std::string_view plain) {
  if (encoded.empty() || plain.empty()) return false;
  return argon2id_verify(encoded.data(), plain.data(), plain.size()) == ARGON2_OK;
}

std::string random_password(std::size_t length) {
  std::random_device device;
  constexpr std::size_t alphabet_size = sizeof(kPasswordAlphabet) - 1;
  std::string password;
  password.reserve(length);
  for (std::size_t i = 0; i < length; ++i) {
    password.push_back(kPasswordAlphabet[device() % alphabet_size]);
  }
  return password;
}

}  // namespace devcard
