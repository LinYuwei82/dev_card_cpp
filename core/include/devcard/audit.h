#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "devcard/db.h"

namespace devcard {

// 一条审计记录：谁（actor/actor_ip）、何时（at）、对哪条记录（entity/entity_id）
// 做了什么（action/summary），前后值见 before_json/after_json。
struct AuditEntry {
  std::int64_t id = 0;
  std::string at;
  std::string actor;
  std::string actor_ip;
  std::string action;
  std::string entity;
  std::string entity_id;
  std::string summary;
  std::string before_json;
  std::string after_json;
};

// 请求侧的审计上下文：操作者与其来源 IP。actor 为空表示不记审计
// （测试与本地导入等没有登录身份的场景）。
struct AuditContext {
  std::string actor;
  std::string actor_ip;

  bool enabled() const { return !actor.empty(); }
};

// 调用方必须已处于 Db::write() 事务内，与业务写入同一次提交。
void record_audit(sqlite3* conn, const AuditEntry& entry);

// 最近 limit 条（按 id 倒序），limit 限制在 1..1000。
std::vector<AuditEntry> list_audit(const Db& db, int limit);

// 审计时间用 UTC ISO-8601（毫秒），避免多客户端时区不同导致顺序错乱；
// 展示用的 updated_at 仍是本地时间秒级 now_text()。
std::string now_iso8601_utc();

// 生成 JSON 字符串字面量时转义，避免把换行/引号写成非法 JSON。
std::string json_escape(std::string_view text);

}  // namespace devcard
