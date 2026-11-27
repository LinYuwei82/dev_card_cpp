#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace devcard {

// 存储层后端无关接口（阶段 1 引入）：
//   - 判据与业务 SQL 仍收口在 DAO（core/src/dao/），本层只定义执行面；
//   - SQLite 实现见 core/src/backend/sqlite_db.cpp（工厂 open_sqlite_db）；
//   - PostgreSQL 实现在阶段 2 按同一接口新增，DAO 不感知后端类型。

// 语句执行面：绑定、单步与取值，不含业务语义。
class StmtView {
 public:
  virtual ~StmtView() = default;

  virtual StmtView& bind(std::int64_t index, std::int64_t value) = 0;
  virtual StmtView& bind(std::int64_t index, std::string_view value) = 0;
  virtual StmtView& bind_null(std::int64_t index) = 0;

  // 有结果行返回 true；语句结束返回 false；出错抛 AppError。
  virtual bool step() = 0;
  // 执行不期望结果集的语句；返回了行则抛 db_unexpected_row。
  virtual void run() = 0;

  virtual std::int64_t i64(int column) const = 0;
  virtual std::string text(int column) const = 0;
  virtual bool is_null(int column) const = 0;

  // 最近一次 INSERT 生成的自增主键（SQLite: last_insert_rowid；PG: RETURNING）。
  virtual std::int64_t last_insert_id() const = 0;
};

// 连接执行面。事务边界由 Db::write()/read() 管理，这里不暴露 begin/commit。
class Conn {
 public:
  virtual ~Conn() = default;

  virtual std::unique_ptr<StmtView> prepare(std::string_view sql) = 0;
  // 直接执行 SQL（多语句以分号分隔，仅用于 DDL/迁移等无绑定场景）。
  virtual void exec(std::string_view sql) = 0;
  // 表是否存在（SQLite: sqlite_master；PG: information_schema）。
  virtual bool table_exists(std::string_view name) = 0;
  // 最近一次写入语句影响的行数（乐观锁判定用；SQLite: sqlite3_changes；PG: PQcmdTuples）。
  virtual int changes() const = 0;
};

// 薄包装：保持 DAO 既有的 `Stmt stmt(conn, sql); stmt.bind(...)` 使用模式
// 不因接口化而改写每一处语句点（阶段 1 决策，见改造技术方案 D8 修订）。
class Stmt {
 public:
  Stmt(Conn& conn, std::string_view sql);
  ~Stmt();

  Stmt(const Stmt&) = delete;
  Stmt& operator=(const Stmt&) = delete;

  Stmt& bind(std::int64_t index, std::int64_t value);
  Stmt& bind(std::int64_t index, std::string_view value);
  Stmt& bind_null(std::int64_t index);

  bool step();
  void run();

  std::int64_t i64(int column) const;
  std::string text(int column) const;
  bool is_null(int column) const;
  std::int64_t last_insert_id() const;

 private:
  std::unique_ptr<StmtView> view_;
};

class BackupDao;

// 连接池与事务管理：SQLite 实现为单写连接 + 小读池；
// 其他后端按各自并发模型实现同一语义（写事务串行、读事务快照内执行）。
class Db {
 public:
  virtual ~Db() = default;

  // 写事务边界：同一个 fn 里的所有语句同属一次提交；fn 抛异常即整体 ROLLBACK，
  // 出事务前不会留下半成品（例如数据已改但审计没写）。
  // 争用等待上限见后端实现；超时抛 locked，由 HTTP 层映射成 423。
  virtual void write(const std::function<void(Conn&)>& fn) = 0;
  // 不带事务包裹的写连接执行（VACUUM/ATTACH 这类不能进事务的语句用）。
  virtual void write_raw(const std::function<void(Conn&)>& fn) = 0;
  // 只读连接池中的快照读。
  virtual void read(const std::function<void(Conn&)>& fn) const = 0;

  // 把 WAL 中已提交的内容尽量写回主库（PASSIVE，不阻塞读写）。
  // 库内心跳依赖它：两条路径 hardlink 到同一个文件时，只有主库里的行可见。
  // 非 SQLite 后端为空实现。
  virtual void checkpoint() = 0;

  virtual std::int64_t schema_version() const = 0;
  // 在调用方事务内更新 schema 版本（SQLite: PRAGMA user_version；PG: meta 表）。
  virtual void set_schema_version(Conn& conn, int version) = 0;

  virtual std::string backend_name() const = 0;
  // 供日志/展示的后端描述（阶段 1 与现状「数据库: <路径>」一致）。
  virtual std::string describe() const = 0;

  // 备份/恢复的后端实现入口（抽象接口见 dao/backup_dao.h）。
  virtual const BackupDao& backup() const = 0;
};

// 兼容旧调用点：等价于 conn.exec(sql)。
void exec_sql(Conn& conn, std::string_view sql);

// 本地时间串 "%Y-%m-%d %H:%M:%S"（纯 C++，与后端无关）。
std::string now_text();

}  // namespace devcard
