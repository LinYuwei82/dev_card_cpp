#!/usr/bin/env python3
"""比对两份 PDF 的文本层，先把文本归一，再逐页逐行比。

为什么要归一，而不直接比字符串：
  - 同字不同码位。PingFang SC 的 ToUnicode 把「水」「风」「生」写成了康熙部首
    （U+2F54 / U+2EDB / U+2F63），新字体写的是统一汉字。直接比码位会得出
    「文本变了」的错误结论；而部首与正字形近（⻛ / 风），肉眼又看不出来。
  - 空格。旧样张有 U+3000（表意空格），新样张是 U+2003（EM SPACE）。

归一顺序固定，不能反：
  1) NFKC：折回康熙部首（U+2F00–U+2FDF）、全角转半角、U+3000/U+2003 转空格；
  2) 查 RADICALS 表：CJK 部首补充区（U+2E80–U+2EFF）NFKC 折不回，只能查表。

用法：python3 tools/print/compare_pdf_text.py 基线.pdf 待比.pdf
退出码：0 文本层一致；1 不一致（打印首个差异与码位）；2 用法/依赖错误。
"""

import subprocess
import sys
import unicodedata

# 取自 Unicode CJKRadicals-18.0.0.txt：U+2E80–U+2EFF 中有对应统一汉字的全部码位。
# 这一区 NFKC 不折，必须显式映射；未列出的码位保持原样（没有对应正字）。
RADICALS = {
    0x2EA6: 0x4E2C, 0x2EB0: 0x7E9F, 0x2EC5: 0x89C1, 0x2EC8: 0x8BA0,
    0x2EC9: 0x8D1D, 0x2ECB: 0x8F66, 0x2ED0: 0x9485, 0x2ED3: 0x957F,
    0x2ED4: 0x95E8, 0x2ED9: 0x97E6, 0x2EDA: 0x9875, 0x2EDB: 0x98CE,
    0x2EDC: 0x98DE, 0x2EE0: 0x9963, 0x2EE2: 0x9A6C, 0x2EE5: 0x9C7C,
    0x2EE6: 0x9E1F, 0x2EE7: 0x5364, 0x2EE8: 0x9EA6, 0x2EE9: 0x9EC4,
    0x2EEA: 0x9EFE, 0x2EEB: 0x6589, 0x2EEC: 0x9F50, 0x2EED: 0x6B6F,
    0x2EEE: 0x9F7F, 0x2EEF: 0x7ADC, 0x2EF0: 0x9F99, 0x2EF2: 0x4E80,
    0x2EF3: 0x9F9F,
}


def normalize(text):
    text = unicodedata.normalize("NFKC", text)
    return "".join(chr(RADICALS.get(ord(ch), ord(ch))) for ch in text)


def pages(pdf):
    """用 poppler 的 pdftotext 抽文本，按分页符切成每页的行。"""
    try:
        out = subprocess.run(
            ["pdftotext", "-enc", "UTF-8", pdf, "-"],
            capture_output=True, check=True,
        ).stdout.decode("utf-8")
    except FileNotFoundError:
        sys.exit("找不到 pdftotext，请先安装 poppler")
    except subprocess.CalledProcessError as exc:
        sys.exit("pdftotext 失败：%s" % exc.stderr.decode("utf-8", "replace"))
    return [[normalize(line) for line in page.splitlines()]
            for page in out.split("\f") if page.strip()]


def code_points(text):
    return " ".join("U+%04X" % ord(ch) for ch in text)


def main():
    if len(sys.argv) != 3:
        sys.exit("用法：python3 tools/print/compare_pdf_text.py 基线.pdf 待比.pdf")
    left, right = pages(sys.argv[1]), pages(sys.argv[2])
    problems = []
    if len(left) != len(right):
        problems.append("页数不同：%d vs %d" % (len(left), len(right)))
    for index, (a_page, b_page) in enumerate(zip(left, right), start=1):
        if len(a_page) != len(b_page):
            problems.append("第 %d 页行数不同：%d vs %d"
                            % (index, len(a_page), len(b_page)))
        for number, (a, b) in enumerate(zip(a_page, b_page), start=1):
            if a == b:
                continue
            problems.append("第 %d 页第 %d 行不同：\n  基线 %r\n  待比 %r\n  基线码位 %s\n  待比码位 %s"
                            % (index, number, a, b, code_points(a), code_points(b)))
            if len(problems) >= 10:
                break
    total = sum(len(page) for page in left)
    if problems:
        print("\n".join(problems))
        return 1
    print("文本层一致：%d 页、%d 行（已按 NFKC + U+2E80–U+2EFF 映射表归一）" % (len(left), total))
    return 0


if __name__ == "__main__":
    sys.exit(main())
