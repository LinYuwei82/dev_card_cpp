#include "routes.h"

#include <cstdint>
#include <string>
#include <vector>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "devcard/error.h"
#include "devcard/models.h"
#include "devcard/service/device_service.h"
#include "devcard/xlsx.h"
#include "http_json.h"
#include "sessions.h"

namespace devcard::detail {

void register_device_routes(ServerState& state) {
  using nlohmann::json;

  state.server.Get("/api/devices", [&state](const httplib::Request& request,
                                            httplib::Response& response) {
    const DevicePage page = state.devices->list(query_from(request));
    json items = json::array();
    for (const Device& device : page.items) items.push_back(device_json(device));
    set_json(response, json{{"items", items},
                            {"total", page.total},
                            {"total_all", page.total_all},
                            {"page", page.page},
                            {"page_size", page.page_size}});
  });
  state.server.Get("/api/devices/options",
                   [&state](const httplib::Request&, httplib::Response& response) {
                     const DeviceOptions values = state.devices->options();
                     set_json(response, json{{"names", values.names},
                                             {"locations", values.locations}});
                   });
  state.server.Post("/api/devices/import",
                    [&state](const httplib::Request& request,
                             httplib::Response& response) {
                      const auto rows = xlsx::read(request.body);
                      const ImportReport report = state.devices->import_rows(
                          rows, audit_context(state, request));
                      set_json(response, json{{"imported", report.imported},
                                              {"skipped_empty", report.skipped_empty},
                                              {"skipped_invalid", report.skipped_invalid},
                                              {"invalid_rows", report.invalid_rows},
                                              {"duplicates", report.duplicates},
                                              {"truncated", report.truncated}});
                    });
  state.server.Get("/api/devices/template",
                   [](const httplib::Request&, httplib::Response& response) {
                     xlsx::Sheet data;
                     data.name = "设备台账";
                     data.rows.push_back(DeviceService::expected_header());

                     xlsx::Sheet help;
                     help.name = "填写说明";
                     help.rows = {
                         {"列名", "含义与要求"},
                         {"设备名称", "必填；导入时超过 10 个字符会被截断"},
                         {"位置", "必填；导入时超过 10 个字符会被截断"},
                         {"控制范围", "必填；导入时超过 10 个字符会被截断"},
                         {"维护电话", "必填；仅数字、空格、短横线；导入时超过 11 个字符会被截断"},
                         {},
                         {"示例（假号码，请勿直接使用）",
                          "13800000001（卡片上显示为 138 0000 0001）"},
                         {},
                         {"说明",
                          "第一个工作表只有表头，可直接上传，导入 0 行；四列全空的行会跳过，"
                          "重复记录照常导入并提示。"},
                     };

                     response.status = 200;
                     response.set_header(
                         "Content-Type",
                         "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet");
                     response.set_header(
                         "Content-Disposition",
                         "attachment; filename=\"devices-template.xlsx\"; "
                         "filename*=UTF-8''%E8%AE%BE%E5%A4%87%E5%AF%BC%E5%85%A5"
                         "%E6%A8%A1%E6%9D%BF.xlsx");
                     response.set_header("Cache-Control", "no-store");
                     response.body = xlsx::write_workbook({data, help});
                   });
  state.server.Get("/api/devices/export",
                   [&state](const httplib::Request&, httplib::Response& response) {
                     const std::string body =
                         xlsx::write(state.devices->export_rows());
                     response.status = 200;
                     response.set_header(
                         "Content-Type",
                         "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet");
                     response.set_header(
                         "Content-Disposition",
                         "attachment; filename=\"devices.xlsx\"; "
                         "filename*=UTF-8''%E8%AE%BE%E5%A4%87%E5%8F%B0%E8%B4%A6.xlsx");
                     response.set_header("Cache-Control", "no-store");
                     response.body = body;
                   });
  state.server.Get(R"(/api/devices/(\d+))",
                   [&state](const httplib::Request& request,
                            httplib::Response& response) {
                     const auto id = parse_path_id(request.matches[1].str());
                     set_json(response, device_json(state.devices->get(id)));
                   });
  state.server.Post("/api/devices", [&state](const httplib::Request& request,
                                             httplib::Response& response) {
    Device device = device_from_json(parse_body(request.body));
    set_json(response,
             device_json(state.devices->create(std::move(device),
                                               audit_context(state, request))),
             201);
  });
  state.server.Put(R"(/api/devices/(\d+))",
                   [&state](const httplib::Request& request,
                            httplib::Response& response) {
                     const auto id = parse_path_id(request.matches[1].str());
                     Device device = device_from_json(parse_body(request.body));
                     try {
                       set_json(response,
                                device_json(state.devices->update(
                                    id, std::move(device),
                                    audit_context(state, request))));
                     } catch (const ConflictError& error) {
                       set_json(response, conflict_json(error), 409);
                     }
                   });
  state.server.Post(R"(/api/devices/(\d+)/force)",
                    [&state](const httplib::Request& request,
                             httplib::Response& response) {
                      const User actor = require_admin(state, request);
                      const auto id = parse_path_id(request.matches[1].str());
                      Device device = device_from_json(parse_body(request.body));
                      const AuditContext context{actor.user_name,
                                                 request.remote_addr};
                      set_json(response,
                               device_json(state.devices->force_update(
                                   id, std::move(device), context)));
                    });
  state.server.Delete(R"(/api/devices/(\d+))",
                      [&state](const httplib::Request& request,
                               httplib::Response& response) {
                        const std::string version_text =
                            request.get_param_value("version");
                        if (version_text.empty()) {
                          throw AppError(
                              "bad_version",
                              "删除需要带 ?version=N：请先刷新列表，"
                              "避免用过期页面删掉别人刚改过的记录");
                        }
                        std::int64_t version = 0;
                        try {
                          version = std::stoll(version_text);
                        } catch (...) {
                          throw AppError("bad_version",
                                         "version 必须是整数：" + version_text);
                        }
                        try {
                          state.devices->remove(
                              parse_path_id(request.matches[1].str()), version,
                              audit_context(state, request));
                          set_json(response, json{{"ok", true}});
                        } catch (const ConflictError& error) {
                          set_json(response, conflict_json(error), 409);
                        }
                      });
  state.server.Post("/api/devices/dedupe",
                    [&state](const httplib::Request& request,
                             httplib::Response& response) {
                      const User actor = require_admin(state, request);
                      const json body = parse_body(request.body);
                      if (body.value("confirm", "") != "MERGE_DUPLICATES") {
                        throw AppError(
                            "bad_request",
                            "合并重复需要确认：请求体带 "
                            "{\"confirm\":\"MERGE_DUPLICATES\"}");
                      }
                      const DedupeReport report = state.devices->dedupe(
                          AuditContext{actor.user_name, request.remote_addr});
                      set_json(response, json{{"removed", report.removed},
                                              {"remaining", report.remaining}});
                    });

}

}  // namespace devcard::detail
