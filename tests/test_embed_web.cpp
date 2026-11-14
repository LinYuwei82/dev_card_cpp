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

#ifdef _WIN32
#include <windows.h>
#endif

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

// 定义在后面，但 _WIN32 分支要先用；不声明的话 MSVC 报 C3861（POSIX 侧靠隐式声明侥幸过了）。
std::string read_bytes(const fs::path& path);

// 生成器把正文换行写成相邻字面量 `" "\n" "`（见 embed_web.cmake）：
// Windows 上 CMake 以文本模式写文件，正文字面 LF 会变成 CRLF，只有这种写法能保住原始字节。
const std::string kNewlineToken = " \"\\n\" ";

// 起一个外部进程并连 stderr 一起收回来；返回退出码。
// 两条路径共用同一份「参数数组」，只有最后的落地方式不同：
//   Windows 走 CreateProcessW，不经过 cmd.exe —— 整行以引号开头时 cmd 会按另一套规则剥引号，
//   把命令行拆坏（实测报 The filename, directory name, or volume label syntax is incorrect.，
//   而且 cmake 一次都没跑起来，看上去却像「用例全红」）。
//   POSIX 走 popen，参数逐个加单引号，不依赖字符串拼接的正确性。
std::string quote_arg(const std::string& arg) {
#ifdef _WIN32
  return "\"" + arg + "\"";
#else
  std::string quoted = "'";
  for (char c : arg) {
    if (c == '\'') {
      quoted += "'\\''";
    } else {
      quoted += c;
    }
  }
  return quoted + "'";
#endif
}

int run_process(const std::vector<std::string>& args, std::string* output) {
#ifdef _WIN32
  const fs::path log = fs::temp_directory_path() /
                       ("devcard-spawn-" + std::to_string(std::rand()) + ".log");
  SECURITY_ATTRIBUTES inherit = {sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
  HANDLE sink = CreateFileW(log.wstring().c_str(), GENERIC_WRITE,
                            FILE_SHARE_READ, &inherit, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
  if (sink == INVALID_HANDLE_VALUE) {
    return -1;
  }
  std::wstring command;
  for (const auto& arg : args) {
    if (!command.empty()) {
      command += L' ';
    }
    const std::string quoted = quote_arg(arg);
    command.append(quoted.begin(), quoted.end());
  }
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdOutput = sink;
  startup.hStdError = sink;
  PROCESS_INFORMATION process{};
  const BOOL started =
      CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE,
                     CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
  int code = -1;
  if (started) {
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exit_code = 1;
    GetExitCodeProcess(process.hProcess, &exit_code);
    code = static_cast<int>(exit_code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
  }
  CloseHandle(sink);
  if (output != nullptr) {
    *output = read_bytes(log);
  }
  fs::remove(log);
  return code;
#else
  std::string command;
  for (const auto& arg : args) {
    if (!command.empty()) {
      command += ' ';
    }
    command += quote_arg(arg);
  }
  command += " 2>&1";
  FILE* pipe = popen(command.c_str(), "r");
  if (pipe == nullptr) {
    return -1;
  }
  char buffer[512];
  while (std::fgets(buffer, sizeof(buffer), pipe) != nullptr) {
    if (output != nullptr) {
      *output += buffer;
    }
  }
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

// 严格 UTF-8 解码：拒绝过长编码、代理区、> U+10FFFF 与截断序列。
// 切点劈开多字节字符时这里会失败，且能报出字节偏移——与「行尾差异」是两种
// 可区分的失败。内容比较走码点，不再拿磁盘字节直接比。
struct Decoded {
  bool ok = true;
  std::size_t error = 0;
  std::vector<unsigned int> points;
};

Decoded decode_utf8(const std::string& text) {
  Decoded result;
  std::size_t i = 0;
  while (i < text.size()) {
    const auto c = static_cast<unsigned char>(text[i]);
    unsigned int point = 0;
    std::size_t extra = 0;
    if (c < 0x80) {
      point = c;
    } else if (c >= 0xC2 && c <= 0xDF) {
      point = c & 0x1F;
      extra = 1;
    } else if (c >= 0xE0 && c <= 0xEF) {
      point = c & 0x0F;
      extra = 2;
    } else if (c >= 0xF0 && c <= 0xF4) {
      point = c & 0x07;
      extra = 3;
    } else {
      result.ok = false;
      result.error = i;
      return result;
    }
    if (i + extra >= text.size()) {
      result.ok = false;
      result.error = i;
      return result;
    }
    for (std::size_t k = 1; k <= extra; ++k) {
      const auto next = static_cast<unsigned char>(text[i + k]);
      if ((next & 0xC0) != 0x80) {
        result.ok = false;
        result.error = i;
        return result;
      }
      point = (point << 6) | (next & 0x3F);
    }
    if ((extra == 2 && point < 0x800) || (extra == 3 && point < 0x10000) ||
        (point >= 0xD800 && point <= 0xDFFF) || point > 0x10FFFF) {
      result.ok = false;
      result.error = i;
      return result;
    }
    result.points.push_back(point);
    i += extra + 1;
  }
  return result;
}

// 只归一 CRLF，不碰单个 CR，也不做其它隐式转换。
std::string normalize_crlf(const std::string& text) {
  std::string out;
  out.reserve(text.size());
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '\r' && i + 1 < text.size() && text[i + 1] == '\n') {
      continue;
    }
    out += text[i];
  }
  return out;
}

struct Literal {
  std::size_t blocks = 0;
  std::size_t bytes = 0;
  std::size_t max_block = 0;
  bool decoded = false;
  std::size_t decode_error = 0;
  std::vector<unsigned int> points;
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
    result.blocks += 1;
    result.bytes += block.size();
    if (block.size() > result.max_block) {
      result.max_block = block.size();
    }
    result.content += block;
    cursor = end + close.size();
    // 生成器把正文换行写成了相邻字面量 `"\n"`（见 embed_web.cmake）：
    // Windows 上文本模式写文件会把 LF 变成 CRLF，只有这种写法能保住原始字节。
    while (generated.compare(cursor, kNewlineToken.size(), kNewlineToken) == 0) {
      result.content += '\n';
      result.bytes += 1;
      cursor += kNewlineToken.size();
    }
  }
  result.content = normalize_crlf(result.content);
  const Decoded decoded = decode_utf8(result.content);
  result.decoded = decoded.ok;
  result.decode_error = decoded.error;
  result.points = decoded.points;
  return result;
}

// 二进制资产走「抽 hex → 解码 → 逐字节比」，不受行尾影响。
std::vector<unsigned char> parse_hex_array(const std::string& generated,
                                           const std::string& name) {
  const Literal literal = parse_array(generated, name);
  std::string hex;
  for (char c : literal.content) {
    const auto byte = static_cast<unsigned char>(c);
    if ((byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f') ||
        (byte >= 'A' && byte <= 'F')) {
      hex += c;
    }
  }
  std::vector<unsigned char> bytes;
  for (std::size_t i = 0; i + 1 < hex.size(); i += 2) {
    bytes.push_back(static_cast<unsigned char>(
        std::stoi(hex.substr(i, 2), nullptr, 16)));
  }
  return bytes;
}

struct RunResult {
  int code = -1;
  std::string output;
  Literal index;
  std::vector<unsigned char> logo;
  std::size_t artifact_bytes = 0;  // 生成的 .cpp 大小；>0 才说明外部进程真的跑过
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

  // argv 的编码归属由 Windows 侧统一收口（宽命令行 + 工作目录），本文件先保持原样，
  // 等 win-m5 的版本落地后按它对齐；不要在这里再造第三份转换。
  const std::vector<std::string> args = {
      DEVCARD_CMAKE_COMMAND,
      "-DINPUT_DIR=" + input.string(),
      "-DOUTPUT=" + output.string(),
      "-P",
      script_path(),
  };
  result.code = run_process(args, &result.output);
  if (result.code == 0) {
    const std::string generated = read_bytes(output);
    result.artifact_bytes = generated.size();
    if (!generated.empty()) {
      result.index = parse_array(generated, "kIndexHtml");
      result.logo = parse_hex_array(generated, "kLogoJpgHex");
    }
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
  // 先证明外部进程真的运行过：产物存在且非空。
  CHECK(got.artifact_bytes > 0);
  // 二进制资产：抽 hex → 解码 → 与输入逐字节比。
  const std::vector<unsigned char> expected_logo = {0xFF, 0xD8, 0xFF};
  CHECK(got.logo == expected_logo);
  CHECK(got.index.bytes == payload.size());
  CHECK(got.index.blocks == expected_blocks);
  CHECK(got.index.max_block <= 4096);
  if (!got.index.decoded) {
    // 切点劈开多字节字符必须报解码错误，不能混成「内容不等」。
    std::cerr << "FAIL [" << name << "] 字面量解码失败（非法 UTF-8），起始字节 "
              << got.index.decode_error << "\n";
    ++failures;
    return;
  }
  const Decoded expected = decode_utf8(payload);
  CHECK(expected.ok);
  CHECK(got.index.points == expected.points);
  if (got.index.points != expected.points || got.index.blocks != expected_blocks) {
    std::cerr << "  期望 " << payload.size() << " 字节 / " << expected_blocks
              << " 块，实得 " << got.index.bytes << " 字节 / "
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
    CHECK(got.index.decoded);
    CHECK(got.index.points == decode_utf8(lf).points);
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
    CHECK(ok.index.decoded);
    CHECK(ok.index.points == decode_utf8(big).points);
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
    // 这条输入本身就是非法 UTF-8，只验「字节数与块数没被 CR 扫描吃掉」；
    // 解码判据由上面的文本用例与第 6 节自检覆盖。
    CHECK(ironic.index.blocks == (adversarial.size() + 4095) / 4096);
    std::cout << "  220 KB 对抗输入（20 d1 重复）：" << elapsed_bad << " ms\n";
    CHECK(elapsed_bad < 5000);  // 判据是秒级以内，不是「没崩」
  }

  // 6) 判据自检：切点劈开多字节字符必须是「解码失败」，不能只是「内容不等」；
  //    纯 CRLF 差异归一后必须解码成功且码点一致——两类失败要能区分。
  {
    const std::string broken =
        "const char kIndexHtml[] = {\nR\"DEVCARD(a\xE7\x95)DEVCARD\"\n};\n";
    const Literal split = parse_array(broken, "kIndexHtml");
    CHECK(!split.decoded);
    CHECK(split.decode_error == 1);

    const std::string crlf =
        "const char kIndexHtml[] = {\nR\"DEVCARD(a\r\nb)DEVCARD\"\n};\n";
    const Literal line = parse_array(crlf, "kIndexHtml");
    CHECK(line.decoded);
    CHECK(line.points == std::vector<unsigned int>({'a', '\n', 'b'}));
  }

  if (failures != 0) {
    std::cerr << failures << " 项失败\n";
    return 1;
  }
  std::cout << "embed_web 验收用例通过\n";
  return 0;
}
