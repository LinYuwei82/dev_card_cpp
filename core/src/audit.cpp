#include "devcard/audit.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>

namespace devcard {
namespace {

std::string padded(int value, int width) {
  char buffer[16] = {};
  std::snprintf(buffer, sizeof(buffer), "%0*d", width, value);
  return buffer;
}

}  // namespace

void record_audit(sqlite3* conn, const AuditEntry& entry) {
  const std::string at = entry.at.empty() ? now_iso8601_utc() : entry.at;
  Stmt stmt(conn,
            "INSERT INTO audit_log(at,actor,actor_ip,action,entity,entity_id,"
            "summary,before_json,after_json) VALUES(?,?,?,?,?,?,?,?,?)");
  stmt.bind(1, at)
      .bind(2, entry.actor)
      .bind(3, entry.actor_ip)
      .bind(4, entry.action)
      .bind(5, entry.entity)
      .bind(6, entry.entity_id)
      .bind(7, entry.summary)
      .bind(8, entry.before_json)
      .bind(9, entry.after_json);
  stmt.run();
}

std::vector<AuditEntry> list_audit(const Db& db, int limit) {
  const int capped = std::clamp(limit, 1, 1000);
  std::vector<AuditEntry> entries;
  db.read([&](sqlite3* conn) {
    Stmt stmt(conn,
              "SELECT id,at,actor,actor_ip,action,entity,entity_id,summary,"
              "before_json,after_json FROM audit_log ORDER BY id DESC LIMIT ?");
    stmt.bind(1, static_cast<std::int64_t>(capped));
    while (stmt.step()) {
      AuditEntry entry;
      entry.id = stmt.i64(0);
      entry.at = stmt.text(1);
      entry.actor = stmt.text(2);
      entry.actor_ip = stmt.text(3);
      entry.action = stmt.text(4);
      entry.entity = stmt.text(5);
      entry.entity_id = stmt.text(6);
      entry.summary = stmt.text(7);
      entry.before_json = stmt.text(8);
      entry.after_json = stmt.text(9);
      entries.push_back(std::move(entry));
    }
  });
  return entries;
}

std::string now_iso8601_utc() {
  const auto now = std::chrono::system_clock::now();
  const std::time_t seconds = std::chrono::system_clock::to_time_t(now);
  const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(
                          now.time_since_epoch()) %
                      1000;
  std::tm tm{};
#ifdef _WIN32
  gmtime_s(&tm, &seconds);
#else
  gmtime_r(&seconds, &tm);
#endif
  std::string out = padded(tm.tm_year + 1900, 4) + "-" + padded(tm.tm_mon + 1, 2) +
                    "-" + padded(tm.tm_mday, 2) + "T" + padded(tm.tm_hour, 2) +
                    ":" + padded(tm.tm_min, 2) + ":" + padded(tm.tm_sec, 2) + "." +
                    padded(static_cast<int>(millis.count()), 3) + "Z";
  return out;
}

std::string json_escape(std::string_view text) {
  std::string out;
  out.reserve(text.size() + 8);
  for (const char ch : text) {
    switch (ch) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (static_cast<unsigned char>(ch) < 0x20) {
          char buffer[8] = {};
          std::snprintf(buffer, sizeof(buffer), "\\u%04x",
                        static_cast<unsigned char>(ch));
          out += buffer;
        } else {
          out.push_back(ch);
        }
        break;
    }
  }
  return out;
}

}  // namespace devcard
