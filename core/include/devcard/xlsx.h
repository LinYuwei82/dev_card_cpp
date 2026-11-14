#pragma once

#include <string>
#include <vector>

namespace devcard::xlsx {

using Rows = std::vector<std::vector<std::string>>;

struct Sheet {
  std::string name;
  Rows rows;
};

// 读取第一个工作表的全部单元格。失败抛 AppError("bad_xlsx", ...)。
Rows read(const std::string& bytes);

// 生成只有一个工作表的 xlsx。单元格一律写成内联字符串。
std::string write(const Rows& rows);

// 生成多工作表 xlsx（第一个工作表即 read() 读取的那个）。
std::string write_workbook(const std::vector<Sheet>& sheets);

}  // namespace devcard::xlsx
