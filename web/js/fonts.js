import { api, appState, systemState } from './core.js';

  // ---------- 打印设置：公司名与图标 ----------
  // 显示宽度：汉字算 1，ASCII 字母/数字/符号算 0.5。
  // 字体覆盖检查：区间来自服务端对随包字体 cmap 的解析结果。
  // 用途是「知情」而不是拦截——名字是真实的，拒绝保存只会逼用户去删字。
  function loadFontCoverage() {
    return api('GET', '/api/fonts/coverage').then(function (data) {
      appState.fontRanges = (data && data.available && data.ranges) ? data.ranges : null;
      // 字体的字节数与 SHA256 由构建期从 web/fonts/ 读出、随接口下发，前端不再写死。
      printFontSpec = (data && data.fonts && data.fonts.length) ? data.fonts : null;
    }).catch(function () {
      appState.fontRanges = null;
      printFontSpec = null;
    });
  }

  function fontCovers(codepoint) {
    var ranges = appState.fontRanges;
    if (!ranges) return true;  // 清单取不到时不误报
    var low = 0;
    var high = ranges.length;
    while (low < high) {
      var mid = (low + high) >> 1;
      if (codepoint < ranges[mid][0]) high = mid;
      else if (codepoint > ranges[mid][1]) low = mid + 1;
      else return true;
    }
    return false;
  }

  function uncoveredChars(text) {
    var missing = [];
    if (!text) return missing;
    for (var i = 0; i < text.length; i++) {
      var cp = text.codePointAt(i);
      if (cp > 0xFFFF) i++;
      if (cp < 0x20) continue;
      if (!fontCovers(cp) && missing.indexOf(String.fromCodePoint(cp)) < 0) {
        missing.push(String.fromCodePoint(cp));
      }
    }
    return missing;
  }

  function companyNameWidth(value) {
    var width = 0;
    Array.from(value).forEach(function (ch) {
      width += ch.codePointAt(0) < 0x80 ? 0.5 : 1;
    });
    return width;
  }

  function truncateCompanyName(value) {
    var width = 0;
    var out = '';
    // 与服务端 truncate_company_name 同语义：第一次超宽就停，不跳过该字继续收后面的窄字，
    // 否则预览里会出现「中间少一个字、后面还被接上」的伪前缀，与实际打印不一致。
    var chars = Array.from(value);
    for (var i = 0; i < chars.length; i++) {
      var w = chars[i].codePointAt(0) < 0x80 ? 0.5 : 1;
      if (width + w > systemState.companyNameMaxWidth + 1e-9) break;
      width += w;
      out += chars[i];
    }
    return out;
  }

  // 字体口径（2026-11-14 统一）：
  //   整份字体缺失 → 影响不可枚举（不知道多少字、多少行会变），导出直接中止；
  //   单字不在子集 → 影响可枚举（就是列出来的这几个字），按真实字宽量：
  //     量下来仍能放进公司名框 → 允许并明确告知；
  //     量下来超出                 → 阻止导出，不能拿系统字体硬塞。
  var COMPANY_BOX_MM = 49.12;   // 公司名可用区域宽（§8.7.2）
  var COMPANY_SCALE_X = 0.88;   // styles.css 里 .company-name span 的横向压缩

  function measurePrintText(text) {
    // 用打印字体栈量：子集里没有的字由系统字体补，量到的就是回退后的真实宽度。
    if (!measurePrintText.canvas) {
      measurePrintText.canvas = document.createElement('canvas');
    }
    var ctx = measurePrintText.canvas.getContext('2d');
    ctx.font = '11pt "DevCard Sans", sans-serif';
    return ctx.measureText(text).width;
  }

  // 返回 { missing, overflow }：missing 是子集外的字，overflow 表示回退后超出公司名框。
  function companyFontCheck(text) {
    var missing = uncoveredChars(text || '');
    if (!missing.length) return { missing: missing, overflow: false };
    var boxPx = COMPANY_BOX_MM * 96 / 25.4;
    var widthPx = measurePrintText(text) * COMPANY_SCALE_X;
    return { missing: missing, overflow: widthPx > boxPx + 0.5 };
  }

  // 打印/导出前必须确认子集字体已就绪。字体走 /fonts/ HTTP 路由，
  // 未加载完就截图会静默退回系统字体：屏幕上「差不多」，但字宽变了，叠图会超 1 mm。
  // 这里既等待 document.fonts.ready，也用 check() 断言两个字重确实可用；
  // 失败时把错误交给调用方中止，绝不产出用系统字体「顶替」的 PDF。
  var printFonts = { ready: false, error: '' };

  // 期望的字体文件、字节数与 SHA256 来自构建期（/api/fonts/coverage 的 fonts 字段），
  // 不在 JS 里写死：重新子集后忘了同步，导出会以「字节数不符」永久中止，而真正的原因是清单过期。
  // 判据用 SHA256：重子集后大小碰巧相同、内容变了，只比大小会判「通过」再拿错字体排版。
  var printFontSpec = null;

export { COMPANY_BOX_MM, COMPANY_SCALE_X, companyFontCheck, companyNameWidth, fontCovers, loadFontCoverage, measurePrintText, printFontSpec, printFonts, truncateCompanyName, uncoveredChars };
