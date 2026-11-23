import { api, appState, el, formatBytes, toast, updateState } from './core.js';
import { isMacShell, isWinWebview, sendToShell } from './shell.js';

  // ---------- 版本检查（M6）：只查、只告知，不下载、不替换 ----------
  //
  // 不做自替换：安装包没有代码签名（Windows 不买 Authenticode、macOS 不公证），
  // 静默下载后覆盖自身会被 SmartScreen / Gatekeeper / 杀软拦住，而且把未经系统信任的
  // 二进制写进程序目录本身就不该做。所以这一层只有三件事：查、提示、给发布页地址。
  //
  // 联网放在页面侧而不是 C++ 侧：httplib 没编 OpenSSL，Windows 那边要为此多带
  // libssl/libcrypto 两份 DLL；页面 fetch 在两个壳里都直接走系统 TLS 栈。
  // 代价是检查地址必须允许跨域（GitHub/Gitee 的 Release 接口都带 CORS 头，
  // 自建清单要在反代上加 Access-Control-Allow-Origin）。
  var UPDATE_DELAY_MS = 4000;    // 启动后延迟，避开首屏
  var UPDATE_TIMEOUT_MS = 3000;  // 后台静默检查：内网客户连不上外网是常态，失败要秒级返回

  // 用户主动点「检查更新」时给足时间：安装后第一次访问是冷连接
  // （DNS + TLS 握手 + 代理），3 秒常不够，第二次复用连接才通过。
  var UPDATE_MANUAL_TIMEOUT_MS = 8000;

  var UPDATE_LAST_KEY = 'devcard.update.lastCheckAt';

  // 自检可替换传输层；正常运行时为 null，走真 fetch。
  var updateTransport = null;

  // 只认点分数字；GitHub/Gitee 的 tag 常带 v 前缀，剥掉再判。
  function normalizeVersion(text) {
    var value = String(text == null ? '' : text).trim().replace(/^[vV]/, '');
    if (!/^\d+(\.\d+)*$/.test(value)) return '';
    return value;
  }

  function compareVersion(left, right) {
    var a = normalizeVersion(left);
    var b = normalizeVersion(right);
    if (!a || !b) return 0;
    var pa = a.split('.').map(Number);
    var pb = b.split('.').map(Number);
    var count = Math.max(pa.length, pb.length);
    for (var i = 0; i < count; i++) {
      var l = i < pa.length ? pa[i] : 0;
      var r = i < pb.length ? pb[i] : 0;
      if (l !== r) return l > r ? 1 : -1;
    }
    return 0;
  }

  // 发布元数据两条来源都认：GitHub/Gitee Release（tag_name/html_url/body）
  // 或 v1 自管清单（latest/download_url/notes）。版本号不合法就整条不认。
  // v1 清单的分平台字段已随清单 v2 退役：版本/构建号的分平台判定
  // 收口到服务端（读验签过的 manifest.json）。
  function parseRelease(doc) {
    if (!doc || typeof doc !== 'object' || Array.isArray(doc)) return null;
    // 更新清单 v2（manifest.json）：两平台同一份，只看本平台的版本号。
    // artifacts 不在前端消费——WebView 里没有 Ed25519，远端清单未经客户端
    // 验签，不能作为下载资产来源；下载入口只信本地清单（服务端验签后透传
    // 的 assets）。assets 给 null：openUpdateModal 会回退到本地透传资产，
    // 本地也没有时就只提示版本、不给下载按钮。
    if (doc.manifest_version === 2 && doc.platforms &&
        typeof doc.platforms === 'object' && !Array.isArray(doc.platforms)) {
      var ua = navigator.userAgent || '';
      var v2key = /Windows/i.test(ua) ? 'windows'
        : (/Macintosh|Mac OS X/i.test(ua) ? 'macos' : '');
      var section = v2key ? doc.platforms[v2key] : null;
      if (!section || typeof section !== 'object') {
        // 清单只投了另一平台：视为「本平台无更新」，而不是格式错误。
        return { version: '', url: '', notes: '', assets: null };
      }
      var v2version = normalizeVersion(section.version || '');
      if (!v2version) return null;
      return {
        version: v2version,
        // notes_url 是发布说明页，当「打开下载页」的跳转目标；不是 https 就不认。
        url: /^https:\/\//i.test(String(section.notes_url || ''))
          ? String(section.notes_url) : '',
        notes: '',
        assets: null,
        // 强制更新：与 Mote / 小兔同一份语义，是否「已生效」由 mandatoryActive 判定
        mandatory: !!section.mandatory,
        mandatory_deadline: String(section.mandatory_deadline || ''),
        mandatory_reason: String(section.mandatory_reason || '')
      };
    }
    var version = normalizeVersion(doc.tag_name || doc.latest ||
      doc.version || '');
    if (!version) return null;
    // release.url 只从 html_url 取（GitHub Release 页面 URL），
    // 不认 download_url——那个字段在 OSS 目录清单里经常是指向某个资产文件的，
    // 把它当作下载页面会出现 "打开下载页" 跳到 404 目录。
    // 没有 html_url 时前端会 fallback 到 manifest.download_page（后端的 kDefaultDownloadPage）。
    var url = doc.html_url || '';
    var notes = doc.body || doc.notes || '';
    return {
      version: version,
      url: /^https?:\/\//i.test(String(url)) ? String(url) : '',
      notes: typeof notes === 'string' ? notes.slice(0, 300) : '',
      assets: parseAssets(doc.assets)
    };
  }

  // 只认带 sha256 摘要与字节数的资产；两者缺一不可，缺了就不提供下载入口。
  function parseAssets(list) {
    if (!Array.isArray(list)) return [];
    var assets = [];
    list.forEach(function (item) {
      if (!item || typeof item !== 'object') return;
      var link = String(item.browser_download_url || item.download_url || '');
      if (!/^https:\/\//i.test(link)) return;
      var digest = String(item.digest || item.sha256 || '');
      var match = /^sha256:([0-9a-f]{64})$/i.exec(digest);
      var size = Number(item.size);
      assets.push({
        name: String(item.name || ''),
        url: link,
        size: isFinite(size) && size > 0 ? size : 0,
        sha256: match ? match[1].toLowerCase() : '',
        version: normalizeVersion(item.version || '')
      });
    });
    return assets;
  }

  function pickUpdateAsset(assets) {
    // Windows 优先安装包（.exe）：自动更新只认 .exe，绿色包 zip 只能手动替换。
    // 顺序反了会让清单里同时有 zip 时选中不可安装的包。
    var prefer = /Windows/i.test(navigator.userAgent) ? ['.exe', '.zip'] : ['.dmg'];
    for (var i = 0; i < prefer.length; i++) {
      for (var j = 0; j < assets.length; j++) {
        var name = String(assets[j].name || '').toLowerCase();
        if (name.slice(-prefer[i].length) === prefer[i]) return assets[j];
      }
    }
    return null;
  }

  // 一次远端检查。永远 resolve，不抛：失败路径要和「没有新版本」在界面上不可区分。
  function remoteCheck(config, timeoutMs) {
    var limit = Number(timeoutMs) > 0 ? Number(timeoutMs) : UPDATE_TIMEOUT_MS;
    var endpoint = String((config && config.endpoint) || '');
    if (!/^https?:\/\//i.test(endpoint)) {
      return Promise.resolve({ status: 'disabled', endpoint: endpoint, raw: '', release: null });
    }
    var guard = updateTimeoutGuard(limit);
    return Promise.race([fetchRemoteManifest(endpoint, guard.controller), guard.timeout])
      .then(function (result) {
        clearTimeout(guard.timer);
        if (result && result.timedOut) {
          return {
            status: 'timeout',
            endpoint: endpoint,
            raw: '',
            release: null,
            detail: '超过 ' + limit + ' ms 没有响应'
          };
        }
        return result;
      });
  }

  // 超时不能只靠 abort：没有 AbortController（或传输层不认 signal）时，
  // 那个请求会一直挂着，界面永远等不到结论。所以超时自己是竞速的一方。
  function updateTimeoutGuard(limit) {
    var controller = typeof AbortController === 'function' ? new AbortController() : null;
    var timer = null;
    var timeout = new Promise(function (resolve) {
      timer = setTimeout(function () {
        if (controller) {
          try { controller.abort(); } catch (error) { /* 已经结束就无所谓 */ }
        }
        resolve({ timedOut: true });
      }, limit);
    });
    return { controller: controller, timer: timer, timeout: timeout };
  }

  // HTTP 取清单原文并解析；传输层与 HTTP 错误都归成 unreachable。
  function fetchRemoteManifest(endpoint, controller) {
    var options = {
      method: 'GET',
      cache: 'no-store',
      headers: { Accept: 'application/vnd.github+json' }
    };
    if (controller) options.signal = controller.signal;
    var transport = updateTransport || function (url, opts) { return fetch(url, opts); };
    return transport(endpoint, options)
      .then(function (response) {
        if (!response || !response.ok) {
          throw new Error('HTTP ' + (response ? response.status : '?'));
        }
        return response.text();
      })
      .then(function (text) {
        return parseRemoteManifest(endpoint, text);
      })
      .catch(function (error) {
        return {
          status: 'unreachable',
          endpoint: endpoint,
          raw: '',
          release: null,
          detail: (error && error.message) || String(error)
        };
      });
  }

  // 解析远端清单。v2 清单的 artifacts 不能直接采信（WebView 里没有 Ed25519
  // 验签能力）：把原文交回服务端用内嵌公钥验签，通过的资产才挂到 release.assets。
  // 没有这一步，运维没往数据目录投放本地清单时，下载与安装按钮会永远是灰的。
  function parseRemoteManifest(endpoint, text) {
    var doc = null;
    try {
      doc = JSON.parse(text);
    } catch (error) {
      doc = null;
    }
    var release = parseRelease(doc);
    var fallback = {
      status: release ? 'parsed' : (doc ? 'invalid-version' : 'invalid-json'),
      endpoint: endpoint,
      raw: String(text || ''),
      release: release
    };
    if (!release || !doc || doc.manifest_version !== 2) return Promise.resolve(fallback);
    return api('POST', '/api/version/verify', { manifest: String(text || '') })
      .then(function (verified) {
        if (verified && verified.ok && Array.isArray(verified.assets) &&
            verified.assets.length) {
          release.assets = parseAssets(verified.assets);
        }
        return fallback;
      })
      .catch(function () {
        // 验签接口不可用就退回「只提示版本」，不影响台账。
        return fallback;
      });
  }

  // 频率：上次检查时间存本地（WebView 自己的存储），每 interval 小时最多查一次。
  // 存不进（隐私模式/存储被清）就当作没查过，最坏是多打一次请求，不影响业务。
  function updateThrottleAllows(intervalHours) {
    try {
      var last = Number(window.localStorage.getItem(UPDATE_LAST_KEY) || 0);
      if (!last) return true;
      return Date.now() - last >= intervalHours * 3600 * 1000;
    } catch (error) {
      return true;
    }
  }

  function updateThrottleMark() {
    try {
      window.localStorage.setItem(UPDATE_LAST_KEY, String(Date.now()));
    } catch (error) { /* 存不下就不存，不为此报错 */ }
  }

  function setUpdatePill(version, notes) {
    el.updatePill.textContent = '有新版本 v' + version;
    el.updatePill.title = (notes ? notes + '；' : '') +
      '点击查看手动更新步骤（不会自动下载或安装）';
    el.updatePill.hidden = false;
  }

  // checkUpdate(silent): silent=true（默认）只设顶栏 pill，不弹窗；
  // silent=false 表示用户主动触发（壳层菜单），有更新直接弹 modal，无更新弹 toast。
  //
  // Windows WebView2 壳里优先走 hostObjects.updater_win bridge（原生桥 + 验签），
  // 浏览器里或 bridge 不可用时退回原有 HTTP 路径（/api/version + remoteCheck 拉 OSS 清单）。
  function checkUpdate(silent) {
    if (silent === undefined) silent = true;
    lastCheckSilent = silent;

    var bridge = winUpdaterBridge();
    if (bridge) return checkViaWinBridge(bridge, silent);

    // —— HTTP 兜底路径（浏览器或 macOS Sparkle 壳）——
    return api('GET', '/api/version')
      .then(function (data) {
        updateState.info = data || null;
        if (applyLocalManifest(data, silent)) return;
        var check = (data && data.check) || {};
        if (!check.enabled) {
          if (!silent) toast('已是最新版 v' + (appState.version || '?'));
          return;
        }
        var hours = Number(check.interval_hours) > 0 ? Number(check.interval_hours) : 24;
        if (!silent) {
          // 用户主动触发时跳过硬编码节流（但仍尊重 update.json 的 enabled）。
          return remoteCheck(check, UPDATE_MANUAL_TIMEOUT_MS).then(function (result) {
            applyManualRemoteCheck(result, data);
          });
        }
        return runSilentRemoteCheck(check, data, hours);
      })
      .catch(function () {
        // 检查失败不打扰台账操作，也不保留上一次的提示。
        updateState.info = null;
        if (!updateState.check) el.updatePill.hidden = true;
        if (!silent) toast('检查更新失败：无法连接服务器');
      });
  }

  // Windows WebView2 壳优先走 updater_win bridge：壳自己拉清单并验签。
  // 结果异步到达：chrome.webview.message → bridgeCheckNowResult 已经在 message 事件里处理 UI。
  function checkViaWinBridge(bridge, silent) {
    // 静默检查也直接触发壳的清单拉取（updater_win 内部自己节流，不会重复打）。
    return bridge.checkNow().then(function () {
      // 主动检查时等待一下让结果先回来，再决定要不要弹 modal。
      if (!silent) return bridge.poll();
    });
  }

  // 本地清单（M5 路径）仍然优先：运维可以直接投放 update.json 指定版本。
  // 命中时返回 true，调用方不再走远端检查。
  function applyLocalManifest(data, silent) {
    if (!data || !data.update_available || !data.latest) return false;
    setUpdatePill(data.latest, data.notes || '');
    if (!silent) openUpdateModal();
    return true;
  }

  // 用户主动触发：失败原因分开说（第一次冷连接超时最容易被误读成「数据格式不合法」）。
  function applyManualRemoteCheck(result, data) {
    updateState.check = result;
    if (result.status !== 'parsed' || !result.release) {
      if (result.status === 'timeout') {
        toast('检查更新失败：连接更新服务器超时，请稍后重试');
      } else if (result.status === 'unreachable') {
        toast('检查更新失败：无法连接更新服务器');
      } else {
        toast('检查更新失败：返回数据格式不合法');
      }
      return;
    }
    var current = appState.version || (data && data.version) || '';
    if (compareVersion(result.release.version, current) <= 0) {
      toast('已是最新版 v' + current);
      return;
    }
    setUpdatePill(result.release.version, result.release.notes || '');
    openUpdateModal();
  }

  // 启动时自动检查：节流 + 延迟首屏之后才发请求，失败保持静默。
  function runSilentRemoteCheck(check, data, hours) {
    if (!updateThrottleAllows(hours)) return;
    return new Promise(function (resolve) {
      setTimeout(resolve, UPDATE_DELAY_MS);
    }).then(function () {
      return remoteCheck(check);
    }).then(function (result) {
      updateState.check = result;
      updateThrottleMark();
      if (result.status !== 'parsed' || !result.release) return;
      var current = appState.version || (data && data.version) || '';
      if (compareVersion(result.release.version, current) <= 0) return;
      setUpdatePill(result.release.version, result.release.notes || '');
    });
  }

  // 手动更新的步骤要按平台写清楚：只说「有新版本」等于没说。
  function updateSteps() {
    var ua = navigator.userAgent || '';
    // macOS 桌面壳走 Sparkle，不再让用户手动拖 DMG。
    if (isMacShell()) {
      return [
        '点「用 Sparkle 安装并重启」，由 Sparkle 检查新版本并下载。',
        '在 Sparkle 窗口里确认安装，App 会自动替换并重启。',
        '台账数据在用户数据目录，升级不会动它；也可用菜单栏「DevCard → 检查更新…」。'
      ];
    }
    if (/Macintosh|Mac OS X/i.test(ua)) {
      return [
        '点「下载更新包」把 DMG 下载到数据目录并校验（只下载，不安装）；也可打开下载页手动下载。',
        '双击打开 DMG，把 DevCard 拖进「应用程序」，选「替换」覆盖旧版本。',
        '退出并重新打开 DevCard。台账数据在用户数据目录，覆盖安装不会动它。'
      ];
    }
    if (/Windows/i.test(ua)) {
      // Windows WebView2 壳有 updater_win bridge：自动下载+校验+静默安装
      if (isWinWebview() && winUpdaterBridge()) {
        return [
          '点「安装并重启」，由壳层自动完成：拉取签名清单、下载安装包、SHA256 校验、',
          '退出当前进程并运行安装器（ISCC /VERYSILENT 覆盖旧版）、自动重启新版。',
          '数据目录在用户目录，升级不动它；也可在顶栏菜单「帮助 → 检查更新」手动触发。'
        ];
      }
      return [
        '点「下载更新包」把安装包下载到数据目录并校验（只下载，不安装）；也可打开下载页手动下载。',
        '先退出 DevCard，再运行安装包，装到原来的目录覆盖旧版本。',
        '重新打开 DevCard。台账数据在用户数据目录，覆盖安装不会动它。'
      ];
    }
    return [
      '点「下载更新包」下载并校验，或按下载页说明手动下载对应平台的安装包。',
      '在运行服务的机器上替换旧版本并重启服务；台账数据在服务端数据目录，不会随升级丢失。'
    ];
  }

  // 强制升级原因文案：与 Mote / 小兔的 mandatoryReasonLabel 逐字一致。
  var MANDATORY_REASON_LABELS = {
    SECURITY: '安全漏洞修复',
    PROTOCOL: '协议不兼容',
    REGULATORY: '法规或税控要求',
    MIN_SUPPORTED: '低于服务端最低支持版本'
  };

  // 强制是否已生效：未设期限 → 立即强制；设了期限 → 到期才强制（截止前可稍后）。
  // 期限解析失败按「已到期」处理，避免格式问题成为绕过手段；三端同一口径。
  function mandatoryActive(info) {
    if (!info || !info.mandatory) return false;
    var raw = info.deadline || '';
    if (!raw) return true;
    var at = Date.parse(raw);
    return !isFinite(at) || Date.now() >= at;
  }

  // 汇总弹窗要用的数据：本地清单命中时以本地为准（与 checkUpdate 的「本地优先」一致），
  // 此时不会拉远端，资产只能来自 /api/version 透传的 manifest.assets。
  function updateModalInfo() {
    var manifest = updateState.info || {};
    var result = updateState.check || null;
    var localWins = !!(manifest.update_available && manifest.latest);
    var release = (!localWins && result && result.release) ? result.release : null;
    // 下载页优先用后端 download_page（运维给的是页面地址）；
    // update.json 的 download_url 在自管清单里指向具体安装包文件，
    // 只有当后端没给页面时才退回它，避免「打开下载页」直接下载一个 exe。
    var url = (release && release.url) || manifest.download_page ||
      manifest.download_url || '';
    return {
      manifest: manifest,
      release: release,
      latest: release ? release.version : (manifest.latest || ''),
      url: url,
      notes: (release && release.notes) || manifest.notes || '',
      mandatory: !!(manifest.mandatory || (release && release.mandatory)),
      deadline: (release && release.mandatory_deadline) ||
        manifest.mandatory_deadline || '',
      reason: (release && release.mandatory_reason) ||
        manifest.mandatory_reason || '',
      assets: (release && release.assets) || parseAssets(manifest.assets || [])
    };
  }

  // 标题区的版本号、更新说明与操作步骤。
  function renderUpdateSummary(info) {
    updateState.url = /^https?:\/\//i.test(info.url) ? info.url : '';
    el.updateVersion.textContent = '当前版本 v' + (appState.version || '?') +
      (info.latest ? '，最新版本 v' + info.latest : '');
    el.updateNotes.textContent = info.notes;
    el.updateNotes.hidden = !info.notes;
    el.updateSteps.innerHTML = '';
    updateSteps().forEach(function (text) {
      var item = document.createElement('li');
      item.textContent = text;
      el.updateSteps.appendChild(item);
    });
    el.updateDownload.disabled = !updateState.url;
  }

  // 强制更新：截止前仍可「稍后」，到期后弹窗不可关闭（与 Mote / 小兔同一口径）。
  function renderUpdateMandatory(info) {
    // 没有下载入口时不阻塞：否则用户既升不了级又关不掉弹窗。
    updateState.force = mandatoryActive({
      mandatory: info.mandatory,
      deadline: info.deadline
    }) && !!updateState.url;
    var force = updateState.force;
    el.updateTitle.textContent = force ? '必须更新' : '发现新版本';
    el.updateLater.hidden = force;
    if (!info.mandatory) {
      el.updateMandatory.hidden = true;
      el.updateMandatory.textContent = '';
      el.updateMandatory.classList.remove('is-error');
      return;
    }
    var label = MANDATORY_REASON_LABELS[info.reason] || '';
    el.updateMandatory.textContent = force
      ? '此更新已到截止时间' + (info.deadline ? '（' + info.deadline + '）' : '') +
        '，不能跳过。' + (label ? '原因：' + label + '。' : '')
      : '该版本为强制更新' + (label ? '（' + label + '）' : '') +
        (info.deadline ? '，请在 ' + info.deadline + ' 前完成升级，到期后将无法跳过。' : '。');
    el.updateMandatory.hidden = false;
    el.updateMandatory.classList.toggle('is-error', force);
  }

  // 阶段一：只下载 + 校验。按钮是否可用取决于清单里有没有「带 SHA256 与
  // 字节数」的当前平台资产；缺任一条件就退回只提示，不给下载入口。
  // 资产两个来源：远端检查结果，或本地清单直接写的 assets（服务端原样透传）。
  function renderUpdateAssets(info) {
    updateState.asset = pickUpdateAsset(info.assets);
    updateState.assetVersion = info.release ? info.release.version
      : (info.manifest.latest || '');
    updateState.downloading = false;
    if (updateState.timer) {
      clearTimeout(updateState.timer);
      updateState.timer = null;
    }
    var asset = updateState.asset;
    var isAdmin = appState.user && appState.user.role === 'admin';
    var ua = navigator.userAgent || '';
    var isWindows = /Windows/i.test(ua);
    // macOS 的替换与重启交给 Sparkle：只有桌面壳里有桥，浏览器里没有。
    var canSparkle = isMacShell();
    // 清单「最新版本」与选中安装包的版本不一致时停用自动下载/安装：
    // 例如 latest=1.2.3 但只投了 1.2.2 的安装包，用户点了会装回 1.2.2，
    // 版本没变 → 下次打开仍提示「有新版本」，形成反复安装失败的循环。
    // 资产版本优先取清单里的 version 字段，没有才从文件名解析。
    var assetVersion = (asset && asset.version) || '';
    if (!assetVersion && asset && asset.name) {
      var matched = /-(\d+\.\d+\.\d+)(?:[-.]|$)/.exec(asset.name);
      if (matched) assetVersion = matched[1];
    }
    var versionMismatch = !!(assetVersion && info.latest &&
      assetVersion !== info.latest);
    if (versionMismatch) {
      asset = null;
      updateState.asset = null;
    }
    // Windows WebView2 壳有 updater_win bridge：壳自己拉 manifest.json + WinHTTP 下载 +
    // BCrypt SHA256 校验，不依赖前端 HTTP 服务的 asset/sh256/size。
    var hasWinBridge = isWinWebview() && winUpdaterBridge();
    // macOS 桌面壳不需要前端下载安装包：Sparkle 自己下载并验签。
    el.updateDownloadPackage.disabled = canSparkle ||
      (hasWinBridge ? false : (versionMismatch || !(asset && asset.sha256 && asset.size && isAdmin)));
    if (versionMismatch && !hasWinBridge) {
      setUpdateProgress('清单里的安装包是 v' + assetVersion + '，与最新版本 v' +
        info.latest + ' 不一致，已停用自动安装（否则会装回旧版并反复提示更新）；请用下载页手动更新。', true);
    } else if (hasWinBridge) {
      setUpdateProgress('Windows 自动更新：点「下载更新包」由壳层 WinHTTP 拉清单并校验，' +
        '或直接点「安装并重启」让壳层自动完成下载+校验+安装。');
    } else if (canSparkle) {
      setUpdateProgress('macOS 走 Sparkle 自动更新：点下方「用 Sparkle 安装并重启」，' +
        '在系统弹出的窗口中完成下载、验签与安装。');
    } else if (!asset) {
      setUpdateProgress('发布清单里没有当前平台的安装包，请用下载页手动更新。');
    } else if (!asset.sha256 || !asset.size) {
      setUpdateProgress('发布清单缺少 SHA256 或字节数，无法校验，已退回只提示；请用下载页手动更新。');
    } else if (!isAdmin) {
      setUpdateProgress('只有管理员可以下载更新包，请用下载页手动更新。');
    } else {
      setUpdateProgress('');
    }
    renderUpdateInstallControls({
      isAdmin: isAdmin,
      isWindows: isWindows,
      canSparkle: canSparkle,
      hasWinBridge: hasWinBridge,
      versionMismatch: versionMismatch
    });
  }

  // 阶段二：Windows 安装版走安装器；macOS 走 Sparkle。
  function renderUpdateInstallControls(ctx) {
    el.updateInstall.textContent = ctx.canSparkle ? '用 Sparkle 安装并重启' : '安装并重启';
    el.updateInstall.hidden = !(ctx.isAdmin && (ctx.isWindows || ctx.canSparkle));
    // Windows bridge 壳：壳自己管理下载+校验+安装，不需要先手动点下载
    el.updateInstall.disabled = ctx.hasWinBridge ? false
      : (ctx.isWindows || ctx.versionMismatch);
    updateState.installing = false;
    // 服务端若已有「下载并校验完成」的包（同一次运行里关掉弹窗再打开），
    // 直接放开「安装并重启」，不必重新下载一遍。
    if (!ctx.isWindows || !ctx.isAdmin || ctx.versionMismatch) return;
    api('GET', '/api/update/download').then(function (data) {
      if (!data || data.state !== 'done') return;
      el.updateInstall.disabled = false;
      el.updateDownloadPackage.disabled = true;
      setUpdateProgress('更新包已就绪（已通过 SHA256 + 字节数校验）：' +
        (data.path || ''));
    }).catch(function () {
      // 读不到进度就当没下载过，按钮保持原状。
    });
  }

  // 打开弹窗：先算一次数据，再交给三个渲染函数，最后展示。
  function openUpdateModal() {
    var info = updateModalInfo();
    renderUpdateSummary(info);
    renderUpdateMandatory(info);
    renderUpdateAssets(info);
    loadInstallResult();
    el.updateModal.hidden = false;
  }

  function setUpdateProgress(text, isError) {
    el.updateProgress.hidden = !text;
    el.updateProgress.textContent = text || '';
    el.updateProgress.classList.toggle('is-error', !!isError);
  }

  // 安装结果由新进程读回。只有「退出码为 0 且版本确实变了」才算成功：
  // 静默失败（安装器非 0、版本没换）一律按失败显示，不给「升级成功」四个字。
  function loadInstallResult() {
    el.updateInstallResult.hidden = true;
    el.updateInstallResult.textContent = '';
    el.updateInstallResult.classList.remove('is-error');
    if (!appState.user || appState.user.role !== 'admin') return;
    // 安装结果只属于 Windows 安装器；macOS 的安装进度在 Sparkle 自己的窗口里。
    if (!/Windows/i.test(navigator.userAgent || '')) return;
    api('GET', '/api/update/install-result').then(function (data) {
      if (!data || data.state !== 'done') return;
      var code = Number(data.exit_code);
      var before = data.version_before || '?';
      var after = data.version_after || '?';
      var ok = code === 0 && !!data.version_after &&
        data.version_after !== data.version_before;
      var text = (ok ? '上次安装成功：' : '上次安装未成功：') +
        '安装器退出码 ' + code + '｜版本 ' + before + ' → ' + after;
      if (data.error) text += '（' + data.error + '）';
      el.updateInstallResult.hidden = false;
      el.updateInstallResult.textContent = text;
      el.updateInstallResult.classList.toggle('is-error', !ok);
    }).catch(function () {
      // 读不到结果不影响手动升级，也不弹错误。
    });
  }

  el.updateInstall.addEventListener('click', handleUpdateInstall);

  function handleUpdateInstall() {
    if (updateState.installing) return;
    // macOS：交给 Sparkle 完成检查、下载、安装与重启。
    if (isMacShell()) {
      if (!sendToShell('checkForUpdates')) {
        setUpdateProgress('当前不在桌面壳内，无法调用 Sparkle；请用下载页手动更新。', true);
        return;
      }
      el.updateInstall.disabled = true;
      setTimeout(function () { el.updateInstall.disabled = false; }, 3000);
      setUpdateProgress('已请求 Sparkle 检查更新：按它弹出的窗口完成下载与安装。');
      return;
    }
    // Windows WebView2 壳：走 hostObjects.updater_win.installUpdate()
    var bridge = winUpdaterBridge();
    if (bridge) {
      el.updateInstall.disabled = true;
      setUpdateProgress('正在启动安装程序（ISCC /VERYSILENT 覆盖旧版）…');
      bridge.installUpdate().catch(function (err) {
        el.updateInstall.disabled = false;
        setUpdateProgress('安装启动失败：' + (err && err.message ? err.message : err), true);
      });
      return;
    }
    // HTTP 兜底（服务端安装器）
    updateState.installing = true;
    el.updateInstall.disabled = true;
    setUpdateProgress('正在启动安装程序，服务即将停止…');
    api('POST', '/api/update/install', {}).then(function () {
      setUpdateProgress('安装程序已启动：服务正在停止，装完会自动打开新版本。');
    }).catch(function (error) {
      updateState.installing = false;
      el.updateInstall.disabled = false;
      setUpdateProgress('安装未启动：' + error.message, true);
    });
  }

  function closeUpdateModal(force) {
    // 强制更新到期后不允许关闭；安装完成或升级后重新打开才解除
    if (updateState.force && !force) return;
    updateState.force = false;
    el.updateModal.hidden = true;
  }

  function openUpdateUrl(url) {
    if (!url) return;
    if (sendToShell('openExternal', { url: url })) return;
    window.open(url, '_blank', 'noopener');
  }

  el.updatePill.addEventListener('click', function () {
    openUpdateModal();
  });

  // 顶栏「检查更新」按钮：主动触发检查（非静默模式）。
  // macOS 壳层转交 Sparkle（原生弹窗承担下载 / 验签 / 替换 / 重启），
  // Windows 壳层与浏览器走前端 update.json 清单通道。
  if (el.updateCheckBtn) {
    el.updateCheckBtn.addEventListener('click', function () {
      if (isMacShell()) {
        if (!sendToShell('checkForUpdates')) {
          toast('当前环境不支持自动检查更新', 'error');
        }
        return;
      }
      checkUpdate(false);
    });
  }

  el.updateLater.addEventListener('click', function () { closeUpdateModal(); });

  el.updateDownload.addEventListener('click', function () {
    if (!updateState.url) {
      toast('未配置下载页地址', 'error');
      return;
    }
    openUpdateUrl(updateState.url);
    closeUpdateModal();
  });

  // 壳内走 NSSavePanel 先问落点：取消就什么都不发、不改状态；选好再让服务端
  // 直接落到该路径（大文件不转 base64，服务端先下到 updates/ 校验再整体搬过去）。
  function pickUpdateSavePath(filename) {
    return new Promise(function (resolve) {
      var bridge = window.webkit && window.webkit.messageHandlers &&
                   window.webkit.messageHandlers.devcard;
      if (!bridge) {
        resolve(null);
        return;
      }
      window.__devcardPathPicked = function (result) {
        window.__devcardPathPicked = null;
        resolve(result && result.status === 'ok' && result.path ? result.path : null);
      };
      bridge.postMessage({
        action: 'pickSavePath',
        filename: filename,
        title: '保存更新包'
      });
    });
  }

  function startUpdateDownload(savePath) {
    var asset = updateState.asset;
    updateState.downloading = true;
    el.updateDownloadPackage.disabled = true;
    setUpdateProgress('正在准备下载…');
    var payload = {
      version: updateState.assetVersion,
      file_name: asset.name,
      url: asset.url,
      sha256: asset.sha256,
      size: asset.size
    };
    if (savePath) payload.save_path = savePath;
    api('POST', '/api/update/download', payload).then(function () {
      pollUpdateDownload();
    }).catch(function (error) {
      updateState.downloading = false;
      el.updateDownloadPackage.disabled = false;
      setUpdateProgress('下载未开始：' + error.message +
        '（可用下载页手动更新）', true);
    });
  }

  el.updateDownloadPackage.addEventListener('click', handleUpdateDownloadPackage);

  function handleUpdateDownloadPackage() {
    // Windows WebView2 壳：走 updater_win.downloadUpdate()，壳自己拉 manifest + 下载 + SHA256 校验
    var bridge = winUpdaterBridge();
    if (bridge) {
      el.updateDownloadPackage.disabled = true;
      setUpdateProgress('正在下载更新包（壳层 WinHTTP + BCrypt SHA256）…');
      bridge.downloadUpdate().catch(function (err) {
        el.updateDownloadPackage.disabled = false;
        setUpdateProgress('下载启动失败：' + (err && err.message ? err.message : err), true);
      });
      return;
    }
    // macOS 壳或浏览器：走 HTTP 路径
    var asset = updateState.asset;
    if (!asset || !asset.sha256 || !asset.size || updateState.downloading) return;
    var macBridge = window.webkit && window.webkit.messageHandlers &&
                    window.webkit.messageHandlers.devcard;
    if (!window.__devcardShell || !macBridge) {
      startUpdateDownload('');
      return;
    }
    var has_ext = /\.[A-Za-z0-9]+$/.test(asset.name || '');
    var filename = has_ext ? asset.name
                           : 'DevCard-' + updateState.assetVersion + '.dmg';
    pickUpdateSavePath(filename).then(function (path) {
      if (!path) return;
      startUpdateDownload(path);
    });
  }

  // 轮询本地服务拿进度；失败一律回到「只提示」，不禁用台账，也不自动替换。
  function pollUpdateDownload() {
    if (updateState.timer) clearTimeout(updateState.timer);
    updateState.timer = setTimeout(function () {
      api('GET', '/api/update/download').then(function (data) {
        if (data.state === 'done') {
          updateState.downloading = false;
          updateState.timer = null;
          el.updateDownloadPackage.disabled = true;
          if (!el.updateInstall.hidden) el.updateInstall.disabled = false;
          setUpdateProgress('已下载并通过 SHA256 + 字节数校验：' + data.path);
          toast('更新包已下载并通过校验');
          return;
        }
        if (data.state === 'failed') {
          updateState.downloading = false;
          updateState.timer = null;
          el.updateDownloadPackage.disabled = false;
          setUpdateProgress('下载失败：' + (data.error || '未知原因') +
            '；已删除残件，可用下载页手动更新。', true);
          return;
        }
        var total = Number(data.total) || 0;
        var received = Number(data.received) || 0;
        setUpdateProgress('正在下载更新包：' + formatBytes(received) +
          (total ? ' / ' + formatBytes(total) +
            '（' + Math.floor(received * 100 / total) + '%）' : ''));
        pollUpdateDownload();
      }).catch(function (error) {
        updateState.downloading = false;
        updateState.timer = null;
        el.updateDownloadPackage.disabled = false;
        setUpdateProgress('无法获取下载进度：' + error.message +
          '（可用下载页手动更新）', true);
      });
    }, 800);
  }

  // Windows updater_win bridge：懒加载 hostObject。返回对象有 4 个方法，
  // 每个都把 WebView2 异步代理（不保证 .then）包一层 async。
  // 壳的异步回调（进度/结果）走 chrome.webview.addEventListener('message', ...)。
  var winUpdaterBridgeInstance = null;

  var winUpdaterListenerInstalled = false;

  function winUpdaterBridge() {
    if (!isWinWebview()) return null;
    // Windows 壳在页面里注入 window.__devcardUpdater（postMessage 通道）；
    // 兼容旧壳：仍支持 chrome.webview.hostObjects.updater_win。
    var updater = window.__devcardUpdater;
    if (!updater) {
      var hostObjects = window.chrome.webview.hostObjects;
      updater = hostObjects && hostObjects.updater_win;
    }
    if (!updater) return null;
    if (winUpdaterBridgeInstance) return winUpdaterBridgeInstance;
    winUpdaterBridgeInstance = makeWinUpdaterBridge(updater);
    // 一次性安装 message listener：壳 updater_win 完成检查/下载/安装后
    // 用 chrome.webview.PostWebMessageAsJson 推 event+code+status。
    installWinUpdaterListener();
    return winUpdaterBridgeInstance;
  }

  // 把 hostObjects.updater_win 包成统一接口；
  // poll 等 800ms 给 WinHTTP 拉 manifest 留一点时间。
  function makeWinUpdaterBridge(updater) {
    return {
      checkNow: async function () { await updater.checkNow(); },
      downloadUpdate: async function () { await updater.downloadUpdate(); },
      installUpdate: async function () { await updater.installUpdate(); },
      cancelPrefetch: async function () { await updater.cancelPrefetch(); },
      checkMinSupportedVersion: async function () { await updater.checkMinSupportedVersion(); },
      statusJson: async function () {
        var raw = await updater.statusJson();
        try { return JSON.parse(raw); } catch (_) { return {}; }
      },
      // 等壳 checkNow 完成后（chrome.webview.message 事件）从 statusJson 取最新状态。
      poll: function () {
        return new Promise(function (resolve) {
          setTimeout(function () {
            updater.statusJson().then(function (raw) {
              try { resolve(JSON.parse(raw)); } catch (_) { resolve({}); }
            });
          }, 800);
        });
      },
    };
  }

  // listener 只装一次：重复调用 winUpdaterBridge 不会重复挂事件。
  function installWinUpdaterListener() {
    if (winUpdaterListenerInstalled) return;
    winUpdaterListenerInstalled = true;
    window.chrome.webview.addEventListener('message', function (event) {
      dispatchWinUpdaterMessage(event && event.data);
    });
  }

  // msg = {event, code, status: {...}}；壳偶尔直接推字符串，解析失败按忽略处理。
  function dispatchWinUpdaterMessage(msg) {
    if (typeof msg === 'string') { try { msg = JSON.parse(msg); } catch (_) { msg = null; } }
    if (!msg || typeof msg !== 'object' || !msg.event) return;
    // 让 updateState.info / updateState.check 反映 bridge 结果
    if (msg.event === 'checkNow') {
      // msg.status = updater_win.statusJson()（检查后的最新状态）
      // 用 status.manifest_version / status.manifest_build 判断有无更新
      bridgeCheckNowResult(msg);
    } else if (msg.event === 'downloadUpdate') {
      bridgeDownloadResult(msg);
    } else if (msg.event === 'installUpdate') {
      bridgeInstallResult(msg);
    }
  }

  // bridge 回调 → 界面状态更新。msg = {event, code, status: {...}}
  function bridgeCheckNowResult(msg) {
    var status = msg.status || {};
    if (msg.code === 'UPDATE_AVAILABLE') {
      var version = status.manifest_version || '';
      var notes = status.notes_url || '';
      if (version) {
        updateState.info = { update_available: true, latest: version, notes: notes };
        setUpdatePill(version, notes);
        // 非静默检查 → 弹 modal
        if (!lastCheckSilent && typeof openUpdateModal === 'function') openUpdateModal();
      }
    } else if (msg.code === 'NO_UPDATE') {
      updateState.info = { update_available: false };
      el.updatePill.hidden = true;
      if (!lastCheckSilent) toast('已是最新版 v' + (appState.version || '?'));
    } else {
      if (!lastCheckSilent) toast('检查更新失败：' + (msg.code || ''));
    }
  }

  function bridgeDownloadResult(msg) {
    // 壳成功码是 DOWNLOADED；DONE 为兼容旧壳保留。
    if (msg.code === 'DONE' || msg.code === 'DOWNLOADED') {
      el.updateDownloadPackage.disabled = true;
      el.updateInstall.disabled = false;
      setUpdateProgress('更新包已下载并通过 SHA256 校验，可以点「安装并重启」');
    } else {
      setUpdateProgress('下载失败：' + (msg.detail || msg.code || ''), true);
    }
  }

  function bridgeInstallResult(msg) {
    // 壳会直接 exec ISCC 覆盖旧版；前端基本收不到成功回传
    if (msg.code === 'STARTED') {
      toast('安装程序已启动，DevCard 将自动重启');
    } else if (msg.code === 'OPEN_FAILED') {
      toast('安装程序启动失败：' + (msg.detail || msg.code));
    }
  }

  var lastCheckSilent = true;

  // ---------- 自动更新自检（M6） ----------
  //
  // 四种失败路径（超时 / DNS 失败 / 返回非 JSON / 版本号格式不对）必须和
  // 「没有新版本」在界面上不可区分：不弹窗、不显示提示、不抛异常。
  // 另留一条正向对照：没有它，「永远不提示」也能让四条失败用例全绿。
  // 只替换传输层，版本比较、状态判定仍是导出路径上的同一份代码。
  window.__devcardUpdateState = function () {
    return {
      version: appState.version,
      pill: el.updatePill.hidden ? '' : el.updatePill.textContent,
      check: updateState.check
    };
  };

  // 回放：把已抓到的真实响应体（fixture）喂进同一条解析/比较/提示链路。
  // 传输路径换成本地回放，响应体是原样的真值；输出里必须注明来源与抓取时间。
  window.__devcardUpdateReplay = function (text) {
    var raw = String(text == null ? '' : text);
    var doc = null;
    try {
      doc = JSON.parse(raw);
    } catch (error) {
      doc = null;
    }
    var release = parseRelease(doc);
    if (!release) {
      return JSON.stringify({ ok: false, reason: 'parse', bytes: raw.length });
    }
    updateState.check = {
      status: 'parsed',
      endpoint: '(fixture)',
      raw: raw,
      release: release
    };
    var current = appState.version || '';
    var fresher = compareVersion(release.version, current) > 0;
    if (fresher) setUpdatePill(release.version, release.notes || '');
    // 字节数按 UTF-8 算：JS 的 length 是 UTF-16 码元，和文件字节数不是一回事。
    var byte_length = raw.length;
    if (typeof TextEncoder === 'function') {
      byte_length = new TextEncoder().encode(raw).length;
    }
    return JSON.stringify({
      ok: fresher,
      current: current,
      latest: release.version,
      url: release.url,
      bytes: byte_length,
      pill: el.updatePill.hidden ? '' : el.updatePill.textContent
    });
  };

  // 超时诊断：卡在登录、被节流、还是请求没发出去，三者的表现都是「没有结论」，
  // 光看结果分不清，所以把中间状态一并吐出来。
  window.__devcardUpdateDiag = function () {
    var last = '';
    try {
      last = window.localStorage.getItem(UPDATE_LAST_KEY) || '';
    } catch (error) {
      last = 'unavailable';
    }
    return JSON.stringify({
      user: (appState.user && appState.user.user_name) || '',
      version: appState.version || '',
      manifest: updateState.info ? 'loaded' : 'missing',
      check_config: (updateState.info && updateState.info.check) || null,
      last_check_at: last,
      status: (updateState.check && updateState.check.status) || ''
    });
  };

  window.__devcardUpdateSelftestRun = function () {
    function okJson(body) {
      return Promise.resolve({
        ok: true,
        status: 200,
        text: function () { return Promise.resolve(body); }
      });
    }
    var cases = [
      {
        name: 'timeout',
        expect: 'timeout',
        stub: function (url, options) {
          return new Promise(function (resolve, reject) {
            var signal = options && options.signal;
            if (signal && signal.addEventListener) {
              signal.addEventListener('abort', function () {
                var error = new Error('The operation was aborted.');
                error.name = 'AbortError';
                reject(error);
              });
            }
          });
        }
      },
      {
        name: 'dns',
        expect: 'unreachable',
        stub: function () { return Promise.reject(new TypeError('Failed to fetch')); }
      },
      {
        name: 'invalid-json',
        expect: 'invalid-json',
        stub: function () { return okJson('<html><body>502 Bad Gateway</body></html>'); }
      },
      {
        name: 'invalid-version',
        expect: 'invalid-version',
        stub: function () {
          return okJson('{"tag_name":"release-2026-11","html_url":"https://example.invalid/r"}');
        }
      },
      {
        name: 'control',
        expect: 'parsed',
        version: '9.9.9',
        stub: function () {
          return okJson('{"tag_name":"v9.9.9","html_url":"https://example.invalid/r","body":"自检对照"}');
        }
      }
    ];

    el.updatePill.hidden = true;
    var results = [];
    var chain = Promise.resolve();
    cases.forEach(function (item) {
      chain = chain.then(function () {
        updateTransport = item.stub;
        var started = Date.now();
        return remoteCheck({ endpoint: 'https://selftest.invalid/releases/latest' })
          .then(function (result) {
            updateTransport = null;
            var failures = [];
            if (result.status !== item.expect) failures.push('status=' + result.status);
            if (item.name !== 'control' && !el.updatePill.hidden) failures.push('显示了更新提示');
            if (item.version && (!result.release || result.release.version !== item.version)) {
              failures.push('version=' + (result.release && result.release.version));
            }
            // 正向对照还要证明比较方向对：确实比当前版本新，否则「永远不提示」也能过。
            if (item.name === 'control' &&
                compareVersion(result.release && result.release.version, '0.1.0') <= 0) {
              failures.push('比较方向不对');
            }
            results.push({
              case: item.name,
              expected: item.expect,
              actual: result.status,
              raw: String(result.raw || '').slice(0, 120),
              detail: result.detail || '',
              pill: el.updatePill.hidden ? '' : el.updatePill.textContent,
              ms: Date.now() - started,
              ok: failures.length === 0,
              failures: failures
            });
          }, function (error) {
            updateTransport = null;
            results.push({
              case: item.name,
              expected: item.expect,
              actual: 'threw',
              detail: String(error),
              ms: Date.now() - started,
              ok: false,
              failures: ['抛异常']
            });
          });
      });
    });
    window.__devcardUpdateSelftestResult = null;
    chain.then(function () {
      window.__devcardUpdateSelftestResult = results;
    }, function (error) {
      // 自检自身出错也要落地，否则壳层只能看到「没有结果」。
      results.push({
        case: 'selftest',
        actual: 'threw',
        detail: String((error && error.stack) || error),
        ok: false,
        failures: ['自检抛异常']
      });
      window.__devcardUpdateSelftestResult = results;
    });
    return 'started';
  };

export { MANDATORY_REASON_LABELS, UPDATE_DELAY_MS, UPDATE_LAST_KEY, UPDATE_MANUAL_TIMEOUT_MS, UPDATE_TIMEOUT_MS, applyLocalManifest, applyManualRemoteCheck, bridgeCheckNowResult, bridgeDownloadResult, bridgeInstallResult, checkUpdate, checkViaWinBridge, closeUpdateModal, compareVersion, dispatchWinUpdaterMessage, fetchRemoteManifest, handleUpdateDownloadPackage, handleUpdateInstall, installWinUpdaterListener, lastCheckSilent, loadInstallResult, makeWinUpdaterBridge, mandatoryActive, normalizeVersion, openUpdateModal, openUpdateUrl, parseAssets, parseRelease, parseRemoteManifest, pickUpdateAsset, pickUpdateSavePath, pollUpdateDownload, remoteCheck, renderUpdateAssets, renderUpdateInstallControls, renderUpdateMandatory, renderUpdateSummary, runSilentRemoteCheck, setUpdatePill, setUpdateProgress, startUpdateDownload, updateModalInfo, updateSteps, updateThrottleAllows, updateThrottleMark, updateTimeoutGuard, updateTransport, winUpdaterBridge, winUpdaterBridgeInstance, winUpdaterListenerInstalled };
