#include "guards.h"

#include <algorithm>
#include <cctype>
#include <string>

namespace devcard::detail {
namespace {

std::string lowercase(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

constexpr const char* kBypassExactPaths[] = {"/api/health"};
constexpr const char* kBypassPrefixes[] = {"/api/session/"};

}  // namespace

bool is_loopback(const std::string& address) {
  return address == "127.0.0.1" || address == "::1" ||
         address == "::ffff:127.0.0.1";
}

bool host_allowed(const ServerState& state, std::string host) {
  if (host.empty()) return false;
  std::string name = lowercase(host);
  if (!name.empty() && name.front() == '[') {
    const std::size_t end = name.find(']');
    if (end != std::string::npos) name = name.substr(1, end - 1);
  } else {
    const std::size_t colon = name.find(':');
    if (colon != std::string::npos) name = name.substr(0, colon);
  }
  if (name == "127.0.0.1" || name == "localhost" || name == "::1") return true;
  if (!state.network) return false;
  if (!state.bound_host.empty() && name == lowercase(state.bound_host)) return true;
  if (!state.machine_name.empty() && name == lowercase(state.machine_name)) {
    return true;
  }
  for (const std::string& allowed : state.allowed_hosts) {
    if (name == lowercase(allowed)) return true;
  }
  return false;
}

bool is_bypass_path(const std::string& path) {
  for (const char* exact : kBypassExactPaths) {
    if (path == exact) return true;
  }
  for (const char* prefix : kBypassPrefixes) {
    if (path.rfind(prefix, 0) == 0) return true;
  }
  return false;
}

}  // namespace devcard::detail
