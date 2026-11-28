#include "routes.h"

#include <algorithm>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "devcard/error.h"
#include "devcard/service/device_service.h"
#include "devcard/settings.h"
#include "font_manifest.h"
#include "http_json.h"

namespace devcard::detail {

void register_print_routes(ServerState& state) {
  using nlohmann::json;

  state.server.Get("/api/fonts/coverage",
                   [&state](const httplib::Request&, httplib::Response& response) {
                     json ranges = json::array();
                     for (const auto& range : state.coverage.ranges()) {
                       ranges.push_back({range.first, range.second});
                     }
                     // 字体的字节数与 SHA256 一并下发：前端不再自己写死这两个数，
                     // 否则重新子集后忘了同步，导出会以「字节数不符」永久中止。
                     json fonts = json::array();
                     for (const auto& spec : kEmbeddedFonts) {
                       fonts.push_back({{"name", spec.name},
                                        {"url", spec.url},
                                        {"bytes", spec.bytes},
                                        {"sha256", spec.sha256}});
                     }
                     set_json(response,
                              json{{"available", !state.coverage.empty()},
                                   {"count", state.coverage.count()},
                                   {"ranges", ranges},
                                   {"fonts", fonts}});
                   });
  state.server.Get("/api/print/cards", [&state](const httplib::Request& request,
                            httplib::Response& response) {
                     DeviceQuery query = query_from(request);
                     query.page = 0;
                     query.page_size = 1000;
                     query.sort = "id";
                     query.desc = false;

                     std::vector<Device> items;
                     std::int64_t total = 0;
                     bool truncated = false;
                     // 打印卡片截断上限：随响应返回 print_limit，
                     // 前端文案直接引用该值，不再各自写死数字。
                     constexpr std::size_t kPrintCardLimit = 10000;
                     while (true) {
                       const DevicePage page = state.devices->list(query);
                       if (query.page == 0) total = page.total;
                       items.insert(items.end(), page.items.begin(), page.items.end());
                       if (page.items.empty() ||
                           static_cast<std::int64_t>(items.size()) >= total) {
                         break;
                       }
                       if (items.size() >= kPrintCardLimit) {
                         truncated = true;
                         break;
                       }
                       ++query.page;
                     }

                     const std::string ids_text = request.get_param_value("ids");
                     if (!ids_text.empty()) {
                       std::set<std::int64_t> wanted;
                       std::size_t start = 0;
                       while (start <= ids_text.size()) {
                         const std::size_t comma = ids_text.find(',', start);
                         const std::string part = ids_text.substr(
                             start, comma == std::string::npos
                                        ? std::string::npos
                                        : comma - start);
                         try {
                           if (!part.empty()) wanted.insert(std::stoll(part));
                         } catch (...) {
                           throw AppError("bad_json", "ids 必须是逗号分隔的数字");
                         }
                         if (comma == std::string::npos) break;
                         start = comma + 1;
                       }
                       items.erase(
                           std::remove_if(items.begin(), items.end(),
                                          [&](const Device& device) {
                                            return wanted.count(device.id) == 0;
                                          }),
                           items.end());
                       total = static_cast<std::int64_t>(items.size());
                     }

                     json array = json::array();
                     for (const Device& device : items) {
                       array.push_back(device_json(device));
                     }
                     set_json(response, json{{"items", array},
                                             {"total", total},
                                             {"truncated", truncated},
                                             {"print_limit", kPrintCardLimit},
                                             {"company_name", company_print_name(*state.db)}});
                   });

}

}  // namespace devcard::detail
