#pragma once

#include <stdexcept>
#include <string>
#include <utility>

namespace devcard {

// 统一错误：code 供 HTTP 层映射，message 面向日志/用户。
class AppError : public std::runtime_error {
 public:
  AppError(std::string code, std::string message)
      : std::runtime_error(message), code_(std::move(code)) {}

  const std::string& code() const noexcept { return code_; }

 private:
  std::string code_;
};

}  // namespace devcard
