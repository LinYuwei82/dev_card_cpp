#include "devcard/xlsx.h"

#include <algorithm>
#include <cstdio>
#include <string_view>

#include "devcard/error.h"
#include "miniz.h"

namespace devcard::xlsx {
namespace {

std::string unescape(std::string_view in) {
  std::string out;
  out.reserve(in.size());
  for (std::size_t i = 0; i < in.size(); ++i) {
    if (in[i] != '&') {
      out.push_back(in[i]);
      continue;
    }
    const std::size_t semi = in.find(';', i);
    if (semi == std::string_view::npos || semi - i > 10) {
      out.push_back(in[i]);
      continue;
    }
    const std::string_view entity = in.substr(i + 1, semi - i - 1);
    if (entity == "amp") {
      out.push_back('&');
    } else if (entity == "lt") {
      out.push_back('<');
    } else if (entity == "gt") {
      out.push_back('>');
    } else if (entity == "quot") {
      out.push_back('"');
    } else if (entity == "apos") {
      out.push_back('\'');
    } else {
      out.append(in.substr(i, semi - i + 1));
    }
    i = semi;
  }
  return out;
}

std::string attr(std::string_view tag, std::string_view name) {
  const std::string key = " " + std::string(name) + "=\"";
  const std::size_t pos = tag.find(key);
  if (pos == std::string_view::npos) return {};
  const std::size_t start = pos + key.size();
  const std::size_t end = tag.find('"', start);
  if (end == std::string_view::npos) return {};
  return std::string(tag.substr(start, end - start));
}

// 取 <t ...> 与 </t> 之间的文本（可多次出现，拼起来）。
std::string collect_t(std::string_view xml) {
  std::string out;
  std::size_t pos = 0;
  while (true) {
    const std::size_t open = xml.find("<t", pos);
    if (open == std::string_view::npos) break;
    const std::size_t open_end = xml.find('>', open);
    if (open_end == std::string_view::npos) break;
    const std::size_t close = xml.find("</t>", open_end);
    if (close == std::string_view::npos) break;
    out += unescape(xml.substr(open_end + 1, close - open_end - 1));
    pos = close + 4;
  }
  return out;
}

int column_index(std::string_view ref) {
  int index = 0;
  for (char c : ref) {
    if (c >= 'A' && c <= 'Z') {
      index = index * 26 + (c - 'A' + 1);
    } else if (c >= 'a' && c <= 'z') {
      index = index * 26 + (c - 'a' + 1);
    } else {
      break;
    }
  }
  return index > 0 ? index - 1 : -1;
}

std::string column_name(int index) {
  std::string out;
  int n = index + 1;
  while (n > 0) {
    const int rem = (n - 1) % 26;
    out.insert(out.begin(), static_cast<char>('A' + rem));
    n = (n - 1) / 26;
  }
  return out;
}

std::string entry_text(mz_zip_archive& zip, const std::string& name) {
  mz_uint32 index = 0;
  if (!mz_zip_reader_locate_file_v2(&zip, name.c_str(), nullptr, 0, &index)) return {};
  mz_zip_archive_file_stat stat;
  if (!mz_zip_reader_file_stat(&zip, index, &stat)) return {};
  std::string out(stat.m_uncomp_size, '\0');
  if (!out.empty() &&
      !mz_zip_reader_extract_to_mem(&zip, index, out.data(), out.size(), 0)) {
    return {};
  }
  return out;
}

void add_entry(mz_zip_archive& zip, const char* name, const std::string& body) {
  if (!mz_zip_writer_add_mem(&zip, name, body.data(), body.size(),
                             MZ_DEFAULT_COMPRESSION)) {
    throw AppError("xlsx_write_failed", "生成 xlsx 失败：写入 zip 条目出错");
  }
}

std::string xml_escape(std::string_view in) {
  std::string out;
  out.reserve(in.size() + 8);
  for (char ch : in) {
    switch (ch) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      case '\'': out += "&apos;"; break;
      default: out.push_back(ch);
    }
  }
  return out;
}

}  // namespace

Rows read(const std::string& bytes) {
  if (bytes.empty()) throw AppError("bad_xlsx", "导入文件为空");

  mz_zip_archive zip{};
  if (!mz_zip_reader_init_mem(&zip, bytes.data(), bytes.size(), 0)) {
    throw AppError("bad_xlsx", "无法读取该文件：不是有效的 xlsx（zip 结构不完整）");
  }
  struct Guard {
    mz_zip_archive* zip;
    ~Guard() { mz_zip_reader_end(zip); }
  } guard{&zip};

  std::vector<std::string> shared;
  const std::string shared_xml = entry_text(zip, "xl/sharedStrings.xml");
  if (!shared_xml.empty()) {
    std::size_t pos = 0;
    while (true) {
      const std::size_t open = shared_xml.find("<si", pos);
      if (open == std::string::npos) break;
      const std::size_t close = shared_xml.find("</si>", open);
      if (close == std::string::npos) break;
      shared.push_back(collect_t(std::string_view(shared_xml).substr(
          open, close - open + 5)));
      pos = close + 5;
    }
  }

  std::string sheet_name = "xl/worksheets/sheet1.xml";
  if (entry_text(zip, sheet_name).empty()) {
    const mz_uint count = mz_zip_reader_get_num_files(&zip);
    sheet_name.clear();
    for (mz_uint i = 0; i < count; ++i) {
      char buf[512] = {0};
      mz_zip_reader_get_filename(&zip, i, buf, sizeof(buf));
      const std::string name(buf);
      if (name.rfind("xl/worksheets/sheet", 0) == 0 &&
          name.size() > 4 && name.compare(name.size() - 4, 4, ".xml") == 0) {
        sheet_name = name;
        break;
      }
    }
  }
  const std::string sheet_xml = sheet_name.empty() ? std::string() : entry_text(zip, sheet_name);
  if (sheet_xml.empty()) {
    throw AppError("bad_xlsx", "xlsx 里找不到工作表（xl/worksheets/sheet*.xml）");
  }

  Rows rows;
  std::size_t pos = 0;
  while (true) {
    const std::size_t row_open = sheet_xml.find("<row", pos);
    if (row_open == std::string::npos) break;
    const std::size_t row_tag_end = sheet_xml.find('>', row_open);
    if (row_tag_end == std::string::npos) break;
    const std::size_t row_close = sheet_xml.find("</row>", row_tag_end);
    if (row_close == std::string::npos) break;
    const std::string_view row_body(sheet_xml.data() + row_tag_end + 1,
                                    row_close - row_tag_end - 1);

    std::vector<std::string> cells;
    std::size_t cpos = 0;
    while (true) {
      const std::size_t c_open = row_body.find("<c", cpos);
      if (c_open == std::string_view::npos) break;
      const std::size_t c_tag_end = row_body.find('>', c_open);
      if (c_tag_end == std::string_view::npos) break;
      if (row_body[c_tag_end - 1] == '/') {
        cpos = c_tag_end + 1;
        continue;
      }
      const std::size_t c_close = row_body.find("</c>", c_tag_end);
      if (c_close == std::string_view::npos) break;
      const std::string_view tag = row_body.substr(c_open, c_tag_end - c_open);
      const std::string_view inner = row_body.substr(c_tag_end + 1, c_close - c_tag_end - 1);

      const int col = column_index(attr(tag, "r"));
      const std::string type = attr(tag, "t");
      std::string value;
      if (type == "s") {
        const std::size_t v_open = inner.find("<v>");
        if (v_open != std::string_view::npos) {
          const std::size_t v_close = inner.find("</v>", v_open);
          if (v_close != std::string_view::npos) {
            const std::string idx(inner.substr(v_open + 3, v_close - v_open - 3));
            try {
              const std::size_t n = static_cast<std::size_t>(std::stoul(idx));
              if (n < shared.size()) value = shared[n];
            } catch (...) {
              value.clear();
            }
          }
        }
      } else if (type == "inlineStr") {
        value = collect_t(inner);
      } else {
        const std::size_t v_open = inner.find("<v>");
        if (v_open != std::string_view::npos) {
          const std::size_t v_close = inner.find("</v>", v_open);
          if (v_close != std::string_view::npos) {
            value = unescape(inner.substr(v_open + 3, v_close - v_open - 3));
          }
        }
      }

      if (col >= 0 && col < 64) {
        if (cells.size() <= static_cast<std::size_t>(col)) {
          cells.resize(static_cast<std::size_t>(col) + 1);
        }
        cells[static_cast<std::size_t>(col)] = value;
      }
      cpos = c_close + 4;
    }
    rows.push_back(std::move(cells));
    pos = row_close + 6;
  }
  return rows;
}

std::string write(const Rows& rows) {
  return write_workbook({Sheet{"设备台账", rows}});
}

std::string write_workbook(const std::vector<Sheet>& sheets) {
  if (sheets.empty()) {
    throw AppError("xlsx_write_failed", "生成 xlsx 失败：至少需要一个工作表");
  }

  const std::string header =
      "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n";

  std::string content_types = header +
      "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
      "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>"
      "<Default Extension=\"xml\" ContentType=\"application/xml\"/>"
      "<Override PartName=\"/xl/workbook.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml\"/>";

  std::string workbook = header +
      "<workbook xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\" "
      "xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\">"
      "<sheets>";

  std::string workbook_rels = header +
      "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">";

  std::vector<std::string> sheet_files;
  sheet_files.reserve(sheets.size());

  for (std::size_t i = 0; i < sheets.size(); ++i) {
    const Sheet& sheet_info = sheets[i];
    if (sheet_info.name.empty()) {
      throw AppError("xlsx_write_failed", "生成 xlsx 失败：工作表名称不能为空");
    }
    const std::string id = std::to_string(i + 1);

    std::string sheet = header +
        "<worksheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\">"
        "<sheetData>";
    for (std::size_t r = 0; r < sheet_info.rows.size(); ++r) {
      sheet += "<row r=\"" + std::to_string(r + 1) + "\">";
      for (std::size_t c = 0; c < sheet_info.rows[r].size(); ++c) {
        sheet += "<c r=\"" + column_name(static_cast<int>(c)) +
                 std::to_string(r + 1) +
                 "\" t=\"inlineStr\"><is><t xml:space=\"preserve\">" +
                 xml_escape(sheet_info.rows[r][c]) + "</t></is></c>";
      }
      sheet += "</row>";
    }
    sheet += "</sheetData></worksheet>";
    sheet_files.push_back(std::move(sheet));

    content_types += "<Override PartName=\"/xl/worksheets/sheet" + id +
                     ".xml\" ContentType=\"application/vnd.openxmlformats-"
                     "officedocument.spreadsheetml.worksheet+xml\"/>";
    workbook += "<sheet name=\"" + xml_escape(sheet_info.name) + "\" sheetId=\"" +
                id + "\" r:id=\"rId" + id + "\"/>";
    workbook_rels += "<Relationship Id=\"rId" + id +
                     "\" Type=\"http://schemas.openxmlformats.org/officeDocument/"
                     "2006/relationships/worksheet\" Target=\"worksheets/sheet" +
                     id + ".xml\"/>";
  }

  content_types +=
      "<Override PartName=\"/xl/styles.xml\" ContentType=\"application/vnd.openxmlformats-"
      "officedocument.spreadsheetml.styles+xml\"/>"
      "</Types>";
  workbook += "</sheets></workbook>";
  workbook_rels += "<Relationship Id=\"rId" + std::to_string(sheets.size() + 1) +
                   "\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/"
                   "relationships/styles\" Target=\"styles.xml\"/>"
                   "</Relationships>";

  const std::string rels = header +
      "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
      "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument\" Target=\"xl/workbook.xml\"/>"
      "</Relationships>";

  const std::string styles = header +
      "<styleSheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\">"
      "<fonts count=\"1\"><font><sz val=\"11\"/><name val=\"Calibri\"/></font></fonts>"
      "<fills count=\"1\"><fill><patternFill patternType=\"none\"/></fill></fills>"
      "<borders count=\"1\"><border/></borders>"
      "<cellStyleXfs count=\"1\"><xf numFmtId=\"0\" fontId=\"0\" fillId=\"0\" borderId=\"0\"/></cellStyleXfs>"
      "<cellXfs count=\"1\"><xf numFmtId=\"0\" fontId=\"0\" fillId=\"0\" borderId=\"0\" xfId=\"0\"/></cellXfs>"
      "</styleSheet>";

  mz_zip_archive zip{};
  if (!mz_zip_writer_init_heap(&zip, 0, 0)) {
    throw AppError("xlsx_write_failed", "生成 xlsx 失败：无法创建 zip");
  }
  void* buf = nullptr;
  std::size_t size = 0;
  try {
    add_entry(zip, "[Content_Types].xml", content_types);
    add_entry(zip, "_rels/.rels", rels);
    add_entry(zip, "xl/workbook.xml", workbook);
    add_entry(zip, "xl/_rels/workbook.xml.rels", workbook_rels);
    add_entry(zip, "xl/styles.xml", styles);
    for (std::size_t i = 0; i < sheet_files.size(); ++i) {
      add_entry(zip, ("xl/worksheets/sheet" + std::to_string(i + 1) + ".xml").c_str(),
                sheet_files[i]);
    }
    if (!mz_zip_writer_finalize_heap_archive(&zip, &buf, &size)) {
      throw AppError("xlsx_write_failed", "生成 xlsx 失败：收尾失败");
    }
  } catch (...) {
    mz_zip_writer_end(&zip);
    throw;
  }
  mz_zip_writer_end(&zip);
  std::string out(static_cast<const char*>(buf), size);
  mz_free(buf);
  return out;
}

}  // namespace devcard::xlsx
