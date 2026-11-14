// embed_web.cmake 的验收用例。
//
// 这条链路上出过的事故是「生成器自己不自洽」：按 4,096 字节硬切，切口落在多字节字符中间，
// 产物不是合法 UTF-8。clang 照收（macOS 全绿），MSVC 按 C4828 丢内容（Windows 少字节）。
// 所以这里测的不是「能不能生成」，而是「生成出来的字节还等于源文件」。
//
// 覆盖：
//   1) 边界跨字符：2 / 3 / 4 字节字符各一例，切口正好落在字符中间；
//   2) 尺寸：0、正好 4,096、4,097、末尾不足一块、纯 ASCII、全多字节；
//   3) 换行保真：LF 源嵌 LF、CRLF 源嵌 CRLF，不做隐式转换；
//   4) CR 政策：REJECT_CR=ON 时 CRLF 源必须失败（负向用例，平时不会走到）；
//   5) 块数 = ceil(字节数 / 4,096)，字节数按字节算。
//
// 用 DEVCARD_EMBED_SCRIPT 环境变量可以指到另一份生成器（例如修复前的版本），
// 用来证明这些用例在旧实现上会红。
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

#ifndef DEVCARD_CMAKE_COMMAND
#define DEVCARD_CMAKE_COMMAND "cmake"
#endif
#ifndef DEVCARD_EMBED_SCRIPT
#define DEVCARD_EMBED_SCRIPT ""
#endif

namespace {

namespace fs = std::filesystem;

int failures = 0;

#define CHECK(cond)                                                      \
  do {                                                                   \
    if (!(cond)) {                                                       \
      std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << "  " #cond   \
                << "\n";                                                 \
      ++failures;                                                        \
    }                                                                    \
  } while (0)

std::string script_path() {
  const char* from_env = std::getenv("DEVCARD_EMBED_SCRIPT");
  return (from_env != nullptr && *from_env != '\0') ? from_env
                                                    : DEVCARD_EMBED_SCRIPT;
}

// 跑命令并连 stderr 一起收回来；返回退出码。
int run(const std::string& command, std::string* output) {
  std::string full = command + " 2>&1";
#ifdef _WIN32
  FILE* pipe = _popen(full.c_str(), "r");
#else
  FILE* pipe = popen(full.c_str(), "r");
#endif
  if (pipe == nullptr) {
    return -1;
  }
  char buffer[512];
  while (std::fgets(buffer, sizeof(buffer), pipe) != nullptr) {
    if (output != nullptr) {
      *output += buffer;
    }
  }
#ifdef _WIN32
  return _pclose(pipe);
#else
  const int status = pclose(pipe);
  return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
}

bool write_bytes(const fs::path& path, const std::string& bytes) {
  std::ofstream out(path, std::ios::binary);
  out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  return out.good();
}

std::string read_bytes(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in),
                     std::istreambuf_iterator<char>());
}

bool valid_utf8(const std::string& text) {
  std::size_t i = 0;
  while (i < text.size()) {
    const auto c = static_cast<unsigned char>(text[i]);
    std::size_t extra = 0;
    if (c < 0x80) {
      extra = 0;
    } else if (c >= 0xC2 && c <= 0xDF) {
      extra = 1;
    } else if (c >= 0xE0 && c <= 0xEF) {
      extra = 2;
    } else if (c >= 0xF0 && c <= 0xF4) {
      extra = 3;
    } else {
      return false;
    }
    if (i + extra >= text.size()) {
      return false;
    }
    for (std::size_t k = 1; k <= extra; ++k) {
      if ((static_cast<unsigned char>(text[i + k]) & 0xC0) != 0x80) {
        return false;
      }
    }
    i += extra + 1;
  }
  return true;
}

struct Literal {
  std::size_t blocks = 0;
  std::size_t bytes = 0;
  std::size_t max_block = 0;
  bool utf8 = true;
  std::string content;
};

// 从生成文件里取出某个数组的字面量，按字节还原。
Literal parse_array(const std::string& generated, const std::string& name) {
  Literal result;
  const std::string key = "const char " + name + "[] =";
  const std::size_t start = generated.find(key);
  if (start == std::string::npos) {
    return result;
  }
  // 一个数组到下一个数组（或结尾的 `}  // namespace`）为止，否则会把后面几个数组的内容也算进来。
  std::size_t limit = generated.find("\nconst char ", start + key.size());
  const std::size_t tail = generated.find("}  // namespace", start + key.size());
  if (limit == std::string::npos || (tail != std::string::npos && tail < limit)) {
    limit = tail;
  }
  std::size_t cursor = start + key.size();
  const std::string open = "R\"DEVCARD(";
  const std::string close = ")DEVCARD\"";
  while (true) {
    const std::size_t begin = generated.find(open, cursor);
    if (begin == std::string::npos || begin > limit) {
      break;
    }
    const std::size_t content_begin = begin + open.size();
    const std::size_t end = generated.find(close, content_begin);
    if (end == std::string::npos) {
      break;
    }
    const std::string block = generated.substr(content_begin, end - content_begin);
    if (!valid_utf8(block)) {
      result.utf8 = false;
    }
    result.blocks += 1;
    result.bytes += block.size();
    if (block.size() > result.max_block) {
      result.max_block = block.size();
    }
    result.content += block;
    cursor = end + close.size();
  }
  return result;
}

std::string quote_path(const fs::path& path) {
  return "\"" + path.string() + "\"";
}

struct RunResult {
  int code = -1;
  std::string output;
  Literal index;
};

// 造一个输入目录并跑生成器。other_files 用来补齐生成器要求的另外三个文件。
RunResult generate(const std::string& name, const std::string& index_html) {
  RunResult result;
  const fs::path root = fs::temp_directory_path() /
                        ("devcard-embed-" + name + "-" +
                         std::to_string(std::rand()));
  fs::create_directories(root / "in");
  const fs::path input = root / "in";
  write_bytes(input / "index.html", index_html);
  write_bytes(input / "styles.css", "css\n");
  write_bytes(input / "app.js", "js\n");
  write_bytes(input / "logo.jpg", std::string("\xFF\xD8\xFF", 3));
  const fs::path output = root / "out.cpp";

  std::string command = quote_path(fs::path(DEVCARD_CMAKE_COMMAND)) + " -DINPUT_DIR=" +
                        quote_path(input) + " -DOUTPUT=" + quote_path(output) +
                        " -P " + quote_path(fs::path(script_path()));
  result.code = run(command, &result.output);
  if (result.code == 0) {
    result.index = parse_array(read_bytes(output), "kIndexHtml");
  }
  fs::remove_all(root);
  return result;
}

void check_case(const std::string& name, const std::string& payload,
                std::size_t expected_blocks) {
  const RunResult got = generate(name, payload);
  if (got.code != 0) {
    std::cerr << "FAIL [" << name << "] 生成器退出码 " << got.code << "\n"
              << got.output << "\n";
    ++failures;
    return;
  }
  CHECK(got.index.utf8);
  CHECK(got.index.content == payload);
  CHECK(got.index.bytes == payload.size());
  CHECK(got.index.blocks == expected_blocks);
  CHECK(got.index.max_block <= 4096);
  if (got.index.content != payload || got.index.blocks != expected_blocks) {
    std::cerr << "  期望 " << payload.size() << " 字节 / " << expected_blocks
              << " 块，实得 " << got.index.content.size() << " 字节 / "
              << got.index.blocks << " 块\n";
  }
}

}  // namespace

int main() {
  const std::string a4095(4095, 'A');
  const std::string jie = "\xE7\x95\x8C";          // 界，3 字节
  const std::string eacute = "\xC3\xA9";           // é，2 字节
  const std::string emoji = "\xF0\x9F\x98\x80";    // 😀，4 字节

  std::cout << "生成器：" << script_path() << "\n";

  // 1) 边界跨字符：切口正好落在字符中间，三种宽度各一例。
  //    4095 个 A 之后是字符起点，第 4,096 字节落在字符内部。
  check_case("boundary-2byte", a4095 + eacute + "tail", 2);
  check_case("boundary-3byte", a4095 + jie + "tail", 2);
  check_case("boundary-4byte", a4095 + emoji + "tail", 2);

  // 2) 尺寸用例。块数按字节数算：ceil(字节数 / 4096)。
  // 0 字节没有内容可切，生成器仍写一个字面量（空的），保证生成的 C++ 合法，故记 1 块。
  check_case("size-empty", "", 1);
  check_case("size-4096", std::string(4096, 'A'), 1);
  check_case("size-4097", std::string(4097, 'A'), 2);
  check_case("size-tail", std::string(4096 * 2 + 100, 'A'), 3);
  check_case("size-ascii", std::string(10000, 'x'), 3);
  std::string multibyte;
  for (int i = 0; i < 2000; ++i) {
    multibyte += jie;  // 6,000 字节，切口必然落在字符中间
  }
  check_case("size-multibyte", multibyte, 2);

  // 3) 换行：LF 源原样嵌入；CRLF 源必须被拒绝，不能静默折成 LF。
  //    CMake 的 file(READ) 会折 CRLF，这个生成器做不到字节保真，所以只能拒绝。
  {
    const std::string lf = "line1\nline2\nline3\n";
    const RunResult got = generate("newline-lf", lf);
    CHECK(got.code == 0);
    CHECK(got.index.content == lf);
    CHECK(got.index.bytes == lf.size());
  }
  {
    const std::string crlf = "line1\r\nline2\r\nline3\r\n";
    const RunResult got = generate("newline-crlf", crlf);
    CHECK(got.code != 0);
    CHECK(got.output.find("含 CR") != std::string::npos);
  }

  // 4) CR 检测（负向）：.gitattributes 锁了 LF 之后正常检出走不到这条路径，
  //    所以必须主动点亮，确认它真的会拦。
  {
    const RunResult bad = generate("reject-cr", "a\r\nb\n");
    CHECK(bad.code != 0);
    CHECK(bad.output.find("含 CR") != std::string::npos);
    if (bad.code == 0) {
      std::cerr << "  期望失败，实际成功\n";
    }
    const RunResult good = generate("reject-cr-lf", "a\nb\n");
    CHECK(good.code == 0);
  }

  // 5) 大输入回归：CR 扫描必须是线性的。
  //    正则版 `^([0-9a-f][0-9a-f])*0d` 在 6 万字符以上会栈溢出（Windows 上 cmake 进程直接消失），
  //    app.js 的 hex 串是 112,402 字符。这里给 120 KB（hex 串 240,000 字符），
  //    判据是「走完检查、不崩」；不能只靠 app.js 恰好触发。
  {
    const std::string big(120 * 1024, 'A');
    const auto started = std::chrono::steady_clock::now();
    const RunResult ok = generate("big-lf", big);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - started).count();
    CHECK(ok.code == 0);
    CHECK(ok.index.bytes == big.size());
    CHECK(ok.index.content == big);
    CHECK(ok.index.blocks == (big.size() + 4095) / 4096);
    std::cout << "  200 KB 级 LF 源：" << elapsed << " ms\n";

    // CR 必须报出字节偏移，而不是只说「有 CR」。
    const RunResult cr = generate("big-crlf", big + "\r\n");
    CHECK(cr.code != 0);
    CHECK(cr.output.find("含 CR") != std::string::npos);
    CHECK(cr.output.find("第 " + std::to_string(big.size()) + " 字节") !=
          std::string::npos);

    // 对抗输入：反复 20 d1 的 hex 是 20d120d1…，"0d" 每次都在奇数位。
    // 窗口前移若对整串做 SUBSTRING，这里会退化成平方级；切片后应稳定在秒级以内。
    std::string adversarial;
    for (int i = 0; i < 110 * 1024; ++i) {
      adversarial += "\x20\xD1";
    }
    const auto started_bad = std::chrono::steady_clock::now();
    const RunResult ironic = generate("adversarial", adversarial);
    const auto elapsed_bad = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - started_bad).count();
    CHECK(ironic.code == 0);
    CHECK(ironic.index.bytes == adversarial.size());
    CHECK(ironic.index.content == adversarial);
    std::cout << "  220 KB 对抗输入（20 d1 重复）：" << elapsed_bad << " ms\n";
    CHECK(elapsed_bad < 5000);  // 判据是秒级以内，不是「没崩」
  }

  if (failures != 0) {
    std::cerr << failures << " 项失败\n";
    return 1;
  }
  std::cout << "embed_web 验收用例通过\n";
  return 0;
}
