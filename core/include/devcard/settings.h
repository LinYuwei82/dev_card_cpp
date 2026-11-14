#pragma once

#include <string>
#include <string_view>

#include "devcard/db.h"

namespace devcard {

// 卡片头部公司名：未写入 app_setting 时回落到这里，保证老客户升级后版式不变。
inline constexpr std::string_view kDefaultCompanyName = "深圳市安业物业管理有限公司";
// 名称显示宽度上限：汉字 1，字母/数字 0.5。默认值 13 个字，同样受限。
inline constexpr double kCompanyNameMaxWidth = 13.0;
// 录入上限（按 Unicode 字符计）：卡面仍按 13 字宽打印，录入放宽到 16 个字符，
// 超出 13 字宽的部分由界面预览明确告知后再截断。
inline constexpr std::size_t kCompanyNameInputMaxLength = 16;

// app_setting 键值对。读接口返回空串表示不存在。
std::string get_setting(const Db& db, std::string_view key);
bool has_setting(const Db& db, std::string_view key);

// 打印用公司名：未设置或为空时返回默认值。
std::string company_name(const Db& db);
// 实际印在卡片上的公司名：统一按 13 个显示宽度截断。
std::string company_print_name(const Db& db);

// 按显示宽度折算：汉字 1，ASCII 字母/数字/符号 0.5，其它非 ASCII 按 1。
double company_name_width(std::string_view text);
// 只保留累计宽度不超过 13 的前缀；不会从字符中间截断。
std::string truncate_company_name(std::string_view text);

// 写接口要求调用方已处于 Db::write() 事务内。
void set_setting(sqlite3* conn, std::string_view key, std::string_view value);

}  // namespace devcard
