#include "routes.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "devcard/audit.h"
#include "devcard/backup.h"
#include "devcard/db.h"
#include "devcard/error.h"
#include "devcard/image_meta.h"
#include "devcard/paths.h"
#include "devcard/settings.h"
#include "heartbeat.h"
#include "http_json.h"
#include "logo_store.h"
#include "sessions.h"

namespace devcard::detail {

void register_system_routes(ServerState& state) {
  using nlohmann::json;

  state.server.Put("/api/system/company",
                   [&state](const httplib::Request& request,
                            httplib::Response& response) {
                     require_admin(state, request);
                     const json body = json::parse(request.body, nullptr, false);
                     if (body.is_discarded() || !body.is_object()) {
                       throw AppError("bad_json", "请求体必须是 JSON 对象");
                     }
                     const std::string value =
                         trim_copy(body.value("company_name", std::string{}));
                     if (utf8_length(value) > kCompanyNameInputMaxLength) {
                       throw AppError(
                           "bad_company_name",
                           "公司名称最多 16 个字符（卡面按 13 字宽打印）");
                     }
                     state.db->write([&](Conn& conn) {
                       set_setting(conn, "print_company_name", value);
                     });
                     set_json(response,
                              json{{"company_name", company_name(*state.db)},
                                   {"company_name_print",
                                    company_print_name(*state.db)},
                                   {"company_name_width",
                                    company_name_width(company_name(*state.db))},
                                   {"company_name_max_width", kCompanyNameMaxWidth},
                                   {"is_default",
                                    get_setting(*state.db, "print_company_name")
                                        .empty()}});
                   });
  state.server.Post("/api/system/company-logo",
                    [&state](const httplib::Request& request,
                             httplib::Response& response) {
                      require_admin(state, request);
                      constexpr std::size_t kMaxBytes = 2 * 1024 * 1024;
                      if (request.body.empty()) {
                        throw AppError("bad_logo", "请选择要上传的图片");
                      }
                      if (request.body.size() > kMaxBytes) {
                        throw AppError("logo_too_large", "图标不能超过 2 MB");
                      }
                      const ImageInfo info = inspect_image(request.body);
                      if (info.format == ImageFormat::kUnknown) {
                        throw AppError("bad_logo",
                                       "只接受 PNG 或 JPEG 图片（不支持 SVG）");
                      }
                      const double ratio =
                          static_cast<double>(info.width) / info.height;
                      const bool aspect_warning =
                          ratio < kLogoMinAspect || ratio > kLogoMaxAspect;

                      const bool png = info.format == ImageFormat::kPng;
                      const std::string relative =
                          std::string("assets/company-logo.") + (png ? "png" : "jpg");
                      std::error_code ec;
                      std::filesystem::create_directories(state.data_dir / "assets",
                                                          ec);
                      {
                        std::ofstream out(state.data_dir / relative,
                                          std::ios::binary | std::ios::trunc);
                        if (!out) {
                          throw AppError("logo_write",
                                         "图标写入失败，请检查数据目录权限");
                        }
                        out.write(request.body.data(),
                                  static_cast<std::streamsize>(request.body.size()));
                      }
                      // 换格式时清掉旧文件，避免两份并存。
                      std::filesystem::remove(
                          state.data_dir /
                              (std::string("assets/company-logo.") +
                               (png ? "jpg" : "png")),
                          ec);
                      state.db->write([&](Conn& conn) {
                        set_setting(conn, kLogoSettingKey, relative);
                        set_setting(conn, "company_logo_updated_at", now_text());
                      });

                      const bool low_resolution =
                          info.width < kLogoMinPixels ||
                          info.height < kLogoMinPixels;
                      std::string message = "图标已更新";
                      if (low_resolution) {
                        message += "；分辨率低于 104×104 像素，300 dpi 打印可能发虚";
                      }
                      if (aspect_warning) {
                        message +=
                            "；图片偏扁或偏窄，缩放后会显得很小，建议换成接近方形的图";
                      }
                      set_json(response,
                               json{{"logo_url", "/company-logo"},
                                    {"format", png ? "png" : "jpeg"},
                                    {"width", info.width},
                                    {"height", info.height},
                                    {"low_resolution", low_resolution},
                                    {"aspect_warning", aspect_warning},
                                    {"updated_at",
                                     get_setting(*state.db,
                                                 "company_logo_updated_at")},
                                    {"message", message}});
                    });
  state.server.Delete("/api/system/company-logo",
                      [&state](const httplib::Request& request,
                               httplib::Response& response) {
                        require_admin(state, request);
                        state.db->write([&](Conn& conn) {
                          set_setting(conn, kLogoSettingKey, "");
                          set_setting(conn, "company_logo_updated_at", "");
                        });
                        std::error_code ec;
                        std::filesystem::remove(
                            state.data_dir / "assets/company-logo.png", ec);
                        std::filesystem::remove(
                            state.data_dir / "assets/company-logo.jpg", ec);
                        set_json(response, json{{"logo_url", "/company-logo"},
                                                {"is_default", true},
                                                {"updated_at", ""}});
                      });
  state.server.Get("/api/system/backups",
                   [&state](const httplib::Request& request,
                            httplib::Response& response) {
                     require_admin(state, request);
                     json items = json::array();
                     for (const BackupEntry& entry : list_backups(state.data_dir)) {
                       items.push_back(json{{"name", entry.name},
                                            {"bytes", entry.bytes},
                                            {"created_at", entry.created_at}});
                     }
                     set_json(response, json{{"items", items}});
                   });
  state.server.Post("/api/system/backup",
                    [&state](const httplib::Request& request,
                             httplib::Response& response) {
                      require_admin(state, request);
                      const BackupEntry entry =
                          create_backup(*state.db, state.data_dir, "manual");
                      set_json(response, json{{"name", entry.name},
                                              {"bytes", entry.bytes},
                                              {"created_at", entry.created_at}}, 201);
                    });
  state.server.Post("/api/system/restore",
                    [&state](const httplib::Request& request,
                             httplib::Response& response) {
                      const User actor = require_admin(state, request);
                      const json body = parse_body(request.body);
                      if (body.value("confirm", "") != "RESTORE") {
                        throw AppError("bad_json", "恢复操作需要 confirm=RESTORE");
                      }
                      const std::string name = body.value("name", "");
                      restore_backup(*state.db, state.data_dir, name);
                      // 备份里带着导出时的心跳行，恢复后必须由当前实例重写，
                      // 否则定时刷新会因 instance_id 不匹配而停摆。
                      force_heartbeat(*state.db, state.instance_id);
                      {
                        std::lock_guard<std::mutex> lock(state.sessions_mutex);
                        state.sessions.clear();
                        persist_sessions(state);
                      }
                      AuditEntry entry;
                      entry.actor = actor.user_name;
                      entry.actor_ip = request.remote_addr;
                      entry.action = "restore";
                      entry.entity = "system";
                      entry.summary = "从备份恢复：" + name;
                      audit_event(state, entry);
                      clear_session_cookie(response);
                      set_json(response, json{{"ok", true}, {"reload", true}});
                    });
  state.server.Post("/api/system/open-data-dir",
                    [&state](const httplib::Request& request,
                             httplib::Response& response) {
                      require_admin(state, request);
#ifdef _WIN32
                      const std::wstring dir = state.data_dir.wstring();
                      const HINSTANCE result =
                          ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr,
                                        nullptr, SW_SHOWNORMAL);
                      if (reinterpret_cast<std::intptr_t>(result) <= 32) {
                        throw AppError("bad_request",
                                       "无法打开数据目录：" +
                                           path_to_utf8(state.data_dir));
                      }
                      set_json(response,
                               json{{"ok", true},
                                    {"path", path_to_utf8(state.data_dir)}});
#else
                      (void)response;
                      throw AppError("bad_request",
                                     "打开数据目录仅在 Windows 版本提供");
#endif
                    });
  state.server.Get("/api/audit", [&state](const httplib::Request& request,
                                          httplib::Response& response) {
    require_admin(state, request);
    const int limit = int_param(request, "limit", 200);
    json items = json::array();
    for (const AuditEntry& entry : list_audit(*state.db, limit)) {
      items.push_back({{"id", entry.id},
                       {"at", entry.at},
                       {"actor", entry.actor},
                       {"actor_ip", entry.actor_ip},
                       {"action", entry.action},
                       {"entity", entry.entity},
                       {"entity_id", entry.entity_id},
                       {"summary", entry.summary},
                       {"before", entry.before_json},
                       {"after", entry.after_json}});
    }
    set_json(response, json{{"items", items}});
  });
  state.server.Get("/company-logo",
                   [&state](const httplib::Request&, httplib::Response& response) {
                     const std::string relative =
                         get_setting(*state.db, kLogoSettingKey);
                     if (safe_logo_relative(relative)) {
                       const std::filesystem::path path = state.data_dir / relative;
                       std::string bytes = read_file_bytes(path);
                       if (!bytes.empty()) {
                         const bool png = path.extension() == ".png";
                         response.status = 200;
                         response.set_header("Content-Type",
                                             png ? "image/png" : "image/jpeg");
                         response.set_header("Cache-Control", "no-store");
                         response.body = std::move(bytes);
                         return;
                       }
                     }
                     response.status = 200;
                     response.set_header("Content-Type", "image/jpeg");
                     response.set_header("Cache-Control", "no-store");
                     response.body = logo_jpeg();
                   });

}

}  // namespace devcard::detail
