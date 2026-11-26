#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#ifdef _WIN32
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

#include "devcard/backup.h"
#include "devcard/dao/device_dao.h"
#include "devcard/db.h"
#include "devcard/error.h"
#include "devcard/migrations.h"
#include "devcard/password.h"
#include "devcard/paths.h"
#include "devcard/service/device_service.h"
#include "devcard/service/user_service.h"
#include "devcard/settings.h"
#include "devcard/sha256.h"
#include "devcard/update_manifest.h"
#include "devcard/update_package.h"
#include "devcard/xlsx.h"

extern "C" {
#include <tweetnacl/tweetnacl.h>
}

namespace {

int failures = 0;
// 断言计数：没有这个数，输出只有「core tests passed」，无法确认哪些断言真的跑过
// （Windows 下整段 #ifndef _WIN32 的用例根本不编译）。
int checks = 0;
int skips = 0;

#define CHECK(cond)                                                        \
  do {                                                                     \
    ++checks;                                                              \
    if (!(cond)) {                                                         \
      std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << "  " #cond     \
                << "\n";                                                   \
      ++failures;                                                          \
    }                                                                      \
  } while (0)

std::filesystem::path temp_db() {
  static std::uint64_t counter = 0;
  const auto stamp =
      std::chrono::steady_clock::now().time_since_epoch().count();
  // 根目录故意带非 ASCII 段：纯 ASCII 下 `.string()` 给出的字节与 UTF-8 相同，
  // 编码错误测不出来（Windows 上会落成 ACP 乱码）。夹具自己也走 path_from_utf8。
  const auto root = std::filesystem::temp_directory_path() /
                    devcard::path_from_utf8("devcard-test-样张-📇");
  const auto dir = root / ("run-" + std::to_string(stamp) + "-" +
                           std::to_string(counter++));
  std::filesystem::create_directories(dir);
  return dir / "devicedb.db";
}


// 删目录之前，连接必须已经关掉。
// 判据用 WAL 残留：库开了 journal_mode=WAL，连接还活着时 -wal/-shm 一定在，
// 正常关闭（最后一个连接）会 checkpoint 并删掉它们。POSIX 允许删打开中的文件，
// 所以「目录删得掉」不能当判据，-wal 残留才可以——这一条在 macOS 上也会红。
void check_db_closed(const std::filesystem::path& dir) {
  // 侧车文件直接在 path 上追加 ASCII 后缀，不经过 .string()：
  // Windows 上窄字符往返走 ACP，中文目录会被改写。
  std::filesystem::path db = dir / "devicedb.db";
  std::filesystem::path wal = db;
  wal += "-wal";
  std::filesystem::path shm = db;
  shm += "-shm";
  CHECK(!std::filesystem::exists(wal));
  CHECK(!std::filesystem::exists(shm));
}

// UTF-8 码点数：续字节（10xxxxxx）不计。
int utf8_codepoints(const std::string& text) {
  int count = 0;
  for (const unsigned char byte : text) {
    if ((byte & 0xC0) != 0x80) ++count;
  }
  return count;
}

std::filesystem::path self_executable() {
#ifdef _WIN32
  std::wstring buffer(32768, L'\0');
  const DWORD size = ::GetModuleFileNameW(nullptr, buffer.data(),
                                          static_cast<DWORD>(buffer.size()));
  buffer.resize(size);
  return std::filesystem::path(buffer);
#elif defined(__APPLE__)
  // 与 server/src/platform_paths.cpp 同一写法：先问长度再取值。
  uint32_t size = 0;
  _NSGetExecutablePath(nullptr, &size);
  std::vector<char> buffer(size + 1, '\0');
  if (_NSGetExecutablePath(buffer.data(), &size) == 0) {
    return std::filesystem::path(buffer.data());
  }
  return {};
#elif defined(__linux__)
  return std::filesystem::canonical("/proc/self/exe");
#else
  // 其余平台没有可靠的「自身可执行文件」查询方式。返回空串让调用方明确失败，
  // 不要编一个假路径——假路径会让用例在没验证的情况下变绿。
  return {};
#endif
}

void set_env(const char* name, const std::string& value) {
#ifdef _WIN32
  _putenv_s(name, value.c_str());
#else
  ::setenv(name, value.c_str(), 1);
#endif
}

// curl_fetch 调过来的替身：放在真实调用点上，不走注入的 fetcher。
// 这里拿到的 argv 是 CRT 按 ACP 从宽命令行转出来的窄串——与真实 curl.exe
// 同一层，所以「命令行里的路径能不能还原成原路径」是真跑过的。
int run_curl_stub(int argc, char** argv) {
  std::filesystem::path output;
  for (int i = 1; i + 1 < argc; ++i) {
    if (std::string(argv[i]) == "--output") output = argv[i + 1];
  }
  const char* payload = std::getenv("DEVCARD_PROBE_PAYLOAD");
  if (output.empty() || !payload) return 2;
  // 工作目录是这条设计里唯一承载非 ASCII 的地方，对不上就直接报 3，
  // 免得「文件碰巧写到了别处」被当成通过。
  const char* expect_cwd = std::getenv("DEVCARD_PROBE_EXPECT_CWD");
  if (expect_cwd) {
    std::error_code cwd_ec;
    const std::filesystem::path expected = devcard::path_from_utf8(expect_cwd);
    if (!std::filesystem::equivalent(std::filesystem::current_path(cwd_ec),
                                     expected, cwd_ec) ||
        cwd_ec) {
      return 3;
    }
  }
  std::error_code ec;
  // 环境变量里给的是 UTF-8（curl_fetch 那边就是这么约定的），这里必须按 UTF-8
  // 还原成 path；直接用 path(const char*) 会按 ACP 再解释一次。
  std::filesystem::copy_file(devcard::path_from_utf8(payload), output,
                             std::filesystem::copy_options::overwrite_existing,
                             ec);
  return ec ? 1 : 0;
}
}  // namespace

// 自更新阶段一：白名单、文件名清洗、SHA256 + 字节数双校验、失败删残件。
void run_update_package_case(const std::filesystem::path& dir) {
  using namespace devcard;
  const std::string good_sha = std::string(64, 'a');

  CHECK(is_trusted_update_url(
      "https://github.com/LinYuwei82/dev_card_cpp/releases/download/v1/DevCard.dmg"));
  CHECK(is_trusted_update_url("https://objects.githubusercontent.com/pkg"));
  // 自建 OSS 下载域：Windows 安装包与 macOS appcast 都从这里发。
  CHECK(is_trusted_update_url(
      "https://dl.xiaotu.online/devcard/DevCard-1.2.0-setup.exe"));
  CHECK(!is_trusted_update_url("https://dl.xiaotu.online.evil.example/x"));
  CHECK(!is_trusted_update_url("https://evil-dl.xiaotu.online/x"));
  CHECK(!is_trusted_update_url("http://github.com/x"));
  CHECK(!is_trusted_update_url("https://evil.example/x"));
  CHECK(!is_trusted_update_url("https://github.com.evil.example/x"));
  CHECK(!is_trusted_update_url("https://user@github.com/x"));
  CHECK(!is_trusted_update_url("https://github.com:8443/x"));
  CHECK(!is_trusted_update_url("https://github.com"));

  CHECK(sanitize_update_file_name("../../a:b?.dmg") == "a_b_.dmg");
  CHECK(sanitize_update_file_name("C:\\tmp\\pkg.zip") == "pkg.zip");
  CHECK(sanitize_update_file_name("..") == "update.bin");
  CHECK(is_valid_sha256(good_sha));
  CHECK(!is_valid_sha256("abc"));

  const std::filesystem::path fixture = dir / "fixture.bin";
  {
    std::ofstream out(fixture, std::ios::binary);
    out << "devcard-update-fixture";
  }
  const std::string hash = sha256_file(fixture);
  const std::uint64_t size =
      static_cast<std::uint64_t>(std::filesystem::file_size(fixture));
  CHECK(verify_update_file(fixture, size, hash).ok);
  CHECK(!verify_update_file(fixture, size + 1, hash).ok);
  const auto wrong_hash = verify_update_file(fixture, size, good_sha);
  CHECK(!wrong_hash.ok);
  CHECK(wrong_hash.error.find("SHA256") != std::string::npos);

  // 搬到用户选定位置：成功时源消失、目标存在；目标不可写时源保持原样、不留残件。
  {
    const std::filesystem::path src = dir / "pkg-src.dmg";
    {
      std::ofstream out(src, std::ios::binary);
      out << "payload";
    }
    const std::filesystem::path dst = dir / "chosen" / "DevCard-1.2.3.dmg";
    std::filesystem::create_directories(dst.parent_path());
    std::string move_error;
    CHECK(relocate_update_package(src, dst, &move_error));
    CHECK(move_error.empty());
    CHECK(!std::filesystem::exists(src));
    CHECK(std::filesystem::exists(dst));
    CHECK(std::filesystem::file_size(dst) == 7u);

    const std::filesystem::path src2 = dir / "pkg-src2.dmg";
    {
      std::ofstream out(src2, std::ios::binary);
      out << "keep";
    }
    const std::filesystem::path unwritable = dir / "no-such-dir" / "x.dmg";
    std::string err;
    CHECK(!relocate_update_package(src2, unwritable, &err));
    CHECK(!err.empty());
    CHECK(std::filesystem::exists(src2));
    CHECK(!std::filesystem::exists(dir / "no-such-dir" / "x.dmg.part"));
  }

  UpdatePackageSpec spec;
  spec.version = "1.2.3";
  spec.file_name = "../DevCard-1.2.3.zip";
  spec.url =
      "https://github.com/example/releases/download/v1.2.3/DevCard-1.2.3.zip";
  spec.sha256 = hash;
  spec.size = size;
  CHECK(validate_update_spec(spec).empty());
  {
    UpdatePackageSpec insecure = spec;
    insecure.url = "http://github.com/example/pkg.zip";
    CHECK(!validate_update_spec(insecure).empty());
  }

  const std::filesystem::path updates = dir / "updates";
  auto copy_fetcher = [&fixture](const std::string&,
                                 const std::filesystem::path& target,
                                 std::string*, std::string*) {
    std::filesystem::copy_file(fixture, target,
                               std::filesystem::copy_options::overwrite_existing);
    return true;
  };
  const UpdateDownloadOutcome ok =
      download_update_package(updates, spec, copy_fetcher);
  CHECK(ok.ok);
  CHECK(ok.path == devcard::path_to_utf8(updates / "DevCard-1.2.3.zip"));
  CHECK(std::filesystem::exists(devcard::path_from_utf8(ok.path)));
  // 调用点判据（不是 helper 判据）：数据目录含中文时，产物必须真的落在
  // <data_dir>/updates/ 下。只看返回字符串会让「helper 全绿、调用点没改」
  // 蒙混过关（`target.string()` 交给 curl 就是这样下到错路径的）。
  // 所以枚举目录项、逐码点比文件名与父路径。
  bool asset_on_disk = false;
  bool asset_parent_ok = false;
  for (const auto& entry : std::filesystem::directory_iterator(updates)) {
    if (devcard::path_to_utf8(entry.path().filename()) != "DevCard-1.2.3.zip") {
      continue;
    }
    asset_on_disk = true;
    asset_parent_ok = devcard::path_to_utf8(entry.path().parent_path()) ==
                      devcard::path_to_utf8(updates);
  }
  CHECK(asset_on_disk);
  CHECK(asset_parent_ok);
  CHECK(!std::filesystem::exists(updates / "DevCard-1.2.3.zip.part"));
  CHECK(ok.sha256 == hash);
  CHECK(ok.bytes == size);

  const std::filesystem::path failed_dir = dir / "updates-failed";
  auto offline_fetcher = [](const std::string&,
                            const std::filesystem::path& target,
                            std::string* error, std::string*) {
    std::ofstream out(target, std::ios::binary);
    out << "partial";
    if (error) *error = "模拟断网";
    return false;
  };
  const UpdateDownloadOutcome offline =
      download_update_package(failed_dir, spec, offline_fetcher);
  CHECK(!offline.ok);
  CHECK(offline.error.find("模拟断网") != std::string::npos);
  CHECK(!std::filesystem::exists(failed_dir / "DevCard-1.2.3.zip.part"));
  CHECK(!std::filesystem::exists(failed_dir / "DevCard-1.2.3.zip"));

  auto corrupt_fetcher = [](const std::string&,
                            const std::filesystem::path& target,
                            std::string*, std::string*) {
    std::ofstream out(target, std::ios::binary);
    out << "corrupted";
    return true;
  };
  const UpdateDownloadOutcome corrupt =
      download_update_package(failed_dir, spec, corrupt_fetcher);
  CHECK(!corrupt.ok);
  CHECK(corrupt.error.find("校验失败") != std::string::npos);
  CHECK(!std::filesystem::exists(failed_dir / "DevCard-1.2.3.zip.part"));
  CHECK(!std::filesystem::exists(failed_dir / "DevCard-1.2.3.zip"));

  // 进度判据（不是字段判据）：服务端轮询读的是 `update_part_path()`，
  // 所以这里就盯着这个路径取 file_size，必须在 curl 还没退出时读到非 0。
  // 两条输入各跑一遍：
  //   1) 包名纯 ASCII → part 名就是原名；
  //   2) 包名含中文 → sanitize 成 ASCII，part 名全 ASCII。
  // 两组的 curl 命令行都必须无中文（目录本身是中文，靠工作目录承载）。
#ifndef _WIN32
  {
    const std::filesystem::path fake = dir / "fake-curl.sh";
    const std::filesystem::path fake_argv = dir / "fake-curl-argv.txt";
    {
      std::ofstream out(fake);
      out << "#!/bin/sh\n"
             "if [ -n \"$DEVCARD_FAKE_ARGV_LOG\" ]; then\n"
             "  for arg in \"$@\"; do printf '%s\\n' \"$arg\"; done"
             " > \"$DEVCARD_FAKE_ARGV_LOG\"\n"
             "fi\n"
             "out=\"\"\n"
             "log=\"\"\n"
             "while [ \"$#\" -gt 0 ]; do\n"
             "  case \"$1\" in\n"
             "    --output) out=\"$2\"; shift 2 ;;\n"
             "    --stderr) log=\"$2\"; shift 2 ;;\n"
             "    *) shift ;;\n"
             "  esac\n"
             "done\n"
             "[ -n \"$out\" ] || exit 2\n"
             "[ -n \"$DEVCARD_FAKE_RELEASE\" ] || exit 4\n"
             "head -c 8 \"$DEVCARD_FAKE_SOURCE\" > \"$out\"\n"
             "i=0\n"
             "while [ ! -f \"$DEVCARD_FAKE_RELEASE\" ] && [ \"$i\" -lt 500 ];"
             " do sleep 0.02; i=$((i+1)); done\n"
             "tail -c +9 \"$DEVCARD_FAKE_SOURCE\" >> \"$out\"\n"
             "[ -z \"$log\" ] || printf 'fake curl ok\\n' > \"$log\"\n"
             "exit 0\n";
    }
    std::filesystem::permissions(fake,
                                 std::filesystem::perms::owner_read |
                                     std::filesystem::perms::owner_write |
                                     std::filesystem::perms::owner_exec,
                                 std::filesystem::perm_options::replace);
    ::setenv("DEVCARD_CURL", fake.c_str(), 1);
    ::setenv("DEVCARD_FAKE_SOURCE", fixture.c_str(), 1);
    ::setenv("DEVCARD_FAKE_ARGV_LOG", fake_argv.c_str(), 1);

    const auto run_curl_case = [&](const std::string& file_name,
                                   const std::string& label) {
      const std::filesystem::path case_updates =
          dir / (std::string("updates-curl-") + label);
      UpdatePackageSpec case_spec = spec;
      case_spec.file_name = file_name;
      const std::filesystem::path part =
          update_part_path(case_updates, case_spec.file_name);
      const std::filesystem::path final_path =
          case_updates / sanitize_update_file_name(case_spec.file_name);

      std::atomic<bool> finished{false};
      UpdateDownloadOutcome outcome;
      // 假 curl 写完前 8 字节就停住等释放文件：主线程先读到非 0 再放行，
      // 这样「进度在下载过程中可读」不依赖 sleep 与调度，不会假绿也不会假红。
      // 释放文件路径必须在起线程之前设好，否则子进程可能继承不到。
      const std::filesystem::path release =
          dir / ("fake-curl-release-" + label);
      std::filesystem::remove(release);
      ::setenv("DEVCARD_FAKE_RELEASE", release.c_str(), 1);
      std::thread worker([&] {
        outcome = download_update_package(case_updates, case_spec);
        finished.store(true);
      });
      std::uint64_t observed = 0;
      const auto deadline =
          std::chrono::steady_clock::now() + std::chrono::seconds(5);
      while (!finished.load() && std::chrono::steady_clock::now() < deadline) {
        std::error_code size_ec;
        const auto size = std::filesystem::file_size(part, size_ec);
        if (!size_ec && size > observed) {
          observed = static_cast<std::uint64_t>(size);
        }
        if (observed > 0) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
      {
        std::ofstream go(release);
        go << "go";
      }
      worker.join();
      std::filesystem::remove(release);

      CHECK(outcome.ok);
      // 进度必须在下载过程中就读到非 0，且小于总字节（不是下载完才有的终值）。
      CHECK(observed > 0);
      CHECK(observed < size);
      CHECK(std::filesystem::exists(final_path));
      CHECK(verify_update_file(final_path, size, hash).ok);

      // 命令行判据：假 curl 收到的每个参数都必须是 ASCII。
      std::ifstream argv_in(fake_argv, std::ios::binary);
      const std::string argv_text{std::istreambuf_iterator<char>(argv_in),
                                  std::istreambuf_iterator<char>()};
      CHECK(!argv_text.empty());
      bool argv_ascii = true;
      for (const unsigned char byte : argv_text) {
        if (byte >= 0x80) argv_ascii = false;
      }
      CHECK(argv_ascii);
      CHECK(argv_text.find("--output\n" + path_to_utf8(part.filename()) + "\n") !=
            std::string::npos);
      argv_in.close();
      std::filesystem::remove(fake_argv);

      std::cout << "[fake-curl] " << label
                << " file_name=" << file_name
                << " part=" << path_to_utf8(part.filename())
                << " argv_ascii=" << (argv_ascii ? 1 : 0)
                << " progress_first=" << observed << " final_bytes=" << size
                << "\n";
    };

    run_curl_case("DevCard-1.2.3.zip", "ascii");
    run_curl_case("卡片-DevCard-1.2.3.zip", "cjk");

    // 非 ASCII 目标直接失败：不再退回固定临时名（那条路进度永远是 0）。
    std::string reject_error;
    std::string reject_code;
    const bool accepted = curl_fetch(
        spec.url, dir / path_from_utf8("卡片.part"), &reject_error,
        &reject_code);
    CHECK(!accepted);
    CHECK(reject_code == "download_failed");
    CHECK(reject_error.find("非 ASCII") != std::string::npos);
    std::cout << "[fake-curl] non-ascii-target code=" << reject_code
              << " error=" << reject_error << "\n";

    ::unsetenv("DEVCARD_CURL");
    ::unsetenv("DEVCARD_FAKE_SOURCE");
    ::unsetenv("DEVCARD_FAKE_ARGV_LOG");
  }
#endif
}

// —— 更新清单 v2：验签与解析 ——
// 固定向量由 mote 仓库 tools/release/manifest_v2.py generate 生成并签名
// （测试专用随机密钥，与线上私钥无关）。C++ 侧能验过这份签名，等价于
// canonical 序列化（键排序/紧凑/UTF-8/整数与转义规则）与发布端逐字节一致。
constexpr const char* kV2ManifestVector = R"json({
  "channel": "stable",
  "key_id": "2026-11",
  "manifest_version": 2,
  "platforms": {
    "macos": {
      "artifacts": {
        "dmg": {
          "sha256": "56281bd59a882147b162cbf5678d6af5ea582eb2075b2904d2735262f37d8a61",
          "size": 16,
          "url": "https://dl.xiaotu.online/devcard/devcard-9.9.9-macos-arm64.dmg"
        }
      },
      "build": 0,
      "min_os": "14.0",
      "rollout_percent": 100,
      "version": "9.9.9"
    },
    "windows": {
      "artifacts": {
        "portable": {
          "sha256": "ee580ec177f0e10d241fa1e83953c3d6f12edde39f0bece030536d604dfc7ee9",
          "size": 25,
          "url": "https://dl.xiaotu.online/devcard/devcard-9.9.9-win64-portable.zip"
        },
        "setup": {
          "sha256": "000829e58dcb369465e8c74da3487b96d7aa17f754c5411efe84f63c89060da9",
          "size": 22,
          "url": "https://dl.xiaotu.online/devcard/devcard-9.9.9-win64-setup.exe"
        }
      },
      "build": 202611180930,
      "min_os": "10.0",
      "rollout_percent": 100,
      "version": "9.9.9"
    }
  },
  "product": "devcard",
  "signature": "Xy3b0SSkirhPxi7/hw8vYLfKbgWU3Ra+N5rwjONg/XtCEnwIUrmQbFamU2/i1jislpWfOcjpc/kfTNgXLeycDA==",
  "updated_at": "2026-11-18T07:09:20Z"
})json";
constexpr const char* kV2VectorPubkey =
    "c877ab759c694dcb2da43b6053be35c9bf20cabc10ad97cb68357a3c1ea74cb5";

// 灰度原语必须 fail-closed：0% 表示「停止放量」，任何分桶都不放行。
void run_rollout_decision_case() {
  using namespace devcard;
  CHECK(!rollout_allows(0, 0));
  CHECK(!rollout_allows(0, 99));
  CHECK(rollout_allows(100, 99));
  CHECK(rollout_allows(50, 49));
  CHECK(!rollout_allows(50, 50));
  // 分桶只由 install_id 与 build 决定，且落在 0-99。
  const int bucket = rollout_bucket("11111111-1111-1111-1111-111111111111", 100);
  CHECK(bucket >= 0 && bucket < 100);
  CHECK(bucket == rollout_bucket("11111111-1111-1111-1111-111111111111", 100));
}

void run_update_manifest_case() {
  using namespace devcard;
  using nlohmann::json;

  // manifest_v2.py selftest 同源的 RFC 8032 向量密钥：seed 派生出 pk，
  // 测试里用 sk = seed || pk 现场给改过的清单重签。
  const unsigned char kVectorSeed[32] = {
      0x9d, 0x61, 0xb1, 0x9d, 0xef, 0xfd, 0x5a, 0x60, 0xba, 0x84, 0x4a,
      0xf4, 0x92, 0xec, 0x2c, 0xc4, 0x44, 0x49, 0xc5, 0x69, 0x7b, 0x32,
      0x69, 0x19, 0x70, 0x3b, 0xac, 0x03, 0x1c, 0xae, 0x7f, 0x60};
  const unsigned char kVectorPub[32] = {
      0xd7, 0x5a, 0x98, 0x01, 0x82, 0xb1, 0x0a, 0xb7, 0xd5, 0x4b, 0xfe,
      0xd3, 0xc9, 0x64, 0x07, 0x3a, 0x0e, 0xe1, 0x72, 0xf3, 0xda, 0xa6,
      0x23, 0x25, 0xaf, 0x02, 0x1a, 0x68, 0xf7, 0x07, 0x51, 0x1a};
  auto base64_of_64 = [](const unsigned char* raw) {
    static const char* kTable =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(88);
    for (std::size_t i = 0; i < 64; i += 3) {
      const int b0 = raw[i], b1 = i + 1 < 64 ? raw[i + 1] : 0,
                b2 = i + 2 < 64 ? raw[i + 2] : 0;
      out.push_back(kTable[b0 >> 2]);
      out.push_back(kTable[((b0 & 3) << 4) | (b1 >> 4)]);
      out.push_back(i + 1 < 64 ? kTable[((b1 & 15) << 2) | (b2 >> 6)] : '=');
      out.push_back(i + 2 < 64 ? kTable[b2 & 63] : '=');
    }
    return out;
  };
  // 用向量密钥对「去掉 signature 后的 canonical」签名，返回 base64 文本。
  auto sign_with_vector_key = [&](const std::string& canonical) {
    unsigned char sk[64];
    std::memcpy(sk, kVectorSeed, 32);
    std::memcpy(sk + 32, kVectorPub, 32);
    std::vector<unsigned char> sm(64 + canonical.size());
    unsigned long long smlen = 0;
    crypto_sign(sm.data(), &smlen,
                reinterpret_cast<const unsigned char*>(canonical.data()),
                canonical.size(), sk);
    return base64_of_64(sm.data());  // 签名就是 sm 的前 64 字节
  };

  // TweetNaCl 层先用 manifest_v2.py selftest 同源的 RFC 8032 固定向量：
  // 两边（Python 与 C）必须对同一 (msg, sig, pk) 给出同一结论。
  {
    const unsigned char pk[32] = {
        0xd7, 0x5a, 0x98, 0x01, 0x82, 0xb1, 0x0a, 0xb7, 0xd5, 0x4b, 0xfe,
        0xd3, 0xc9, 0x64, 0x07, 0x3a, 0x0e, 0xe1, 0x72, 0xf3, 0xda, 0xa6,
        0x23, 0x25, 0xaf, 0x02, 0x1a, 0x68, 0xf7, 0x07, 0x51, 0x1a};
    const unsigned char sig[64] = {
        0xe6, 0x42, 0x30, 0x9f, 0x0f, 0x79, 0x10, 0x45, 0xd3, 0xd8, 0xe1,
        0x12, 0xe5, 0x07, 0xc3, 0x3f, 0x89, 0xb5, 0xd7, 0xb0, 0xcb, 0x79,
        0xb4, 0xca, 0x58, 0x41, 0x2c, 0xc4, 0xf5, 0x8f, 0x2c, 0x76, 0x2e,
        0x2f, 0x1c, 0xab, 0xf5, 0xa2, 0xfa, 0xc0, 0x5e, 0x63, 0x3b, 0x52,
        0x03, 0x88, 0x0b, 0xa8, 0xa9, 0xde, 0x76, 0x01, 0x47, 0x47, 0x22,
        0xd7, 0xe1, 0x5f, 0x31, 0x46, 0x36, 0x60, 0x86, 0x0d};
    const char* msg = "manifest-v2-selftest";
    unsigned char sm[64 + 20];
    unsigned char m[64 + 20];
    unsigned long long mlen = 0;
    std::memcpy(sm, sig, 64);
    std::memcpy(sm + 64, msg, 20);
    CHECK(crypto_sign_open(m, &mlen, sm, 64 + 20, pk) == 0);
    sm[70] ^= 1;  // 篡改消息
    CHECK(crypto_sign_open(m, &mlen, sm, 64 + 20, pk) != 0);
  }

  std::string error;
  CHECK(verify_manifest_signature(kV2ManifestVector, kV2VectorPubkey, &error));
  CHECK(error.empty());

  // 本平台解析：非 Windows 只认 dmg；Windows 认 setup/portable 并读出构建号。
  const SignedManifest parsed = parse_signed_manifest(kV2ManifestVector, kV2VectorPubkey);
  CHECK(parsed.ok);
#ifdef _WIN32
  CHECK(parsed.platform.version == "9.9.9");
  CHECK(parsed.platform.build == 202611180930ULL);
  CHECK(parsed.platform.artifacts.size() == 2);
  bool seen_setup = false;
  bool seen_portable = false;
  for (const ManifestArtifact& artifact : parsed.platform.artifacts) {
    if (artifact.role == "setup") {
      seen_setup = true;
      CHECK(artifact.name == "devcard-9.9.9-win64-setup.exe");
      CHECK(artifact.size == 22);
      CHECK(artifact.sha256 ==
            "000829e58dcb369465e8c74da3487b96d7aa17f754c5411efe84f63c89060da9");
    }
    if (artifact.role == "portable") seen_portable = true;
  }
  CHECK(seen_setup);
  CHECK(seen_portable);
#else
  CHECK(parsed.platform.version == "9.9.9");
  CHECK(parsed.platform.build == 0);
  CHECK(parsed.platform.artifacts.size() == 1);
  if (parsed.platform.artifacts.size() == 1) {
    CHECK(parsed.platform.artifacts[0].role == "dmg");
    CHECK(parsed.platform.artifacts[0].name == "devcard-9.9.9-macos-arm64.dmg");
    CHECK(parsed.platform.artifacts[0].size == 16);
  }
#endif

  // 篡改任何字段（含 9 位构建号）都必须验签失败。
  std::string tampered(kV2ManifestVector);
  const std::size_t build_at = tampered.find("202611180930");
  CHECK(build_at != std::string::npos);
  tampered.replace(build_at, 10, "202611180001");
  CHECK(!verify_manifest_signature(tampered, kV2VectorPubkey, &error));
  CHECK(error.find("签名验证失败") != std::string::npos);

  // 公钥不匹配也必须失败：换掉内置公钥的第一个字符。
  std::string wrong_pubkey(kV2VectorPubkey);
  wrong_pubkey[0] = wrong_pubkey[0] == 'c' ? 'd' : 'c';
  CHECK(!verify_manifest_signature(kV2ManifestVector, wrong_pubkey, &error));

  // signature 缺失 / 不是合法 base64，都要给出明确错误而不是崩。
  json doc = json::parse(kV2ManifestVector);
  doc.erase("signature");
  const std::string no_signature = doc.dump();
  CHECK(!verify_manifest_signature(no_signature, kV2VectorPubkey, &error));
  CHECK(error.find("signature") != std::string::npos);
  doc["signature"] = "!!!not-base64!!!";
  CHECK(!verify_manifest_signature(doc.dump(), kV2VectorPubkey, &error));
  CHECK(error.find("base64") != std::string::npos);

  // 非 JSON / 非 v2 / 缺本平台：解析层直接拒绝。
  CHECK(!parse_signed_manifest("not json at all", kV2VectorPubkey).ok);
  const SignedManifest wrong_version =
      parse_signed_manifest(R"({"manifest_version":1,"signature":"AAAA"})", kV2VectorPubkey);
  CHECK(!wrong_version.ok);
  // 缺本平台：把向量清单的 platforms 清空并用测试密钥重签（签名有效才
  // 能走到解析层，否则错误会停在验签上）。验签也用同一把向量公钥。
  json emptied = json::parse(kV2ManifestVector);
  emptied["platforms"] = json::object();
  emptied.erase("signature");
  const std::string emptied_canonical = emptied.dump();
  emptied["signature"] = sign_with_vector_key(emptied_canonical);
  const SignedManifest no_platform = parse_signed_manifest(
      emptied.dump(),
      "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a");
  CHECK(!no_platform.ok);
  CHECK(no_platform.error.find("本平台") != std::string::npos);

  std::cout << "[update-manifest] signature + parse checks done\n";

  // 构建号读取断言：如果编译期注入了 DEVCARD_BUILD，必须等于该值；
  // 未注入时必须返回 0（回退路径）。这一条保证两端都走得通。
#if defined(DEVCARD_BUILD)
  CHECK(devcard::compiled_build_number() ==
        static_cast<std::uint64_t>(DEVCARD_BUILD));
#else
  CHECK(devcard::compiled_build_number() == 0);
#endif
}

// 场景函数：Db 是局部对象，函数返回即析构，调用方随后删目录才不会撞上打开的库。
void run_user_service_case(const std::filesystem::path& dir) {
  const std::filesystem::path file = dir / "devicedb.db";
  devcard::Db db(file);
  devcard::migrate(db);

  const devcard::SeedResult seed = devcard::seed_default_users(db, dir);
  CHECK(seed.seeded);
  CHECK(seed.accounts.size() == 2);
  CHECK(std::filesystem::exists(seed.password_file));
  CHECK(!devcard::seed_default_users(db, dir).seeded);  // 幂等：已有账号不再重建

  devcard::UserService users(db);
  CHECK(users.count() == 2);

  const std::string root_password = seed.accounts[0].second;
  const devcard::User root = users.authenticate("root", root_password);
  CHECK(root.must_change_password);
  CHECK(users.local_user() == "root");

  bool bad_login = false;
  try {
    users.authenticate("root", "definitely-wrong");
  } catch (const devcard::AppError& error) {
    bad_login = std::string(error.code()) == "invalid_credentials";
  }
  CHECK(bad_login);

  for (int i = 0; i < 4; ++i) {
    try {
      users.authenticate("root", "definitely-wrong");
    } catch (const devcard::AppError&) {
    }
  }
  bool locked = false;
  try {
    users.authenticate("root", root_password);
  } catch (const devcard::AppError& error) {
    locked = std::string(error.code()) == "locked";
  }
  CHECK(locked);

  // 命令行恢复通道：重置后清锁、强制改密。
  users.reset_password("root", "ResetPassw0rd");
  const devcard::User reset = users.authenticate("root", "ResetPassw0rd");
  CHECK(reset.must_change_password);

  users.change_password("root", "ResetPassw0rd", "ChangedPassw0rd");
  CHECK(!users.get("root").must_change_password);
  bool old_rejected = false;
  try {
    users.authenticate("root", "ResetPassw0rd");
  } catch (const devcard::AppError&) {
    old_rejected = true;
  }
  CHECK(old_rejected);
  CHECK(users.authenticate("root", "ChangedPassw0rd").user_name == "root");

  // 首次登录 / 重置后的强制改密不要求旧密码；但只有服务端标记
  // must_change_password 时才放行，普通状态仍必须走 change_password。
  users.reset_password("root", "ResetPassw0rd2");
  users.complete_forced_change("root", "ForcedPassw0rd");
  CHECK(!users.get("root").must_change_password);
  CHECK(users.authenticate("root", "ForcedPassw0rd").user_name == "root");
  bool forced_guard = false;
  try {
    users.complete_forced_change("root", "SneakyPassw0rd");
  } catch (const devcard::AppError& error) {
    forced_guard = std::string(error.code()) == "old_password_required";
  }
  CHECK(forced_guard);

  bool weak = false;
  try {
    users.create("short", "123", "", "user");
  } catch (const devcard::AppError& error) {
    weak = std::string(error.code()) == "weak_password";
  }
  CHECK(weak);

  const devcard::User normal =
      users.create("tester", "TesterPassw0rd", "测试员", "user");
  CHECK(normal.role == "user");
  CHECK(normal.must_change_password);

  bool last_admin = false;
  try {
    users.remove("root", "tester");
  } catch (const devcard::AppError& error) {
    last_admin = std::string(error.code()) == "last_admin";
  }
  CHECK(last_admin);

  users.remove("tester", "root");
  CHECK(users.count() == 2);

  // 两个默认账号都改密后，一次性密码文件可删除。
  users.change_password("安业物业", seed.accounts[1].second, "AnyePassw0rd");
  CHECK(!users.any_must_change());
  devcard::remove_seed_password_file(dir);
  CHECK(!std::filesystem::exists(seed.password_file));

}

// 中文路径判据：只看磁盘上实际的目录项码元，不看 path / 界面怎么显示。
// Windows 上 path 的窄字符构造按 ACP（简中即 GBK）解释字节，源码里的 UTF-8
// 会落成 13 个乱码码元（9352 6FC6 E750 7490 FE40 5F7F 9359 FF44 62A4 002E ...），
// 而正确值是 10 个码元。这条用例在 Windows 上必须红过一次，否则等于没测。
void run_path_encoding_case(const std::filesystem::path& dir) {
  const std::string expected = "初始账号密码.txt";
  CHECK(utf8_codepoints(expected) == 10);
  // 夹具目录必须真的带非 ASCII 段，否则下面几条在 ASCII 路径下等于没测。
  CHECK(devcard::path_to_utf8(dir).find("样张-📇") != std::string::npos);
  CHECK(std::filesystem::exists(dir));

  // 1) 内存里来回一趟不能变形（Windows 上这里就会暴露 ACP 解释）。
  CHECK(devcard::path_to_utf8(devcard::path_from_utf8(expected)) == expected);
  // ACP（GBK）表达不了 emoji：这条能区分「按 UTF-8 转」和「按 ACP 转」。
  const std::string emoji = "样张-📇.pdf";
  CHECK(devcard::path_to_utf8(devcard::path_from_utf8(emoji)) == emoji);

  {
    devcard::Db db(dir / "devicedb.db");
    devcard::migrate(db);
    const devcard::SeedResult seed = devcard::seed_default_users(db, dir);
    CHECK(seed.seeded);

    // 2) 种子文件名的码元必须与常量逐码点一致。
    CHECK(devcard::path_to_utf8(seed.password_file.filename()) == expected);

    // 3) 再对着枚举出来的目录项核一遍：这是「落盘」而不是「内存里看着对」。
    bool found = false;
    bool mojibake = false;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
      const std::string name = devcard::path_to_utf8(entry.path().filename());
      if (name == expected) found = true;
      if (name.find("初始账号密码.txt") == std::string::npos &&
          name.find(".txt") != std::string::npos) {
        mojibake = true;
      }
    }
    CHECK(found);
    CHECK(!mojibake);
  }

  // 5) 宽→窄→宽往返的退化检查（`path.string() + "-wal"` vs 在 path 上 `+=`）。
  //    POSIX：窄编码就是 UTF-8，两种写法必然相等；
  //    Windows/ACP：`.string()` 走当前码页，ACP 表达不了的段不是「改坏」，而是
  //    直接抛 std::system_error（实测 what()：No mapping for the Unicode character
  //    exists in the target multi-byte code page）——本夹具带 emoji，必走 catch。
  //    不接住会把整个二进制带崩（0xC0000409），红的原因反而看不出是编码问题。
  const std::filesystem::path db_file = dir / "devicedb.db";
  std::filesystem::path direct_wal = db_file;
  direct_wal += "-wal";
  // 旧写法在非 ASCII 路径上有两种退化：POSIX 上返回错误字节，Windows 上
  // .string() 直接抛「目标代码页无法表示的字符」。两种都算旧写法不可用，
  // 不能让它把整个用例打崩，更不能把「抛错」当成「通过」。
  bool legacy_threw = false;
  std::filesystem::path legacy_wal;
  try {
    legacy_wal = std::filesystem::path(db_file.string() + "-wal");
  } catch (const std::exception&) {
    // MSVC 的窄转换失败抛的是 system_error 一系，不是 filesystem_error；
    // 按 std::exception 兜住，免得整条用例 fail-fast 成 0xC0000409。
    legacy_threw = true;
  }
  if (legacy_threw) {
    // 这条分支不断言（旧写法在该平台直接抛），但必须计数：否则「断言数」与实际
    // 执行数对不上，看输出也分不清是跳过还是漏跑。
    ++skips;
    std::cout << "SKIP 宽→窄→宽：Windows 上 path::string() 对非 ASCII 路径直接抛错，"
                 "旧写法不可用" << std::endl;
  } else {
    CHECK(legacy_wal == direct_wal);
  }
  CHECK(direct_wal.parent_path() == db_file.parent_path());
  CHECK(devcard::path_to_utf8(direct_wal.parent_path()) ==
        devcard::path_to_utf8(db_file.parent_path()));

  // 6) `+=` 的结果要落在真实目录里：写出来再按目录项核对，而不是只看内存。
  {
    std::ofstream probe(direct_wal, std::ios::binary);
    probe << "x";
    probe.close();
    bool wal_found = false;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
      if (devcard::path_to_utf8(entry.path().filename()) == "devicedb.db-wal") {
        wal_found = true;
      }
    }
    CHECK(wal_found);
    std::filesystem::remove(direct_wal);
  }

  // 4) 删除也要按同一套码元命中，不能删出个「看起来删了」。
  devcard::remove_seed_password_file(dir);
  CHECK(!std::filesystem::exists(dir / devcard::path_from_utf8(expected)));
}

// 调用点断言：如果错在 curl_fetch 里的 target.string()，helper 全绿也照样下错地方。
// 所以这里走真实的 download_update_package（默认 fetcher 就是 curl_fetch），
// 并把 DEVCARD_CURL 指向本测试程序自己——它按真实 curl 的方式重新解析 argv。
// 判据是枚举 <data_dir>\updates\ 的目录项逐码点比，不是断言某个字符串相等。
void run_update_download_landing_case(const std::filesystem::path& dir) {
  using namespace devcard;
  const std::filesystem::path data_dir = dir / "中文数据";
  const std::filesystem::path updates = data_dir / "updates";
  std::filesystem::create_directories(updates);

  const std::filesystem::path payload = data_dir / "payload.bin";
  {
    std::ofstream out(payload, std::ios::binary | std::ios::trunc);
    for (int i = 0; i < 4096; ++i) {
      out.put(static_cast<char>((i * 31 + 7) & 0xFF));
    }
  }

  UpdatePackageSpec spec;
  spec.version = "1.2.3";
  spec.file_name = "DevCard-1.2.3.exe";
  spec.url =
      "https://github.com/LinYuwei82/dev_card_cpp/releases/download/v1.2.3/"
      "DevCard-1.2.3.exe";
  spec.size = std::filesystem::file_size(payload);
  spec.sha256 = sha256_file(payload);

  // 环境变量本身也要按 UTF-8 给：curl_fetch 取出来后会再 widen(CP_UTF8)。
  set_env("DEVCARD_CURL", path_to_utf8(self_executable()));
  set_env("DEVCARD_PROBE_PAYLOAD", path_to_utf8(payload));
  set_env("DEVCARD_PROBE_EXPECT_CWD", path_to_utf8(updates));

  const UpdateDownloadOutcome outcome = download_update_package(updates, spec);
  set_env("DEVCARD_CURL", "");
  set_env("DEVCARD_PROBE_PAYLOAD", "");
  set_env("DEVCARD_PROBE_EXPECT_CWD", "");

  CHECK(outcome.ok);
  if (!outcome.ok) std::cerr << "  下载失败：" << outcome.error << "\n";
  CHECK(outcome.bytes == spec.size);

  int entries = 0;
  bool name_ok = false;
  bool leftovers = false;
  for (const auto& entry : std::filesystem::directory_iterator(updates)) {
    const std::string name = path_to_utf8(entry.path().filename());
    ++entries;
    if (name == "DevCard-1.2.3.exe") name_ok = true;
    if (name.size() >= 5 &&
        (name.compare(name.size() - 5, 5, ".part") == 0 ||
         name.compare(name.size() - 5, 5, ".log") == 0)) {
      leftovers = true;
    }
  }
  CHECK(entries == 1);
  CHECK(name_ok);
  CHECK(!leftovers);

  const std::filesystem::path landed =
      updates / path_from_utf8("DevCard-1.2.3.exe");
  CHECK(std::filesystem::exists(landed));
  std::error_code size_ec;
  const auto landed_size = std::filesystem::file_size(landed, size_ec);
  CHECK(!size_ec);
  CHECK(landed_size == spec.size);
  CHECK(outcome.path == path_to_utf8(landed));
}

// 场景函数：Db 是局部对象，函数返回即析构，调用方随后删目录才不会撞上打开的库。
void run_backup_case(const std::filesystem::path& dir) {
  devcard::Db db(dir / "devicedb.db");
  devcard::migrate(db);

  devcard::DeviceService service(db);
  devcard::Device seed;
  seed.dev_name = "备份样例";
  seed.location = "B1栋";
  seed.control_range = "1层";
  seed.phone = "13800000001";
  const devcard::Device created = service.create(seed);

  devcard::DeviceDao devices;
  CHECK(devices.count(db, devcard::DeviceQuery{}) == 1);

  // 先改一次，让备份里的 version/updated_by 不是列默认值。
  devcard::Device edited = created;
  edited.phone = "13800000002";
  db.write([&](sqlite3* conn) { CHECK(devices.update(conn, edited, "测试员")); });
  devcard::Device snapshot;
  CHECK(devices.find(db, created.id, snapshot));
  CHECK(snapshot.version == created.version + 1);
  CHECK(snapshot.updated_by == "测试员");

  // 备份与恢复：删除一条后可从备份恢复。
  const devcard::BackupEntry backup =
      devcard::create_backup(db, dir, "test");
  CHECK(std::filesystem::exists(devcard::path_from_utf8(backup.path)));
  CHECK(backup.bytes > 0);
  service.remove(created.id, snapshot.version);
  CHECK(devices.count(db, devcard::DeviceQuery{}) == 0);
  devcard::restore_backup(db, dir, backup.name);
  CHECK(devices.count(db, devcard::DeviceQuery{}) == 1);

  // 恢复必须带回 version/updated_by，否则乐观锁与「被谁修改」提示都会退化。
  devcard::Device restored;
  CHECK(devices.find(db, created.id, restored));
  CHECK(restored.version == snapshot.version);
  CHECK(restored.updated_by == "测试员");

  bool bad_name = false;
  try {
    devcard::restore_backup(db, dir, "../devicedb.db");
  } catch (const devcard::AppError& error) {
    bad_name = std::string(error.code()) == "bad_backup";
  }
  CHECK(bad_name);

}

// 场景函数：Db 是局部对象，函数返回即析构，调用方随后删目录才不会撞上打开的库。
void run_company_name_case(const std::filesystem::path& dir) {
  devcard::Db db(dir / "devicedb.db");
  devcard::migrate(db);

  CHECK(devcard::company_name(db) ==
        std::string(devcard::kDefaultCompanyName));
  CHECK(devcard::company_print_name(db) ==
        std::string(devcard::kDefaultCompanyName));
  CHECK(devcard::company_name_width("安业AB") == 3.0);
  CHECK(devcard::company_name_width("") == 0.0);

  db.write([](sqlite3* conn) {
    devcard::set_setting(conn, "print_company_name",
                         "深圳市安业物业管理有限公司南山分公司");
  });
  CHECK(devcard::company_name_width(devcard::company_name(db)) == 18.0);
  CHECK(devcard::company_print_name(db) == "深圳市安业物业管理有限公司");

  // 录入上限 16 个字符：存得下，卡面仍按 13 字宽截断。
  CHECK(devcard::kCompanyNameInputMaxLength == 16);
  db.write([](sqlite3* conn) {
    devcard::set_setting(conn, "print_company_name",
                         "深圳市安业物业管理有限公司南山分");
  });
  CHECK(devcard::company_name_width(devcard::company_name(db)) == 16.0);
  CHECK(devcard::company_print_name(db) == "深圳市安业物业管理有限公司");

  db.write([](sqlite3* conn) {
    devcard::set_setting(conn, "print_company_name",
                         "ABCDEFGHIJKLMNOPQRSTUVWXYZ");
  });
  CHECK(devcard::company_name_width(devcard::company_name(db)) == 13.0);
  CHECK(devcard::company_print_name(db) == "ABCDEFGHIJKLMNOPQRSTUVWXYZ");

  // 边界：12 个汉字（12.0）+ 一个 ASCII（12.5）之后再来汉字就超宽了。
  // 超宽字必须直接停，不能「跳过它继续收后面的窄字」，否则打印名不是原串的真前缀，
  // 前端预览会和实际打印不一致（web/app.js truncateCompanyName 同口径）。
  db.write([](sqlite3* conn) {
    devcard::set_setting(conn, "print_company_name",
                         "安安全全生产管理责任到位A汉A");
  });
  CHECK(devcard::company_name_width(devcard::company_name(db)) == 14.0);
  CHECK(devcard::company_print_name(db) == "安安全全生产管理责任到位A");

}

// 场景函数：Db 是局部对象，函数返回即析构，调用方随后删目录才不会撞上打开的库。
void run_xlsx_template_case(const std::filesystem::path& dir) {
  const std::filesystem::path file = dir / "devicedb.db";
  devcard::Db db(file);
  devcard::migrate(db);
  devcard::DeviceService service(db);

  devcard::xlsx::Sheet data;
  data.name = "设备台账";
  data.rows.push_back(devcard::DeviceService::expected_header());
  devcard::xlsx::Sheet help;
  help.name = "填写说明";
  help.rows = {{"设备名称", "必填"}, {"示例（假号码）", "13800000001"}};

  const std::string bytes = devcard::xlsx::write_workbook({data, help});
  CHECK(!bytes.empty());
  const auto rows = devcard::xlsx::read(bytes);
  CHECK(rows.size() == 1);
  CHECK(rows[0] == devcard::DeviceService::expected_header());

  // 模板原样上传 = 0 行导入，不会带入假数据。
  const devcard::ImportReport report = service.import_rows(rows);
  CHECK(report.imported == 0);
  CHECK(report.skipped_empty == 0);
  CHECK(report.duplicates == 0);
  CHECK(service.export_rows().size() == 1);  // 只剩表头

}

// M2 硬验收②：同一份文件连续导入两次——不去重、不合并、不跳过、不覆盖。
// 判据按文档原话：第二次仍新增 N 条、重复只计数提示、两次后总数 2N，
// 且重复行各自持有新的 id（COUNT(DISTINCT 三字段) 不变）。
void run_xlsx_reimport_case(const std::filesystem::path& dir) {
  const std::filesystem::path file = dir / "devicedb.db";
  devcard::Db db(file);
  devcard::migrate(db);
  devcard::DeviceService service(db);

  devcard::xlsx::Sheet data;
  data.name = "设备台账";
  data.rows.push_back(devcard::DeviceService::expected_header());
  data.rows.push_back({"1号水泵", "B1栋", "1-3层", "13800000001"});
  data.rows.push_back({"排风机", "B1栋", "屋顶", "13800000002"});
  data.rows.push_back({"", "", "", ""});  // 四字段全空：跳过
  data.rows.push_back({"消防泵", "A栋", "B1层", "13800000003"});
  const std::string bytes = devcard::xlsx::write_workbook({data});
  CHECK(!bytes.empty());

  // 两次都从同一份字节重新解析，模拟「同一个文件上传两次」。
  const auto first_rows = devcard::xlsx::read(bytes);
  CHECK(first_rows.size() == 5);
  const devcard::ImportReport first = service.import_rows(first_rows);
  CHECK(first.imported == 3);
  CHECK(first.skipped_empty == 1);
  CHECK(first.duplicates == 0);
  CHECK(service.export_rows().size() == 4);  // 表头 + 3

  const auto second_rows = devcard::xlsx::read(bytes);
  const devcard::ImportReport second = service.import_rows(second_rows);
  CHECK(second.imported == 3);     // 不阻断、不跳过
  CHECK(second.skipped_empty == 1);
  CHECK(second.duplicates == 3);   // 仅提示
  CHECK(service.export_rows().size() == 7);  // 表头 + 2N

  // 主键必须各自独立：6 条不同 id，且三字段组合仍是 3 种。
  devcard::DeviceDao dao;
  devcard::DeviceQuery query;
  const auto all = dao.list(db, query);
  CHECK(all.size() == 6);
  std::set<std::int64_t> ids;
  std::set<std::string> triples;
  for (const auto& item : all) {
    ids.insert(item.id);
    triples.insert(item.dev_name + "|" + item.location + "|" + item.control_range);
  }
  CHECK(ids.size() == 6);
  CHECK(triples.size() == 3);

  // 先插入的 3 条没有被覆盖：第一次导入的内容原样还在。
  bool found_pump = false;
  for (const auto& item : all) {
    if (item.dev_name == "1号水泵" && item.phone == "13800000001" &&
        item.control_range == "1-3层") {
      found_pump = true;
    }
  }
  CHECK(found_pump);
}

// 必填项缺失只跳过该行，不整批回滚，并带回行号。
void run_xlsx_invalid_row_case(const std::filesystem::path& dir) {
  const std::filesystem::path file = dir / "devicedb.db";
  devcard::Db db(file);
  devcard::migrate(db);
  devcard::DeviceService service(db);

  devcard::xlsx::Sheet data;
  data.name = "设备台账";
  data.rows.push_back(devcard::DeviceService::expected_header());
  data.rows.push_back({"1号水泵", "B1栋", "1-3层", "13800000001"});
  data.rows.push_back({"只填了电话", "", "", "13800000002"});  // 第 3 行缺必填
  data.rows.push_back({"排风机", "B1栋", "屋顶", "13800000003"});
  const std::string bytes = devcard::xlsx::write_workbook({data});
  CHECK(!bytes.empty());

  const devcard::ImportReport report =
      service.import_rows(devcard::xlsx::read(bytes));
  CHECK(report.imported == 2);
  CHECK(report.skipped_invalid == 1);
  CHECK(report.invalid_rows.size() == 1);
  if (!report.invalid_rows.empty()) {
    CHECK(report.invalid_rows[0] == 3);
  }
  devcard::DeviceDao dao;
  CHECK(dao.count(db, devcard::DeviceQuery{}) == 2);
}

// 合并重复：三字段完全相同的记录每组只留最早一条；保留行电话为空时从重复行补齐，
// 已有电话不覆盖；电话没变的行版本号不动，旧页面仍能正常编辑。
void run_dedupe_case(const std::filesystem::path& dir) {
  const std::filesystem::path file = dir / "devicedb.db";
  devcard::Db db(file);
  devcard::migrate(db);
  devcard::DeviceService service(db);

  devcard::xlsx::Sheet data;
  data.name = "设备台账";
  data.rows.push_back(devcard::DeviceService::expected_header());
  data.rows.push_back({"1号水泵", "B1栋", "1-3层", ""});             // 保留行，电话为空
  data.rows.push_back({"1号水泵", "B1栋", "1-3层", "13800000001"});  // 重复：补电话
  data.rows.push_back({"排风机", "B1栋", "屋顶", "13800000002"});    // 保留行，已有电话
  data.rows.push_back({"排风机", "B1栋", "屋顶", "13900000000"});    // 重复：不覆盖
  data.rows.push_back({"消防泵", "A栋", "B1层", "13800000003"});     // 唯一：不动
  const auto rows = devcard::xlsx::read(devcard::xlsx::write_workbook({data}));
  CHECK(service.import_rows(rows).imported == 5);

  const devcard::DedupeReport merged = service.dedupe(devcard::AuditContext{});
  CHECK(merged.removed == 2);
  CHECK(merged.remaining == 3);

  devcard::DeviceDao dao;
  const auto all = dao.list(db, devcard::DeviceQuery{});
  CHECK(all.size() == 3);
  for (const auto& item : all) {
    if (item.dev_name == "1号水泵") {
      CHECK(item.phone == "13800000001");  // 空电话从重复行补齐
      CHECK(item.version == 2);            // 内容变了，版本必须前移
    } else if (item.dev_name == "排风机") {
      CHECK(item.phone == "13800000002");  // 不覆盖已有值
      CHECK(item.version == 1);            // 内容没变，版本不动
    } else {
      CHECK(item.version == 1);
    }
  }

  // 幂等：没有重复时一条都不删。
  const devcard::DedupeReport again = service.dedupe(devcard::AuditContext{});
  CHECK(again.removed == 0);
  CHECK(again.remaining == 3);
}

int run_all_tests() {
  const std::filesystem::path file = temp_db();
  {
    devcard::Db db(file);
    CHECK(db.schema_version() == 0);
    devcard::migrate(db);
    CHECK(db.schema_version() == devcard::target_schema_version());
    devcard::migrate(db);  // 幂等
    CHECK(db.schema_version() == devcard::target_schema_version());

    devcard::DeviceDao dao;
    devcard::Device pump;
    pump.dev_name = "1号水泵";
    pump.location = "B1栋";
    pump.control_range = "1-3层";
    pump.phone = "13800000001";
    db.write([&](sqlite3* conn) { dao.insert(conn, pump); });
    CHECK(pump.id == 1);

    devcard::Device fan;
    fan.dev_name = "排风机";
    fan.location = "B1栋";
    fan.control_range = "屋顶";
    fan.phone = "13800000002";
    db.write([&](sqlite3* conn) { dao.insert(conn, fan); });

    devcard::DeviceQuery query;
    CHECK(dao.count(db, query) == 2);
    auto all = dao.list(db, query);
    CHECK(all.size() == 2);
    CHECK(all.front().dev_name == "1号水泵");

    query.keyword = "水泵";
    CHECK(dao.count(db, query) == 1);
    query.keyword.clear();
    query.name = "排风机";
    CHECK(dao.count(db, query) == 1);

    devcard::Device found;
    CHECK(dao.find(db, fan.id, found));
    CHECK(found.location == "B1栋");

    found.location = "A2栋";
    db.write([&](sqlite3* conn) { dao.update(conn, found); });
    devcard::Device updated;
    CHECK(dao.find(db, fan.id, updated));
    CHECK(updated.location == "A2栋");

    db.write([&](sqlite3* conn) { dao.erase(conn, pump.id, pump.version); });
    CHECK(dao.count(db, devcard::DeviceQuery{}) == 1);

    query.name.clear();
    query.page_size = 1000;
    query.sort = "dev_name; DROP TABLE device";
    CHECK(dao.list(db, query).size() == 1);  // 非法排序被忽略

    bool rejected = false;
    try {
      devcard::Device bad;
      bad.dev_name = "";
      db.write([&](sqlite3* conn) { dao.insert(conn, bad); });
    } catch (const devcard::AppError&) {
      rejected = true;
    }
    CHECK(rejected);
  }

  {
    devcard::Db db(file);
    CHECK(db.schema_version() == devcard::target_schema_version());
  }

  {
    const std::filesystem::path service_file = temp_db();
    {
      devcard::Db db(service_file);
      devcard::migrate(db);
      devcard::DeviceService service(db);

      auto make = [](std::string name, std::string location,
                     std::string range, std::string phone) {
        devcard::Device device;
        device.dev_name = std::move(name);
        device.location = std::move(location);
        device.control_range = std::move(range);
        device.phone = std::move(phone);
        return device;
      };

      const devcard::Device first =
          service.create(make(" 1号水泵 ", "B1栋", "1-3层", "13800000001"));
      CHECK(first.dev_name == "1号水泵");
      CHECK(first.id > 0);
      service.create(make("2号水泵", "B1栋", "1-3层", "13800000002"));
      service.create(make("排风机", "B1栋", "屋顶", "13800000003"));
      service.create(make("空调机组", "A2栋", "2层", "13800000004"));
      service.create(make("照明配电箱", "A2栋", "1层", "13800000005"));

      bool duplicate = false;
      try {
        service.create(make("1号水泵", "B1栋", "1-3层", "13800000009"));
      } catch (const devcard::AppError& error) {
        duplicate = std::string(error.code()) == "duplicate";
      }
      CHECK(duplicate);

      bool too_long = false;
      try {
        service.create(make(std::string(21, 'a'), "B1栋", "1层", "138"));
      } catch (const devcard::AppError& error) {
        too_long = std::string(error.code()) == "invalid_device";
      }
      CHECK(too_long);

      bool empty_field = false;
      try {
        service.create(make("设备", "   ", "1层", "138"));
      } catch (const devcard::AppError& error) {
        empty_field = std::string(error.code()) == "invalid_device";
      }
      CHECK(empty_field);

      const devcard::DeviceOptions options = service.options();
      CHECK(options.locations.size() == 2);
      CHECK(options.locations.front() == "A2栋");
      CHECK(options.locations.back() == "B1栋");
      CHECK(options.names.size() == 5);

      devcard::DeviceQuery query;
      query.page_size = 2;
      const devcard::DevicePage page0 = service.list(query);
      CHECK(page0.total == 5);
      CHECK(page0.items.size() == 2);
      query.page = 2;
      const devcard::DevicePage page2 = service.list(query);
      CHECK(page2.items.size() == 1);
      CHECK(page2.page == 2);

      // 极端页码不能因 int 乘法溢出回绕成第一页。（不能叫 far：MSVC 里是关键字）
      devcard::DeviceQuery last_page;
      last_page.page = 2147483647;
      last_page.page_size = 1000;
      CHECK(service.list(last_page).items.empty());

      devcard::DeviceQuery by_location;
      by_location.location = "A2栋";
      CHECK(service.list(by_location).total == 2);

      devcard::DeviceQuery by_keyword;
      by_keyword.keyword = "水泵";
      by_keyword.field = "dev_name";
      CHECK(service.list(by_keyword).total == 2);

      devcard::Device edited = first;
      edited.phone = "13900000000";
      const devcard::Device saved = service.update(first.id, edited);
      CHECK(saved.phone == "13900000000");
      CHECK(service.get(first.id).phone == "13900000000");

      bool missing_update = false;
      try {
        devcard::Device absent =
            make("不存在", "B1栋", "1层", "138");
        absent.version = 1;  // 有版本才谈得上「记录不存在」，否则先被 bad_version 拦住
        service.update(99999, absent);
      } catch (const devcard::AppError& error) {
        missing_update = std::string(error.code()) == "not_found";
      }
      CHECK(missing_update);

      // 真并发：两个线程拿同一个版本同时改同一条，只能一个成功。
      {
        const devcard::Device base = service.get(first.id);
        struct Outcome {
          bool ok = false;
          bool conflict = false;
          devcard::Device current;
          std::vector<std::string> fields;
          std::string phone;
        };
        Outcome left;
        Outcome right;
        auto attempt = [&](Outcome& out, const char* phone) {
          devcard::Device edit = base;
          edit.phone = phone;
          out.phone = phone;
          try {
            service.update(base.id, edit);
            out.ok = true;
          } catch (const devcard::ConflictError& error) {
            out.conflict = true;
            out.current = error.snapshot().current;
            out.fields = error.snapshot().fields;
          }
        };
        std::thread one(attempt, std::ref(left), "13911112222");
        std::thread two(attempt, std::ref(right), "13933334444");
        one.join();
        two.join();

        CHECK(left.ok != right.ok);
        CHECK(left.conflict != right.conflict);
        const Outcome& loser = left.conflict ? left : right;
        const Outcome& winner = left.ok ? left : right;
        CHECK(loser.conflict);
        CHECK(loser.current.version == base.version + 1);
        CHECK(loser.current.phone == winner.phone);
        CHECK(loser.fields.size() == 1);
        CHECK(!loser.fields.empty() && loser.fields.front() == "phone");

        const devcard::Device merged = service.get(base.id);
        CHECK(merged.version == base.version + 1);
        CHECK(merged.phone == winner.phone);
      }

      const std::int64_t remove_version = service.get(first.id).version;
      service.remove(first.id, remove_version);
      CHECK(service.list(devcard::DeviceQuery{}).total == 4);
      bool missing_remove = false;
      try {
        service.remove(first.id, remove_version);
      } catch (const devcard::AppError& error) {
        missing_remove = std::string(error.code()) == "not_found";
      }
      CHECK(missing_remove);
    }
    std::filesystem::remove_all(service_file.parent_path());
  }

  {
    const std::filesystem::path import_file = temp_db();
    {
      devcard::Db db(import_file);
      devcard::migrate(db);
      devcard::DeviceService service(db);

      const std::vector<std::vector<std::string>> rows = {
          {"设备名称", "位置", "控制范围", "维护电话"},
          {"1号泵", "B1栋", "1-3层", "13800000001"},
          {"2号泵", "B1栋", "1-3层", "13800000002"},
          {"风机", "A2栋", "屋顶", "13800000003"},
          {"", "", "", ""},
          {"一二三四五六七八九十十一", "A2栋", "1层", "13800000004"},
      };

      const devcard::ImportReport first = service.import_rows(rows);
      CHECK(first.imported == 4);
      CHECK(first.skipped_empty == 1);
      CHECK(first.truncated == 1);
      CHECK(first.duplicates == 0);

      const devcard::ImportReport second = service.import_rows(rows);
      CHECK(second.imported == 4);
      CHECK(second.duplicates == 4);
      CHECK(second.skipped_empty == 1);

      devcard::DeviceQuery query;
      query.page_size = 1000;
      CHECK(service.list(query).total == 8);

      // 表头不对整批拒绝，且不允许错位对齐。
      bool bad_header = false;
      try {
        service.import_rows({{"位置", "设备名称", "控制范围", "维护电话"},
                             {"B1栋", "3号泵", "1层", "138"}});
      } catch (const devcard::AppError& error) {
        bad_header = std::string(error.code()) == "bad_header";
      }
      CHECK(bad_header);
      CHECK(service.list(query).total == 8);

      const auto exported = service.export_rows();
      CHECK(exported.size() == 9);
      CHECK(exported.front() == devcard::DeviceService::expected_header());
      const std::string bytes = devcard::xlsx::write(exported);
      CHECK(!bytes.empty());
      const auto roundtrip = devcard::xlsx::read(bytes);
      CHECK(roundtrip.size() == exported.size());
      CHECK(roundtrip.front() == exported.front());
      CHECK(roundtrip[1] == exported[1]);
      CHECK(roundtrip.back() == exported.back());
    }
    std::filesystem::remove_all(import_file.parent_path());
  }

  {
    const std::filesystem::path legacy_file = temp_db();
    {
      devcard::Db db(legacy_file);
      db.write_raw([](sqlite3* conn) {
        devcard::exec_sql(
            conn,
            "CREATE TABLE tb_device(dev_id INTEGER PRIMARY KEY, dev_name TEXT)");
      });
      bool rejected = false;
      try {
        devcard::migrate(db);
      } catch (const devcard::AppError& error) {
        rejected = std::string(error.code()) == "legacy_db";
      }
      CHECK(rejected);
    }
    std::filesystem::remove_all(legacy_file.parent_path());
  }

  {
    // Argon2id：编码串、正确/错误密码。
    const std::string hash = devcard::hash_password("Passw0rd!");
    CHECK(hash.rfind("$argon2id$", 0) == 0);
    CHECK(devcard::verify_password(hash, "Passw0rd!"));
    CHECK(!devcard::verify_password(hash, "passw0rd!"));
    CHECK(!devcard::verify_password("", "Passw0rd!"));
  }

  {
    const std::filesystem::path dir = temp_db().parent_path();
    run_user_service_case(dir);
    check_db_closed(dir);
    std::filesystem::remove_all(dir);
    CHECK(!std::filesystem::exists(dir));
  }

  {
    // 中文路径：种子文件名按「磁盘上的目录项码元」比，不看界面显示。
    const std::filesystem::path dir = temp_db().parent_path();
    run_path_encoding_case(dir);
    check_db_closed(dir);
    std::filesystem::remove_all(dir);
    CHECK(!std::filesystem::exists(dir));
  }

  {
    // 备份与恢复：删除一条后可从备份恢复。
    const std::filesystem::path dir = temp_db().parent_path();
    run_backup_case(dir);
    check_db_closed(dir);
    std::filesystem::remove_all(dir);
    CHECK(!std::filesystem::exists(dir));
  }

  {
    // 公司名：默认值回落；自定义按 13 个显示宽度截断（汉字 1，ASCII 0.5）。
    const std::filesystem::path dir = temp_db().parent_path();
    run_company_name_case(dir);
    check_db_closed(dir);
    std::filesystem::remove_all(dir);
    CHECK(!std::filesystem::exists(dir));
  }

  {
    // 下载落盘：数据目录含中文，判据是枚举出来的目录项码元。
    const std::filesystem::path dir = temp_db().parent_path();
    run_update_download_landing_case(dir);
    std::filesystem::remove_all(dir);
    CHECK(!std::filesystem::exists(dir));
  }

  {
    // 自更新阶段一：地址白名单、文件名清洗、双校验与失败清理。
    const std::filesystem::path dir = temp_db().parent_path();
    run_update_package_case(dir);
    std::filesystem::remove_all(dir);
    CHECK(!std::filesystem::exists(dir));
  }

  {
    // 更新清单 v2：Ed25519 验签与本平台解析。
    run_update_manifest_case();
    // 灰度判定 fail-closed（0% 不放行）。
    run_rollout_decision_case();
  }

  {
    // 导入模板：第一个工作表只有表头，且与导入校验共用同一份常量。
    const std::filesystem::path dir = temp_db().parent_path();
    run_xlsx_template_case(dir);
    check_db_closed(dir);
    std::filesystem::remove_all(dir);
    CHECK(!std::filesystem::exists(dir));
  }

  {
    // M2 硬验收②：同一文件连续导入两次，不覆盖、不合并、不跳过。
    const std::filesystem::path dir = temp_db().parent_path();
    run_xlsx_reimport_case(dir);
    check_db_closed(dir);
    std::filesystem::remove_all(dir);
    CHECK(!std::filesystem::exists(dir));
  }

  {
    // 必填项缺失：只跳过该行并报行号，其余行照常入库。
    const std::filesystem::path dir = temp_db().parent_path();
    run_xlsx_invalid_row_case(dir);
    check_db_closed(dir);
    std::filesystem::remove_all(dir);
    CHECK(!std::filesystem::exists(dir));
  }

  {
    // 合并重复：每组保留最早一条、补空电话、幂等。
    const std::filesystem::path dir = temp_db().parent_path();
    run_dedupe_case(dir);
    check_db_closed(dir);
    std::filesystem::remove_all(dir);
    CHECK(!std::filesystem::exists(dir));
  }

  std::filesystem::remove_all(file.parent_path());
  std::cout << "断言 " << checks << " 条执行，失败 " << failures << " 条，跳过 "
            << skips << " 条\n";
  if (failures) {
    std::cerr << failures << " 项失败\n";
    return 1;
  }
  std::cout << "core tests passed\n";
  return 0;
}

int main(int argc, char** argv) {
  // 被 curl_fetch 当作 curl 调起来时，先走替身分支。
  if (argc > 1 && std::string(argv[1]) == "--fail") {
    return run_curl_stub(argc, argv);
  }
  // 未捕获的异常在这里落成一行可读输出：否则 0xC0000409 的 fail-fast
  // 只会留下一个退出码，看不出坏在哪条路径上。
  try {
    return run_all_tests();
  } catch (const std::exception& error) {
    std::cerr << "未捕获异常：" << error.what() << "\n";
    return 1;
  }
}
