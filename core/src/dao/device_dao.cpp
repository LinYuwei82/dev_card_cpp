#include "devcard/dao/device_dao.h"

#include <algorithm>
#include <string>

#include "devcard/error.h"

namespace devcard {
namespace {

const char* kColumns =
    "id, dev_name, location, control_range, phone, created_at, updated_at,"
    " version, updated_by";

bool is_sortable(const std::string& column) {
  static const char* allowed[] = {"id", "dev_name", "location",
                                  "control_range", "phone", "created_at",
                                  "updated_at"};
  return std::any_of(std::begin(allowed), std::end(allowed),
                     [&](const char* item) { return column == item; });
}

void read_row(Stmt& stmt, Device& device) {
  device.id = stmt.i64(0);
  device.dev_name = stmt.text(1);
  device.location = stmt.text(2);
  device.control_range = stmt.text(3);
  device.phone = stmt.text(4);
  device.created_at = stmt.text(5);
  device.updated_at = stmt.text(6);
  device.version = stmt.i64(7);
  device.updated_by = stmt.text(8);
}

}  // namespace

std::string DeviceDao::where_clause(const DeviceQuery& query) {
  std::string sql;
  auto add = [&](const std::string& condition) {
    sql += sql.empty() ? " WHERE " : " AND ";
    sql += condition;
  };

  if (!query.keyword.empty()) {
    static const char* fields[] = {"dev_name", "location", "control_range",
                                   "phone"};
    if (query.field.empty()) {
      std::string group;
      for (const char* field : fields) {
        if (!group.empty()) group += " OR ";
        group += std::string(field) + " LIKE ? ESCAPE '\\'";
      }
      add("(" + group + ")");
    } else {
      const bool valid = std::any_of(std::begin(fields), std::end(fields),
                                     [&](const char* field) {
                                       return query.field == field;
                                     });
      if (!valid) throw AppError("bad_field", "不支持的关键字字段");
      add(query.field + " LIKE ? ESCAPE '\\'");
    }
  }
  if (!query.name.empty()) add("dev_name = ?");
  if (!query.location.empty()) add("location = ?");
  return sql;
}

void DeviceDao::bind_filters(Stmt& stmt, const DeviceQuery& query, int start) {
  int index = start;
  if (!query.keyword.empty()) {
    std::string escaped;
    escaped.reserve(query.keyword.size() + 2);
    for (char c : query.keyword) {
      if (c == '%' || c == '_' || c == '\\') escaped.push_back('\\');
      escaped.push_back(c);
    }
    const std::string pattern = "%" + escaped + "%";
    if (query.field.empty()) {
      for (int i = 0; i < 4; ++i) stmt.bind(index++, pattern);
    } else {
      stmt.bind(index++, pattern);
    }
  }
  if (!query.name.empty()) stmt.bind(index++, query.name);
  if (!query.location.empty()) stmt.bind(index++, query.location);
}

std::vector<Device> DeviceDao::list(const Db& db, const DeviceQuery& query) const {
  const std::string sort = is_sortable(query.sort) ? query.sort : "id";
  const int page = std::max(0, query.page);
  const int page_size = std::clamp(query.page_size, 1, 1000);

  std::vector<Device> devices;
  db.read([&](sqlite3* conn) {
    const std::string sql = std::string("SELECT ") + kColumns + " FROM device" +
                            where_clause(query) + " ORDER BY " + sort +
                            (query.desc ? " DESC" : " ASC") +
                            " LIMIT ? OFFSET ?";
    Stmt stmt(conn, sql);
    bind_filters(stmt, query, 1);
    // 绑定参数个数必须与 where_clause 一致，单独计算偏移。
    int used = 0;
    if (!query.keyword.empty()) used += query.field.empty() ? 4 : 1;
    if (!query.name.empty()) ++used;
    if (!query.location.empty()) ++used;
    // 偏移用 64 位算：page 没有上限，page * page_size 在 int 上溢出会变成负数，
    // SQLite 把负 OFFSET 当 0，于是返回第一页却把大页码回给前端。
    const std::int64_t offset = static_cast<std::int64_t>(page) * page_size;
    stmt.bind(used + 1, page_size).bind(used + 2, offset);
    while (stmt.step()) {
      Device device;
      read_row(stmt, device);
      devices.push_back(std::move(device));
    }
  });
  return devices;
}

std::int64_t DeviceDao::count(const Db& db, const DeviceQuery& query) const {
  std::int64_t total = 0;
  db.read([&](sqlite3* conn) {
    Stmt stmt(conn, "SELECT COUNT(*) FROM device" + where_clause(query));
    bind_filters(stmt, query, 1);
    if (stmt.step()) total = stmt.i64(0);
  });
  return total;
}

bool DeviceDao::find(const Db& db, std::int64_t id, Device& out) const {
  bool found = false;
  db.read([&](sqlite3* conn) { found = find(conn, id, out); });
  return found;
}

bool DeviceDao::find(sqlite3* conn, std::int64_t id, Device& out) const {
  Stmt stmt(conn, std::string("SELECT ") + kColumns + " FROM device WHERE id = ?");
  stmt.bind(1, id);
  if (!stmt.step()) return false;
  read_row(stmt, out);
  return true;
}

std::int64_t DeviceDao::insert(sqlite3* conn, Device& device) const {
  if (device.dev_name.empty() || device.location.empty() ||
      device.control_range.empty()) {
    throw AppError("invalid_device", "设备名称、位置、控制范围不能为空");
  }
  const std::string now = now_text();
  if (device.created_at.empty()) device.created_at = now;
  if (device.updated_at.empty()) device.updated_at = now;

  Stmt stmt(conn,
            "INSERT INTO device(dev_name,location,control_range,phone,"
            "created_at,updated_at,version,updated_by)"
            " VALUES(?,?,?,?,?,?,1,'')");
  stmt.bind(1, device.dev_name)
      .bind(2, device.location)
      .bind(3, device.control_range)
      .bind(4, device.phone)
      .bind(5, device.created_at)
      .bind(6, device.updated_at);
  stmt.run();
  device.id = sqlite3_last_insert_rowid(conn);
  device.version = 1;
  device.updated_by.clear();
  return device.id;
}

// 乐观锁：版本条件写在 SQL 里，和「version+1」同一条语句。
// 不能先读后比再写——两个请求之间仍可能插入一次提交。
bool DeviceDao::update(sqlite3* conn, const Device& device,
                       std::string_view updated_by) const {
  const std::string actor(updated_by);
  Stmt stmt(conn,
            "UPDATE device SET dev_name=?, location=?, control_range=?, phone=?,"
            " updated_at=?, updated_by=CASE WHEN ?='' THEN updated_by ELSE ? END,"
            " version=version+1 WHERE id=? AND version=?");
  stmt.bind(1, device.dev_name)
      .bind(2, device.location)
      .bind(3, device.control_range)
      .bind(4, device.phone)
      .bind(5, now_text())
      .bind(6, actor)
      .bind(7, actor)
      .bind(8, device.id)
      .bind(9, device.version);
  stmt.run();
  return sqlite3_changes(conn) > 0;
}

bool DeviceDao::erase(sqlite3* conn, std::int64_t id,
                      std::int64_t version) const {
  Stmt stmt(conn, "DELETE FROM device WHERE id = ? AND version = ?");
  stmt.bind(1, id).bind(2, version);
  stmt.run();
  return sqlite3_changes(conn) > 0;
}

void DeviceDao::erase_all(sqlite3* conn) const { exec_sql(conn, "DELETE FROM device"); }

std::int64_t DeviceDao::dedupe(sqlite3* conn,
                               std::string_view updated_by) const {
  const std::string actor(updated_by);

  // 1) 补电话：只动每组 id 最小的那一行，且仅当它电话为空、组内还有非空电话。
  //    电话是从重复行里「捡回来」的，不是覆盖：有值的那行才是原始数据。
  {
    Stmt stmt(conn,
              "UPDATE device SET"
              " phone=(SELECT d2.phone FROM device d2"
              "        WHERE d2.dev_name=device.dev_name"
              "          AND d2.location=device.location"
              "          AND d2.control_range=device.control_range"
              "          AND d2.phone<>'' ORDER BY d2.id LIMIT 1),"
              " updated_at=?,"
              " updated_by=CASE WHEN ?='' THEN updated_by ELSE ? END,"
              " version=version+1"
              " WHERE phone=''"
              "   AND id IN (SELECT MIN(id) FROM device"
              "              GROUP BY dev_name, location, control_range)"
              "   AND EXISTS (SELECT 1 FROM device d2"
              "               WHERE d2.dev_name=device.dev_name"
              "                 AND d2.location=device.location"
              "                 AND d2.control_range=device.control_range"
              "                 AND d2.phone<>'')");
    stmt.bind(1, now_text()).bind(2, actor).bind(3, actor);
    stmt.run();
  }

  // 2) 删重复：每组只保留 id 最小的一行，其余删掉。
  Stmt stmt(conn,
            "DELETE FROM device WHERE id NOT IN"
            " (SELECT MIN(id) FROM device"
            "  GROUP BY dev_name, location, control_range)");
  stmt.run();
  return sqlite3_changes(conn);
}

bool DeviceDao::exists_triplet(sqlite3* conn, std::string_view name,
                               std::string_view location,
                               std::string_view control_range) const {
  Stmt stmt(conn,
            "SELECT 1 FROM device WHERE dev_name = ? AND location = ? "
            "AND control_range = ? LIMIT 1");
  stmt.bind(1, name).bind(2, location).bind(3, control_range);
  return stmt.step();
}

std::vector<std::string> DeviceDao::distinct_names(const Db& db) const {
  std::vector<std::string> values;
  db.read([&](sqlite3* conn) {
    Stmt stmt(conn, "SELECT DISTINCT dev_name FROM device ORDER BY dev_name");
    while (stmt.step()) values.push_back(stmt.text(0));
  });
  return values;
}

std::vector<std::string> DeviceDao::distinct_locations(const Db& db) const {
  std::vector<std::string> values;
  db.read([&](sqlite3* conn) {
    Stmt stmt(conn, "SELECT DISTINCT location FROM device ORDER BY location");
    while (stmt.step()) values.push_back(stmt.text(0));
  });
  return values;
}

}  // namespace devcard
