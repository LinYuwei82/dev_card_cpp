// M7 并发压测入口：真起服务，多线程打登录与业务接口，报吞吐与状态分布。
// 运行：devcard_bench_http [线程数=16] [秒数=5]
// 退出码：0 无 5xx、本机来源无 429、第二实例被拒；1 任一项不满足；3 环境不具备。
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "devcard/error.h"
#include "devcard/paths.h"
#include "devcard/server.h"
#include "httplib.h"

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

static std::string seed_password(const fs::path& data_dir,
                                 const std::string& user) {
  std::ifstream in(data_dir / devcard::path_from_utf8("初始账号密码.txt"),
                   std::ios::binary);
  const std::string prefix = user + "：";
  std::string line;
  while (std::getline(in, line)) {
    if (line.rfind(prefix, 0) == 0) return line.substr(prefix.size());
  }
  return {};
}

int main(int argc, char** argv) {
  // 服务启动要求字体随包；压测直接指向仓库里的子集字体，避免依赖部署布局。
#ifdef _WIN32
  _putenv_s("DEVCARD_FONT_DIR", DEVCARD_WEB_FONTS_DIR);
#else
  setenv("DEVCARD_FONT_DIR", DEVCARD_WEB_FONTS_DIR, 1);
#endif
  const int workers = argc > 1 ? std::atoi(argv[1]) : 16;
  const int seconds = argc > 2 ? std::atoi(argv[2]) : 5;
  if (workers < 1 || seconds < 1) {
    std::cerr << "用法：devcard_bench_http [线程数] [秒数]\n";
    return 3;
  }

  const auto stamp = Clock::now().time_since_epoch().count();
  const fs::path data_dir =
      fs::temp_directory_path() / ("devcard-load-" + std::to_string(stamp));
  std::error_code ec;
  fs::create_directories(data_dir, ec);
  if (ec) {
    std::cerr << "[环境不具备] 无法创建临时目录：" << ec.message() << "\n";
    return 3;
  }

  std::unique_ptr<devcard::ServerHandle> server;
  std::string start_error;
  for (int port = 18500; port <= 18530 && !server; ++port) {
    devcard::ServerOptions options;
    options.data_dir = data_dir;
    options.port = port;
    try {
      server = devcard::start_server(options);
    } catch (const std::exception& error) {
      start_error = error.what();
    }
  }
  if (!server) {
    std::cerr << "[环境不具备] 起服务失败（端口全占用或初始化失败）："
              << start_error << "\n";
    fs::remove_all(data_dir, ec);
    return 3;
  }
  const std::string password = seed_password(data_dir, "root");
  if (password.empty()) {
    std::cerr << "[环境不具备] 读不到初始密码\n";
    server.reset();
    fs::remove_all(data_dir, ec);
    return 3;
  }

  // 先完成强制改密，否则业务接口会被 must_change_password 拦住。
  std::string new_password = "LoadTest123";
  {
    httplib::Client client("127.0.0.1", server->port());
    client.set_connection_timeout(5, 0);
    client.set_read_timeout(10, 0);
    const auto login = client.Post(
        "/api/session/login",
        "{\"username\":\"root\",\"password\":\"" + password + "\"}",
        "application/json");
    if (!login || login->status != 200) {
      std::cerr << "[环境不具备] 初始登录失败："
                << (login ? login->status : -1) << "\n";
      server.reset();
      fs::remove_all(data_dir, ec);
      return 3;
    }
    const std::string raw = login->get_header_value("Set-Cookie");
    const std::string cookie = raw.substr(0, raw.find(';'));
    httplib::Headers headers = {{"Cookie", cookie},
                                {"X-Requested-With", "DevCard"}};
    const auto change = client.Post("/api/users/password", headers,
                                    R"({"new_password":"LoadTest123"})",
                                    "application/json");
    if (!change || change->status != 200) {
      std::cerr << "[环境不具备] 首次改密失败："
                << (change ? change->status : -1) << "\n";
      server.reset();
      fs::remove_all(data_dir, ec);
      return 3;
    }
  }

  std::atomic<long long> total{0};
  std::atomic<long long> logins{0};
  std::mutex dist_mutex;
  std::map<int, long long> distribution;

  const auto deadline = Clock::now() + std::chrono::seconds(seconds);
  const auto begin = Clock::now();
  std::vector<std::thread> threads;
  threads.reserve(workers);
  for (int i = 0; i < workers; ++i) {
    threads.emplace_back([&]() {
      httplib::Client client("127.0.0.1", server->port());
      client.set_connection_timeout(5, 0);
      client.set_read_timeout(10, 0);
      const auto login = client.Post(
          "/api/session/login",
          "{\"username\":\"root\",\"password\":\"LoadTest123\"}",
          "application/json");
      int status = login ? login->status : -1;
      {
        std::lock_guard<std::mutex> lock(dist_mutex);
        ++distribution[status];
      }
      total.fetch_add(1);
      ++logins;
      if (!login || login->status != 200) return;
      const std::string raw = login->get_header_value("Set-Cookie");
      const std::string cookie = raw.substr(0, raw.find(';'));
      httplib::Headers headers = {{"Cookie", cookie}};
      while (Clock::now() < deadline) {
        // 每个请求都新建连接：keep-alive 复用会在服务端关闭连接时产生假的
        // 连接失败（-1），把「客户端复用」和「服务端能力」混在一起。
        httplib::Client request_client("127.0.0.1", server->port());
        request_client.set_connection_timeout(2, 0);
        request_client.set_read_timeout(5, 0);
        const auto response = request_client.Get("/api/devices?page=1&page_size=20",
                                                 headers);
        const int code = response ? response->status : -1;
        {
          std::lock_guard<std::mutex> lock(dist_mutex);
          ++distribution[code];
        }
        total.fetch_add(1);
      }
    });
  }
  for (auto& thread : threads) thread.join();
  const double elapsed =
      std::chrono::duration<double>(Clock::now() - begin).count();

  long long errors_5xx = 0;
  long long too_many = 0;
  long long conflicts = 0;
  long long ok = 0;
  long long conn_fail = 0;
  std::printf("并发压测：%d 线程 / %.1f 秒，来源 127.0.0.1\n", workers, elapsed);
  std::printf("  请求总数：%lld，吞吐：%.1f req/s，登录次数：%lld\n",
              total.load(), total.load() / elapsed, logins.load());
  for (const auto& item : distribution) {
    std::printf("  状态 %4d ：%lld\n", item.first, item.second);
    if (item.first == 200) ok += item.second;
    if (item.first >= 500) errors_5xx += item.second;
    if (item.first == 429) too_many += item.second;
    if (item.first == 409) conflicts += item.second;
    if (item.first == -1) conn_fail += item.second;
  }
  std::printf("  错误分布：5xx=%lld  429=%lld  409=%lld  连接失败=%lld  2xx=%lld\n",
              errors_5xx, too_many, conflicts, conn_fail, ok);

  // 三层多实例防护：同一数据目录再起一个实例，必须被拒。
  bool guarded = false;
  std::string guard_detail;
  try {
    devcard::ServerOptions second;
    second.data_dir = data_dir;
    second.port = 18560;
    auto duplicate = devcard::start_server(second);
    guard_detail = "第二个实例竟然启动成功（防护未触发）";
    if (duplicate) duplicate.reset();
  } catch (const devcard::AppError& error) {
    guard_detail = error.code() + "：" + error.what();
    guarded = error.code() == "instance_in_use";
  } catch (const std::exception& error) {
    guard_detail = error.what();
  }
  std::printf("  多实例防护：%s（%s）\n", guarded ? "已触发" : "未触发",
              guard_detail.c_str());

  server.reset();
  fs::remove_all(data_dir, ec);

  const bool pass = errors_5xx == 0 && too_many == 0 && guarded && conn_fail == 0;
  std::printf("结论：%s\n",
              pass ? "达标（无 5xx、本机来源无 429、无连接失败、第二实例被拒）"
                   : "未达标");
  return pass ? 0 : 1;
}
