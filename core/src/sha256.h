#pragma once

#include <string>
#include <string_view>

namespace devcard {

std::string sha256_hex(std::string_view input);

}  // namespace devcard
