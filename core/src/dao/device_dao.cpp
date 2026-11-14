#include "devcard/dao/device_dao.h"

#include <algorithm>
#include <string>

#include "devcard/error.h"

namespace devcard {
namespace {

const char* kColumns =
    "id, dev_name, location, control_range, phone, created_at, updated_at";

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

std::vector<Device> DeviceDao::list(Db& db, const DeviceQuery& query) const {
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
    int index = 1;
    bind_filters(stmt, query, index);
    index += 0;
    // 绑定参数个数必须与 where_clause 一致，单独计算偏移。
    int used = 0;
    if (!query.keyword.empty()) used += query.field.empty() ? 4 : 1;
    if (!query.name.empty()) ++used;
    if (!query.location.empty()) ++used;
    stmt.bind(used + 1, page_size).bind(used + 2, page * page_size);
    while (stmt.step()) {
      Device device;
      read_row(stmt, device);
      devices.push_back(std::move(device));
    }
  });
  return devices;
}

std::int64_t DeviceDao::count(Db& db, const DeviceQuery& query) const {
  std::int64_t total = 0;
  db.read([&](sqlite3* conn) {
    Stmt stmt(conn, "SELECT COUNT(*) FROM device" + where_clause(query));
    bind_filters(stmt, query, 1);
    if (stmt.step()) total = stmt.i64(0);
  });
  return total;
}

bool DeviceDao::find(Db& db, std::int64_t id, Device& out) const {
  bool found = false;
  db.read([&](sqlite3* conn) {
    Stmt stmt(conn, std::string("SELECT ") + kColumns + " FROM device WHERE id = ?");
    stmt.bind(1, id);
    if (stmt.step()) {
      read_row(stmt, out);
      found = true;
    }
  });
  return found;
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
            "created_at,updated_at) VALUES(?,?,?,?,?,?)");
  stmt.bind(1, device.dev_name)
      .bind(2, device.location)
      .bind(3, device.control_range)
      .bind(4, device.phone)
      .bind(5, device.created_at)
      .bind(6, device.updated_at);
  stmt.run();
  device.id = sqlite3_last_insert_rowid(conn);
  return device.id;
}

void DeviceDao::update(sqlite3* conn, const Device& device) const {
  Stmt stmt(conn,
            "UPDATE device SET dev_name=?, location=?, control_range=?, phone=?,"
            " updated_at=? WHERE id=?");
  stmt.bind(1, device.dev_name)
      .bind(2, device.location)
      .bind(3, device.control_range)
      .bind(4, device.phone)
      .bind(5, now_text())
      .bind(6, device.id);
  stmt.run();
}

void DeviceDao::erase(sqlite3* conn, std::int64_t id) const {
  Stmt stmt(conn, "DELETE FROM device WHERE id = ?");
  stmt.bind(1, id);
  stmt.run();
}

void DeviceDao::erase_all(sqlite3* conn) const { exec_sql(conn, "DELETE FROM device"); }

std::vector<std::string> DeviceDao::distinct_names(Db& db) const {
  std::vector<std::string> values;
  db.read([&](sqlite3* conn) {
    Stmt stmt(conn, "SELECT DISTINCT dev_name FROM device ORDER BY dev_name");
    while (stmt.step()) values.push_back(stmt.text(0));
  });
  return values;
}

std::vector<std::string> DeviceDao::distinct_locations(Db& db) const {
  std::vector<std::string> values;
  db.read([&](sqlite3* conn) {
    Stmt stmt(conn, "SELECT DISTINCT location FROM device ORDER BY location");
    while (stmt.step()) values.push_back(stmt.text(0));
  });
  return values;
}

}  // namespace devcard
