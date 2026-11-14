#include "config.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <stdexcept>

#include <nlohmann/json.hpp>

#include "devcard/error.h"
#include "devcard/paths.h"
#include "devcard/server.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <windows.h>
#else
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <unistd.h>
#endif

namespace devcard {
namespace {

using nlohmann::json;

constexpr const char* kConfigFileName = "devcard-server.json";
constexpr const char* kDefaultListenSource = "自动选择私有网卡";
constexpr int kDefaultPort = 18080;

std::optional<std::string> env_value(const char* name) {
#ifdef _WIN32
  char* buffer = nullptr;
  std::size_t length = 0;
  if (_dupenv_s(&buffer, &length, name) == 0 && buffer) {
    std::string value(buffer);
    std::free(buffer);
    if (!value.empty()) return value;
  }
  return std::nullopt;
#else
  const char* value = std::getenv(name);
  if (value && *value) return std::string(value);
  return std::nullopt;
#endif
}

std::filesystem::path resolve_relative(const std::filesystem::path& base,
                                       const std::filesystem::path& path) {
  return path.is_relative() ? base / path : path;
}

int parse_port(const std::string& text, const char* source) {
  try {
    const std::size_t used = text.find_first_not_of("0123456789");
    if (used != std::string::npos) throw std::invalid_argument("port");
    const int port = std::stoi(text);
    if (port < 1 || port > 65535) throw std::out_of_range("port");
    return port;
  } catch (const std::exception&) {
    throw AppError("bad_config",
                   std::string(source) + " 的端口不是 1–65535 的整数: " + text);
  }
}

struct FileConfig {
  bool exists = false;
  std::optional<std::string> data_dir;
  std::optional<std::string> listen;
  std::optional<int> port;
  std::vector<std::string> allowed_hosts;
};

FileConfig read_config_file(const std::filesystem::path& path) {
  FileConfig config;
  std::error_code ec;
  if (!std::filesystem::exists(path, ec)) return config;

  std::ifstream in(path, std::ios::binary);
  if (!in) {
    throw AppError("bad_config", "无法读取配置文件: " + path.string());
  }
  const std::string text((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());
  const json doc = json::parse(text, nullptr, false);
  if (doc.is_discarded() || !doc.is_object()) {
    throw AppError("bad_config",
                   "配置文件必须是 JSON 对象: " + path.string());
  }

  for (const auto& [key, value] : doc.items()) {
    if (key == "data_dir") {
      if (!value.is_string()) {
        throw AppError("bad_config", "配置项 data_dir 必须是字符串");
      }
      config.data_dir = value.get<std::string>();
    } else if (key == "listen") {
      if (!value.is_string()) {
        throw AppError("bad_config", "配置项 listen 必须是字符串");
      }
      config.listen = value.get<std::string>();
    } else if (key == "port") {
      if (!value.is_number_integer()) {
        throw AppError("bad_config", "配置项 port 必须是整数");
      }
      const int port = value.get<int>();
      if (port < 1 || port > 65535) {
        throw AppError("bad_config", "配置项 port 必须在 1–65535 之间");
      }
      config.port = port;
    } else if (key == "allowed_hosts") {
      if (!value.is_array()) {
        throw AppError("bad_config", "配置项 allowed_hosts 必须是字符串数组");
      }
      for (const auto& item : value) {
        if (!item.is_string()) {
          throw AppError("bad_config", "配置项 allowed_hosts 必须是字符串数组");
        }
        config.allowed_hosts.push_back(item.get<std::string>());
      }
    } else {
      // 未知键报错，不静默忽略：拼错的键会让用户以为配置生效了。
      throw AppError("bad_config", "配置文件包含未知键: " + key);
    }
  }
  config.exists = true;
  return config;
}

}  // namespace

bool is_private_ipv4(const std::string& address) {
  unsigned parts[4] = {};
  int index = 0;
  unsigned value = 0;
  bool has_digit = false;
  for (const char ch : address) {
    if (ch >= '0' && ch <= '9') {
      value = value * 10 + static_cast<unsigned>(ch - '0');
      if (value > 255) return false;
      has_digit = true;
    } else if (ch == '.') {
      if (!has_digit || index >= 3) return false;
      parts[index++] = value;
      value = 0;
      has_digit = false;
    } else {
      return false;
    }
  }
  if (!has_digit || index != 3) return false;
  parts[3] = value;

  if (parts[0] == 10) return true;
  if (parts[0] == 172 && parts[1] >= 16 && parts[1] <= 31) return true;
  if (parts[0] == 192 && parts[1] == 168) return true;
  return false;
}

bool is_wildcard_listen(const std::string& address) {
  return address == "0.0.0.0" || address == "::" || address == "[::]";
}

std::vector<std::string> private_ipv4_addresses() {
  std::vector<std::string> result;
  auto add = [&](const std::string& ip) {
    if (!is_private_ipv4(ip)) return;
    if (std::find(result.begin(), result.end(), ip) == result.end()) {
      result.push_back(ip);
    }
  };

#ifdef _WIN32
  ULONG size = 16 * 1024;
  std::vector<BYTE> buffer(size);
  auto* addresses = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
  ULONG status = GetAdaptersAddresses(
      AF_INET,
      GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
      nullptr, addresses, &size);
  if (status == ERROR_BUFFER_OVERFLOW) {
    buffer.resize(size);
    addresses = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
    status = GetAdaptersAddresses(
        AF_INET,
        GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
            GAA_FLAG_SKIP_DNS_SERVER,
        nullptr, addresses, &size);
  }
  if (status != NO_ERROR) return result;

  for (auto* adapter = addresses; adapter; adapter = adapter->Next) {
    if (adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
    for (auto* unicast = adapter->FirstUnicastAddress; unicast;
         unicast = unicast->Next) {
      if (!unicast->Address.lpSockaddr ||
          unicast->Address.lpSockaddr->sa_family != AF_INET) {
        continue;
      }
      const auto* sin =
          reinterpret_cast<const sockaddr_in*>(unicast->Address.lpSockaddr);
      char text[INET_ADDRSTRLEN] = {};
      if (!inet_ntop(AF_INET, &sin->sin_addr, text, sizeof(text))) continue;
      add(text);
    }
  }
#else
  ifaddrs* list = nullptr;
  if (getifaddrs(&list) != 0) return result;
  for (ifaddrs* item = list; item; item = item->ifa_next) {
    if (!item->ifa_addr || item->ifa_addr->sa_family != AF_INET) continue;
    if (item->ifa_flags & IFF_LOOPBACK) continue;
    const auto* sin = reinterpret_cast<const sockaddr_in*>(item->ifa_addr);
    char text[INET_ADDRSTRLEN] = {};
    if (!inet_ntop(AF_INET, &sin->sin_addr, text, sizeof(text))) continue;
    add(text);
  }
  freeifaddrs(list);
#endif
  return result;
}

std::string local_host_name() {
#ifdef _WIN32
  wchar_t buffer[256] = {};
  DWORD size = static_cast<DWORD>(std::size(buffer));
  if (!GetComputerNameW(buffer, &size)) return {};
  const int bytes = WideCharToMultiByte(CP_UTF8, 0, buffer, static_cast<int>(size),
                                        nullptr, 0, nullptr, nullptr);
  if (bytes <= 0) return {};
  std::string name(static_cast<std::size_t>(bytes), '\0');
  WideCharToMultiByte(CP_UTF8, 0, buffer, static_cast<int>(size), name.data(),
                      bytes, nullptr, nullptr);
  return name;
#else
  char buffer[256] = {};
  if (gethostname(buffer, sizeof(buffer) - 1) != 0) return {};
  return buffer;
#endif
}

ServerConfig resolve_config(const CliOverrides& cli) {
  const std::filesystem::path exe = executable_dir();
  ServerConfig config;

  config.config_file = exe / kConfigFileName;
  config.config_source = "exe 同目录默认";
  if (cli.config) {
    config.config_file = *cli.config;
    config.config_source = "命令行 --config";
  } else if (const auto env = env_value("DEVCARD_CONFIG")) {
    config.config_file = *env;
    config.config_source = "环境变量 DEVCARD_CONFIG";
  }
  config.config_file = resolve_relative(exe, config.config_file);

  FileConfig file;
  std::error_code ec;
  if (std::filesystem::exists(config.config_file, ec)) {
    file = read_config_file(config.config_file);
    config.config_loaded = true;
  } else if (cli.config || config.config_source.rfind("环境变量", 0) == 0) {
    throw AppError("bad_config",
                   "配置文件不存在: " + config.config_file.string());
  } else {
    config.config_source += "（不存在，未使用）";
  }

  if (cli.data_dir) {
    config.data_dir = *cli.data_dir;
    config.data_dir_source = "命令行 --data-dir";
  } else if (const auto env = env_value("DEVCARD_DATA_DIR")) {
    config.data_dir = *env;
    config.data_dir_source = "环境变量 DEVCARD_DATA_DIR";
  } else if (file.data_dir) {
    // 配置文件是 UTF-8：这里必须按 UTF-8 还原，否则 ACP 机器上 emoji/生僻字路径
    // 会被按 GBK 再解释一次（命令行/环境变量那两条来源本身就是 ACP，保持原样）。
    config.data_dir = path_from_utf8(*file.data_dir);
    config.data_dir_source = "配置文件 data_dir";
  } else {
    config.data_dir = default_data_dir();
    config.data_dir_source =
        std::string("形态默认（") +
        (is_installed_layout() ? "安装版" : "便携版") + "）";
  }
  config.data_dir = resolve_relative(exe, config.data_dir);

  if (cli.listen) {
    config.listen = *cli.listen;
    config.listen_source = "命令行 --listen";
  } else if (const auto env = env_value("DEVCARD_LISTEN")) {
    config.listen = *env;
    config.listen_source = "环境变量 DEVCARD_LISTEN";
  } else if (file.listen) {
    config.listen = *file.listen;
    config.listen_source = "配置文件 listen";
  } else {
    config.listen.clear();
    config.listen_source = kDefaultListenSource;
  }

  if (cli.port) {
    config.port = *cli.port;
    config.port_source = "命令行 --port";
  } else if (const auto env = env_value("DEVCARD_PORT")) {
    config.port = parse_port(*env, "环境变量 DEVCARD_PORT");
    config.port_source = "环境变量 DEVCARD_PORT";
  } else if (file.port) {
    config.port = *file.port;
    config.port_source = "配置文件 port";
  } else {
    config.port = kDefaultPort;
    config.port_source = "默认";
  }

  config.allow_any = cli.allow_any;
  config.allowed_hosts = file.allowed_hosts;
  return config;
}

std::string select_listen_address(const ServerConfig& config) {
  if (!config.listen.empty()) return config.listen;

  const std::vector<std::string> candidates = private_ipv4_addresses();
  if (candidates.size() == 1) return candidates.front();

  std::string detail;
  for (const std::string& candidate : candidates) {
    detail += "\n  - " + candidate;
  }
  if (candidates.empty()) {
    throw AppError("bad_listen",
                   "未找到可用的私有 IPv4 地址（10/172.16–31/192.168 网段）。"
                   "请用 --listen <ip> 显式指定监听地址。");
  }
  throw AppError("bad_listen",
                 "检测到多块私有 IPv4 网卡，无法自动选择：" + detail +
                     "\n请用 --listen <ip> 显式指定监听地址。");
}

}  // namespace devcard
