import { api, devicesState, el, escapeHtml, handleAuthFailure, hasNativeBridge, printState, systemState, toast } from './core.js';
import { sendToShell } from './shell.js';
import { companyFontCheck, loadFontCoverage, printFontSpec, printFonts, uncoveredChars } from './fonts.js';

  // 与旧 PDF 一致：前 3 + 空格 + 中 4 + 空格 + 后 4。
  function formatPhone(value) {
    var phone = String(value == null ? '' : value);
    if (phone.length <= 3) return phone;
    if (phone.length <= 7) return phone.slice(0, 3) + ' ' + phone.slice(3);
    return phone.slice(0, 3) + ' ' + phone.slice(3, 7) + ' ' + phone.slice(7);
  }

  function fieldHtml(row, label, value) {
    return (
      '<div class="card-field r' + row + '">' +
      '<span class="card-label">' + label + '</span>' +
      '<span class="card-value">' + escapeHtml(value) + '</span>' +
      '</div>'
    );
  }

  function cardHtml(device) {
    return (
      '<article class="card">' +
      '<span class="card-logo"><img src="/company-logo" alt=""></span>' +
      '<div class="card-company"><span>' + escapeHtml(systemState.companyNamePrint) + '</span></div>' +
      '<div class="card-title">设备管理卡</div>' +
      fieldHtml(1, '设备名称：', device.dev_name) +
      fieldHtml(2, '位　　置：', device.location) +
      fieldHtml(3, '控制范围：', device.control_range) +
      fieldHtml(4, '维护电话：', formatPhone(device.phone)) +
      '<span class="card-sep s1"></span><span class="card-sep s2"></span>' +
      '<span class="card-sep s3"></span><span class="card-sep s4"></span>' +
      '</article>'
    );
  }

  function printQuery() {
    var params = new URLSearchParams();
    var f = devicesState.filters;
    if (f.keyword) params.set('keyword', f.keyword);
    if (f.field) params.set('field', f.field);
    if (f.name) params.set('name', f.name);
    if (f.location) params.set('location', f.location);
    // 台账「打印选中」传进来的是 id 快照；服务端按它再过滤一次。
    if (printState.ids.length > 0) params.set('ids', printState.ids.join(','));
    return params.toString();
  }

  function printLayoutSize() {
    return Number(el.printLayout.value) || 12;
  }

  function updatePrintControls() {
    var empty = printState.items.length === 0;
    var selected = printState.ids.length;
    el.printExport.disabled = printState.busy || empty;
    el.printPrint.disabled = printState.busy || empty;
    el.printEmpty.hidden = !empty || printState.busy;
    el.printClearSelection.hidden = selected === 0;
    var layout = printLayoutSize();
    var pages = empty ? 0 : Math.ceil(printState.items.length / layout);
    el.printScope.textContent = (selected > 0 ? '已选 ' + selected + ' 台 · ' : '当前筛选结果 ') +
      printState.items.length + ' 条 · 每页 ' +
      layout + ' 张 · 共 ' + pages + ' 页' +
      (printState.truncated ? '（超过 ' + printState.limit + ' 条，已截断）' : '');
  }

  function renderPrint() {
    var layout = printLayoutSize();
    var items = printState.items;
    var html = '';
    for (var start = 0; start < items.length; start += layout) {
      html += '<div class="sheet-wrap"><div class="sheet" data-layout="' + layout + '">' +
        items.slice(start, start + layout).map(cardHtml).join('') + '</div></div>';
    }
    el.printStage.innerHTML = html;
    updatePrintControls();
    updatePrintScale();
    updatePrintFontWarning();
  }

  // 预览里把「打印字体没有的字」集中标出来，避免用户在按下导出后才发现版式变了。
  function updatePrintFontWarning() {
    var missing = [];
    var where = [];
    function scan(text, label) {
      var chars = uncoveredChars(text);
      if (!chars.length) return;
      chars.forEach(function (ch) {
        if (missing.indexOf(ch) < 0) missing.push(ch);
      });
      if (where.indexOf(label) < 0) where.push(label);
    }
    scan(systemState.companyNamePrint, '公司名');
    printState.items.forEach(function (item) {
      scan(item.dev_name, '设备名称');
      scan(item.location, '位置');
      scan(item.control_range, '控制范围');
    });
    el.printFontWarning.hidden = missing.length === 0;
    if (!missing.length) {
      el.printFontWarning.textContent = '';
      return;
    }
    el.printFontWarning.textContent =
      '以下字符不在打印字体范围内：' + missing.join('、') + '（出现于：' + where.join('、') +
      '）。其中公司名会按回退后的真实字宽量一次：塞得进公司名框就允许导出，塞不进则导出被阻止；' +
      '设备名称/位置/控制范围按固定列宽排版，不做此判定。要两平台完全一致，请避免这些字或扩展字体子集后重出样张。';
  }

  // 预览按容器宽度缩放；打印时 CSS 会还原成 1:1 的 A4。
  function updatePrintScale() {
    if (el.viewPrint.hidden) return;
    var width = el.printStage.clientWidth;
    if (!width) return;
    var pageWidth = 210 * 96 / 25.4;
    var scale = Math.min(1, (width - 8) / pageWidth);
    el.printStage.style.setProperty('--sheet-scale',
      String(Math.max(0.2, Math.round(scale * 1000) / 1000)));
  }

  function sha256Hex(buffer) {
    return crypto.subtle.digest('SHA-256', buffer).then(function (digest) {
      return Array.prototype.map.call(new Uint8Array(digest), function (b) {
        return ('0' + b.toString(16)).slice(-2);
      }).join('');
    });
  }

  function probePrintFonts() {
    // 清单随覆盖接口下发；还没拿到就先取一次，避免第一次导出被误判成「字体不可用」。
    if (!printFontSpec) {
      return loadFontCoverage().then(function () {
        return printFontSpec ? probePrintFonts() : false;
      });
    }
    if (!printFontSpec.length) return Promise.resolve(false);
    if (!(window.crypto && crypto.subtle)) return Promise.resolve(false);
    return Promise.all(printFontSpec.map(function (spec) {
      // no-store：走缓存会掩盖「这一版根本没带字体」。
      return fetch(spec.url, { cache: 'no-store' }).then(function (response) {
        if (!response.ok) return false;
        return response.arrayBuffer().then(function (buffer) {
          if (buffer.byteLength !== spec.bytes) return false;
          return sha256Hex(buffer).then(function (digest) {
            return digest === spec.sha256;
          });
        });
      }).catch(function () { return false; });
    })).then(function (results) {
      return results.every(function (ok) { return ok; });
    });
  }

  // 校验机制本身能不能跑，必须和「字体不一致」分开报：前者是环境问题，后者是数据问题，
  // 处置完全不同。crypto.subtle 只在安全上下文里存在；缺了它 digest 会抛 TypeError，
  // 若被 catch 吞成「字体校验失败」，用户看到的是「导出被拒绝」，真实原因却是判据没跑起来。
  function fontGateEnvironmentError() {
    if (window.isSecureContext !== true) {
      return '当前页面不是安全上下文（isSecureContext=false），无法校验字体完整性，导出已中止';
    }
    if (!(window.crypto && window.crypto.subtle &&
          typeof window.crypto.subtle.digest === 'function')) {
      return '当前环境缺少 crypto.subtle.digest，无法校验字体完整性，导出已中止';
    }
    return '';
  }

  // 负向自检专用：壳层以 DEVCARD_SELFTEST_NOSUBTLE=1 启动时页面带 ?selftest_nosubtle=1，
  // 这里摘掉 subtle，验证「判据没跑起来」会被独立拦下，而不是混进「字体不一致」。
  if (/[?&]selftest_nosubtle=1/.test(location.search)) {
    try {
      Object.defineProperty(window.crypto, 'subtle', { value: undefined, configurable: true });
    } catch (e) { /* 只读实现也拦不住：下面的探测照样为假 */ }
  }

  // 自检取值口：安全上下文的真实值，供壳层在 selftest 里打印，不按规范推断。
  // 同时暴露字体就绪入口，负向自检要先跑一遍闸门，才能拿到具体错误而不是「未就绪」。
  window.__devcardEnsurePrintFonts = ensurePrintFonts;

  window.__devcardFontGate = function () {
    return {
      href: location.href,
      secure: window.isSecureContext,
      subtle: typeof (window.crypto && window.crypto.subtle),
      digest: typeof (window.crypto && window.crypto.subtle && window.crypto.subtle.digest),
      error: fontGateEnvironmentError()
    };
  };

  function ensurePrintFonts() {
    var envError = fontGateEnvironmentError();
    if (envError) {
      printFonts.ready = false;
      printFonts.error = envError;
      return Promise.resolve(false);
    }
    var sample = '设备管理卡 0123456789';
    var loaded = (document.fonts && document.fonts.load)
      ? Promise.all([
          document.fonts.load('700 11pt "DevCard Sans"', sample),
          document.fonts.load('400 11pt "DevCard Sans"', sample)
        ]).then(function () { return document.fonts.ready; })
          .then(function () {
            return document.fonts.check('700 11pt "DevCard Sans"', sample) &&
                   document.fonts.check('400 11pt "DevCard Sans"', sample);
          })
          .catch(function () { return false; })
      : Promise.resolve(true);
    return Promise.all([probePrintFonts(), loaded]).then(function (results) {
      var ok = results[0] && results[1];
      printFonts.ready = ok;
      printFonts.error = ok ? '' :
        '子集字体未通过校验（/fonts/ 取不到、字节数或 SHA256 与构建期清单不符），导出已中止：' +
        '整份字体缺失时影响不可枚举，无法判断哪些字、哪些行会变，不继续产出 PDF';
      return ok;
    });
  }

  function loadPrint() {
    printState.busy = true;
    updatePrintControls();
    return api('GET', '/api/print/cards?' + printQuery())
      .then(function (data) {
        printState.items = data.items || [];
        printState.truncated = !!data.truncated;
        printState.limit = data.print_limit || 10000;
        // 勾选快照与实际可打印数据对齐：被删除或不再匹配当前筛选的 id 剔除，
        // 「已选 N 台 · X 条」不再显示两个对不上的数字。
        if (printState.ids.length > 0) {
          var present = {};
          printState.items.forEach(function (item) { present[item.id] = true; });
          printState.ids = printState.ids.filter(function (id) {
            return present[id];
          });
        }
        if (data.company_name) systemState.companyNamePrint = data.company_name;
        // 打印页初始化就等字体：用户点导出时字体必须已经就位，
        // 而不是在截图那一瞬间才去加载。
        return ensurePrintFonts();
      })
      .then(function (fontsOk) {
        printState.busy = false;
        renderPrint();
        if (!fontsOk) {
          toast(printFonts.error, 'error');
          return;
        }
        el.printHint.textContent = hasNativeBridge()
          ? '导出 PDF 会打开保存对话框；直接打印会打开系统打印面板。两条通道共用同一份版式与数据快照。'
          : '浏览器模式：两个按钮都会打开打印对话框，选「另存为 PDF」即可导出。';
      })
      .catch(function (error) {
        printState.busy = false;
        printState.items = [];
        printState.truncated = false;
        renderPrint();
        if (handleAuthFailure(error)) return;
        toast(error.message, 'error');
      });
  }

  // 壳层导出 PDF 前切换到 1:1 布局，并返回每页的截取区域（CSS px，页面坐标）。
  window.__devcardExportLayout = function () {
    // 字体没就绪就返回错误：壳层据此中止，不产出用系统字体顶替的 PDF。
    if (!printFonts.ready) {
      return { error: printFonts.error || '子集字体未就绪，已中止导出' };
    }
    // 单字回退是可枚举的：公司名按真实字宽量，塞不进公司名框就不许导出。
    // 这里查 systemState.companyNamePrint（打印实际用的那份），不是输入框里的原文。
    var fontCheck = companyFontCheck(systemState.companyNamePrint || '');
    if (fontCheck.overflow) {
      return { error: '公司名含打印字体不支持的字（' + fontCheck.missing.join('、') +
                      '），回退渲染后超出公司名框，已中止导出' };
    }
    document.body.classList.add('is-export');
    window.scrollTo(0, 0);
    var sheets = document.querySelectorAll('#print-stage .sheet');
    if (!sheets.length) {
      document.body.classList.remove('is-export');
      return null;
    }
    var rects = Array.prototype.map.call(sheets, function (sheet) {
      var rect = sheet.getBoundingClientRect();
      return {
        x: rect.left + window.pageXOffset,
        y: rect.top + window.pageYOffset,
        width: rect.width,
        height: rect.height
      };
    });
    return { pages: rects.length, rects: rects };
  };

  window.__devcardExportDone = function () {
    document.body.classList.remove('is-export');
    updatePrintScale();
  };

  el.printLayout.addEventListener('change', renderPrint);

  window.addEventListener('resize', updatePrintScale);

  // Windows 壳的打印对话框没有 SDK 事件可监听关闭（稳定版 ICoreWebView2_16
  // 无 PrintDialogClosing）：afterprint 触发说明对话框已关或已出纸，
  // 通知壳释放 print_busy 并收起 is-export 布局；若对话框被取消、页面
  // 收不到打印事件，由壳侧 20s 兜底定时器释放。浏览器模式无需上报。
  window.addEventListener('afterprint', function () {
    if (window.__devcardShell) sendToShell('printDone');
  });

  el.printExport.addEventListener('click', function () {
    if (!printState.items.length) return;
    // 有壳层时一律交给壳：壳会向页面要布局，页面用 {error} 回应并中止，
    // 这样错误能落到系统对话框/日志里，而不是停在页面里没人知道。
    if (sendToShell('exportPdf', { filename: '设备管理卡.pdf' })) return;
    // 在壳里却拿不到桥：说明注入失败，不能再静默退回 window.print()
    // （那样文件名由打印预览按标题生成，壳既不知道也管不了）。
    if (window.__devcardShell) {
      toast('壳层桥不可用，导出已中止，请重启程序', 'error');
      return;
    }
    if (!printFonts.ready) {
      toast(printFonts.error || '子集字体未就绪，已中止导出', 'error');
      return;
    }
    window.print();
  });

  el.printPrint.addEventListener('click', function () {
    if (!printState.items.length) return;
    if (sendToShell('print')) return;
    if (!printFonts.ready) {
      toast(printFonts.error || '子集字体未就绪，已中止打印', 'error');
      return;
    }
    window.print();
  });

export { cardHtml, ensurePrintFonts, fieldHtml, fontGateEnvironmentError, formatPhone, loadPrint, printLayoutSize, printQuery, probePrintFonts, renderPrint, sha256Hex, updatePrintControls, updatePrintFontWarning, updatePrintScale };
