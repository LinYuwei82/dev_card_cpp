#include "heartbeat.h"

#include <cerrno>
#include <cstdio>
#include <ctime>
#include <string>

#include "config.h"
#include "devcard/audit.h"
#include "devcard/dao/instance_dao.h"
#include "devcard/db.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <csignal>
#include <unistd.h>
#endif

namespace devcard {
namespace {

std::string lowercase(std::string value) {
  for (char& ch : value) {
    if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
  }
  return value;
}

long long process_id() {
#ifdef _WIN32
  return static_cast<long long>(GetCurrentProcessId());
#else
  return static_cast<long long>(getpid());
#endif
}

bool process_alive(long long pid) {
  if (pid <= 0) return false;
#ifdef _WIN32
  HANDLE handle = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
                              FALSE, static_cast<DWORD>(pid));
  if (!handle) {
    // 权限不足时不能断定对方已退出，按「活着」处理，宁可误拒也不双写。
    return GetLastError() == ERROR_ACCESS_DENIED;
  }
  const DWORD result = WaitForSingleObject(handle, 0);
  CloseHandle(handle);
  return result == WAIT_TIMEOUT;
#else
  if (kill(static_cast<pid_t>(pid), 0) == 0) return true;
  return errno == EPERM;
#endif
}

// 解析 now_iso8601_utc() 写出的 "YYYY-MM-DDTHH:MM:SS.mmmZ"。
// 解析失败按「很旧」处理：宁可接管一条坏行，也不要因为一行坏数据永久起不来。
std::time_t parse_utc(const std::string& text) {
  int year = 0;
  int month = 0;
  int day = 0;
  int hour = 0;
  int minute = 0;
  int second = 0;
  int millis = 0;
  if (std::sscanf(text.c_str(), "%d-%d-%dT%d:%d:%d.%dZ", &year, &month, &day,
                  &hour, &minute, &second, &millis) != 7) {
    return 0;
  }
  std::tm tm{};
  tm.tm_year = year - 1900;
  tm.tm_mon = month - 1;
  tm.tm_mday = day;
  tm.tm_hour = hour;
  tm.tm_min = minute;
  tm.tm_sec = second;
#ifdef _WIN32
  return _mkgmtime(&tm);
#else
  return timegm(&tm);
#endif
}

std::string owner_text(const InstanceOwner& owner, long long age_seconds) {
  std::string text = "数据库内的心跳行显示另一个 DevCard 实例正在使用这个库：\n";
  text += "  实例: " + owner.instance_id + "（PID " + std::to_string(owner.pid) +
          " @ " + owner.host + "）\n";
  text += "  启动于: " + owner.started_at + "\n";
  text += "  最近心跳: " + owner.heartbeat_at + "（约 " +
          std::to_string(age_seconds) + " 秒前）\n";
  text += "  监听: " + (owner.listen.empty() ? std::string("未知（尚未绑定端口）")
                                              : owner.listen) +
          "\n";
  text +=
      "即使锁文件被删除或被绕开，这一层也会拒绝第二个实例。\n"
      "如果确认该进程已退出：本机崩溃残留会按 PID 自动接管；"
      "跨机器残留需等待心跳过期（" +
      std::to_string(kStaleAfterSeconds) + " 秒），不要手工改库文件。";
  return text;
}

void write_owner(Conn& conn, const std::string& instance_id) {
  const std::string now = now_iso8601_utc();
  InstanceDao().write(conn, instance_id, process_id(), local_host_name(), now,
                      now);
}

}  // namespace

bool claim_heartbeat(Db& db, const std::string& instance_id,
                     std::string* error) {
  std::string message;
  db.write([&](Conn& conn) {
    InstanceOwner owner;
    if (InstanceDao().read(conn, owner)) {
      const std::time_t beat = parse_utc(owner.heartbeat_at);
      const std::time_t now = std::time(nullptr);
      const long long age =
          beat > 0 ? static_cast<long long>(now - beat) : kStaleAfterSeconds + 1;
      const bool same_host = lowercase(owner.host) == lowercase(local_host_name());
      const bool dead_same_host = same_host && !process_alive(owner.pid);
      const bool stale = beat <= 0 || age > kStaleAfterSeconds || dead_same_host;

      if (!stale) {
        message = owner_text(owner, age < 0 ? 0 : age);
        return;
      }
      // 崩溃残留：同一事务里直接接管，避免两个进程各写一半。
    }
    write_owner(conn, instance_id);
  });
  if (!message.empty()) {
    if (error) *error = message;
    return false;
  }
  // 心跳行必须落到主库文件：hardlink 另一条路径打开时只看得到主库，
  // WAL 里的新行对方不可见，第三层就形同虚设。
  db.checkpoint();
  return true;
}

bool refresh_heartbeat(Db& db, const std::string& instance_id) {
  bool still_owner = false;
  db.write([&](Conn& conn) {
    still_owner = InstanceDao().touch(conn, instance_id, now_iso8601_utc());
  });
  if (still_owner) db.checkpoint();
  return still_owner;
}

void force_heartbeat(Db& db, const std::string& instance_id) {
  db.write([&](Conn& conn) { write_owner(conn, instance_id); });
  db.checkpoint();
}

void set_heartbeat_listen(Db& db, const std::string& instance_id,
                          const std::string& listen) {
  db.write([&](Conn& conn) {
    InstanceDao().set_listen(conn, instance_id, listen);
  });
  db.checkpoint();
}

void release_heartbeat(Db& db, const std::string& instance_id) {
  db.write([&](Conn& conn) {
    InstanceDao().erase(conn, instance_id);
  });
}

}  // namespace devcard
