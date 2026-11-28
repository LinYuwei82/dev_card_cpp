#pragma once

// 错误码 → HTTP 状态的唯一映射表（实现见 error_status.cpp）。
// 公共查询接口（http_status_for / http_status_known / route_error_codes /
// mapped_error_codes）仍由 devcard/server.h 声明，供测试与壳使用。

namespace devcard {

class AppError;

namespace detail {

// 路由异常的统一出口：等价于 http_status_for(error.code())。
int status_for(const AppError& error);

}  // namespace detail
}  // namespace devcard
