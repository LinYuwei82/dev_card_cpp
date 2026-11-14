#include "devcard/server.h"

#include <cstdlib>
#include <stdexcept>

#include "devcard/error.h"

namespace devcard {

std::filesystem::path default_data_dir() {
#ifdef _WIN32
  if (const char* local = std::getenv("LOCALAPPDATA"); local && *local) {
    return std::filesystem::path(local) / "Anye" / "DevCard";
  }
  return std::filesystem::temp_directory_path() / "Anye" / "DevCard";
#elif defined(__APPLE__)
  if (const char* home = std::getenv("HOME"); home && *home) {
    return std::filesystem::path(home) / "Library" / "Application Support" /
           "Anye" / "DevCard";
  }
  return std::filesystem::temp_directory_path() / "Anye" / "DevCard";
#else
  if (const char* home = std::getenv("HOME"); home && *home) {
    return std::filesystem::path(home) / ".local" / "share" / "Anye" / "DevCard";
  }
  return std::filesystem::temp_directory_path() / "Anye" / "DevCard";
#endif
}

}  // namespace devcard
