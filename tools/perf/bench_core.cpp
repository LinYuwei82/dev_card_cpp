// §10.7 性能预算的本机基准入口（冷启动 / 1 万行查询 / 5000 行导入）。
// 运行：devcard_bench_core（Release 构建），可重复跑；每次用新的临时目录。
// 退出码：0 三项全部达标；1 有未达标；3 环境不具备（临时目录建不出来）。
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include "devcard/dao/device_dao.h"
#include "devcard/db.h"
#include "devcard/migrations.h"
#include "devcard/paths.h"
#include "devcard/service/device_service.h"
#include "devcard/xlsx.h"

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

static double ms_since(Clock::time_point start) {
  return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

int main() {
  const auto stamp =
      std::chrono::steady_clock::now().time_since_epoch().count();
  const fs::path dir =
      fs::temp_directory_path() / ("devcard-bench-" + std::to_string(stamp));
  std::error_code ec;
  fs::create_directories(dir, ec);
  if (ec) {
    std::cerr << "[环境不具备] 无法创建临时目录：" << ec.message() << "\n";
    return 3;
  }

  std::cout << "环境：本机 " << (sizeof(void*) == 8 ? "x64" : "x86")
            << " / Release；临时目录 " << devcard::path_to_utf8(dir) << "\n";
  std::cout << "口径：三项均为单次冷跑（进程内首次调用），阈值取 §10.7 预算\n";

  bool all_pass = true;
  auto report = [&all_pass](const char* name, double ms, double limit) {
    const bool pass = ms < limit;
    all_pass = all_pass && pass;
    std::printf("  %-34s %9.2f ms（阈值 %6.0f ms）→ %s\n", name, ms, limit,
                pass ? "达标" : "未达标");
  };

  // 1) 冷启动：打开新库 + 跑完全部迁移。这是「冷启动到首屏」里服务端那一段的
  //    可复现代理；窗口/WebView 启动时间不在这里，别把它当成整条链路的数字。
  {
    const auto start = Clock::now();
    devcard::Db db(dir / "cold.db");
    devcard::migrate(db);
    report("冷启动（开库 + 迁移到 vN）", ms_since(start), 1000.0);
  }

  // 2) 1 万行查询：单事务装载 10000 行，再量首屏分页（20 条）。
  {
    devcard::Db db(dir / "devicedb.db");
    devcard::migrate(db);
    devcard::DeviceService service(db);
    const auto seed_start = Clock::now();
    // 走 DAO 的批量插入（单事务），不要在这里再套一层 service.create。
    devcard::DeviceDao dao;
    db.write([&dao](sqlite3* conn) {
      for (int i = 0; i < 10000; ++i) {
        devcard::Device device;
        device.dev_name = "设备" + std::to_string(i);
        device.location = "B" + std::to_string(i % 20 + 1) + "栋";
        device.control_range = "1-" + std::to_string(i % 10 + 1) + "层";
        char phone[16] = {};
        std::snprintf(phone, sizeof(phone), "1380000%04d", i % 10000);
        device.phone = phone;
        dao.insert(conn, device);
      }
    });
    const double seed_ms = ms_since(seed_start);
    std::printf("  （装载 10000 行耗时 %.2f ms，仅作参考）\n", seed_ms);

    devcard::DeviceQuery query;
    query.page = 0;
    query.page_size = 20;
    const auto start = Clock::now();
    const devcard::DevicePage page = service.list(query);
    const double list_ms = ms_since(start);
    if (page.total_all != 10000) {
      std::cerr << "[失败] 装载后总数不是 10000，而是 " << page.total_all << "\n";
      all_pass = false;
    }
    report("1 万行首屏查询（20 条分页）", list_ms, 50.0);

    devcard::DeviceQuery keyword = query;
    keyword.keyword = "设备9999";
    const auto keyword_start = Clock::now();
    const devcard::DevicePage hit = service.list(keyword);
    const double keyword_ms = ms_since(keyword_start);
    if (hit.total_all != 10000 || hit.total != 1) {
      std::cerr << "[失败] 关键字查询命中数异常：total=" << hit.total
                << " total_all=" << hit.total_all << "\n";
      all_pass = false;
    }
    report("1 万行关键字查询（单条命中）", keyword_ms, 50.0);
  }

  // 3) 5000 行导入：造 xlsx → 重新解析 → import_rows。
  {
    devcard::Db db(dir / "import.db");
    devcard::migrate(db);
    devcard::DeviceService service(db);
    devcard::xlsx::Sheet sheet;
    sheet.name = "设备台账";
    sheet.rows.push_back(devcard::DeviceService::expected_header());
    for (int i = 0; i < 5000; ++i) {
      sheet.rows.push_back({"导入设备" + std::to_string(i),
                            "C" + std::to_string(i % 30 + 1) + "栋",
                            "1-" + std::to_string(i % 10 + 1) + "层",
                            "139" + std::to_string(10000000 + i)});
    }
    const std::string bytes = devcard::xlsx::write_workbook({sheet});
    const auto start = Clock::now();
    const auto rows = devcard::xlsx::read(bytes);
    const devcard::ImportReport report_row = service.import_rows(rows);
    const double import_ms = ms_since(start);
    if (report_row.imported != 5000) {
      std::cerr << "[失败] 导入行数不是 5000，而是 " << report_row.imported << "\n";
      all_pass = false;
    }
    report("5000 行导入（含 xlsx 解析）", import_ms, 3000.0);
  }

  fs::remove_all(dir, ec);
  std::cout << (all_pass ? "结论：三项全部达标" : "结论：存在未达标项")
            << "\n";
  return all_pass ? 0 : 1;
}
