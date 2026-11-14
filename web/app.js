(function () {
  'use strict';

  var PAGE_SIZE = 20;

  var state = {
    page: 0,
    total: 0,
    totalAll: 0,
    items: [],
    filters: { keyword: '', field: '', name: '', location: '' },
    editingId: null,
    // 编辑/删除时带回的乐观锁版本号：服务端用它判断页面是不是过期了。
    editingVersion: 0,
    pendingDeleteId: null,
    pendingDeleteVersion: 0,
    user: null,
    version: '',
    schemaVersion: '',
    dataDir: '',
    // 打印字体能覆盖的码位区间 [[start,end], ...]，来自 /api/fonts/coverage，
    // 也就是随包字体 cmap 的真实内容，不是手写表。
    fontRanges: null,
    view: 'devices',
    users: [],
    backups: [],
    editingUser: null,
    resetUser: null,
    forcedPassword: false,
    printItems: [],
    printTruncated: false,
    printBusy: false,
    companyName: '',
    companyNamePrint: '',
    companyNameMaxWidth: 13,
    update: null,
    // 远端检查的原始结论（含接口原文），自检与核对用；
    // 这层出过「拿假数据截图」的事，解析结果必须能对回原文。
    updateCheck: null,
    updateUrl: ''
  };

  var el = {
    app: document.getElementById('app'),
    health: document.getElementById('health'),
    version: document.getElementById('version-pill'),
    updatePill: document.getElementById('update-pill'),
    updateModal: document.getElementById('update-modal'),
    updateVersion: document.getElementById('update-version'),
    updateNotes: document.getElementById('update-notes'),
    updateSteps: document.getElementById('update-steps'),
    updateDownload: document.getElementById('update-download'),
    updateDownloadPackage: document.getElementById('update-download-package'),
    updateProgress: document.getElementById('update-progress'),
    updateInstall: document.getElementById('update-install'),
    updateInstallResult: document.getElementById('update-install-result'),
    updateLater: document.getElementById('update-later'),
    userChip: document.getElementById('user-chip'),
    logout: document.getElementById('logout'),
    navUsers: document.getElementById('nav-users'),
    pageTitle: document.getElementById('page-title'),
    pageSub: document.getElementById('page-sub'),
    viewDevices: document.getElementById('view-devices'),
    viewUsers: document.getElementById('view-users'),
    legacyNotice: document.getElementById('legacy-notice'),

    filters: document.getElementById('filters'),
    keyword: document.getElementById('keyword'),
    field: document.getElementById('field'),
    name: document.getElementById('name'),
    location: document.getElementById('location'),
    reset: document.getElementById('reset'),
    rows: document.getElementById('rows'),
    empty: document.getElementById('empty'),
    total: document.getElementById('total'),
    pageLabel: document.getElementById('page-label'),
    prev: document.getElementById('prev'),
    next: document.getElementById('next'),
    add: document.getElementById('add'),
    importBtn: document.getElementById('import-btn'),
    exportBtn: document.getElementById('export-btn'),
    templateBtn: document.getElementById('template-btn'),
    importFile: document.getElementById('import-file'),

    scrim: document.getElementById('scrim'),
    drawer: document.getElementById('drawer'),
    drawerTitle: document.getElementById('drawer-title'),
    drawerClose: document.getElementById('drawer-close'),
    drawerCancel: document.getElementById('drawer-cancel'),
    form: document.getElementById('device-form'),
    formError: document.getElementById('form-error'),
    deviceId: document.getElementById('device-id'),
    devName: document.getElementById('dev-name'),
    devLocation: document.getElementById('dev-location'),
    devRange: document.getElementById('dev-range'),
    devPhone: document.getElementById('dev-phone'),
    modal: document.getElementById('modal'),
    modalText: document.getElementById('modal-text'),
    cancelDelete: document.getElementById('cancel-delete'),
    confirmDelete: document.getElementById('confirm-delete'),

    login: document.getElementById('login'),
    loginForm: document.getElementById('login-form'),
    loginUser: document.getElementById('login-user'),
    loginPassword: document.getElementById('login-password'),
    loginError: document.getElementById('login-error'),

    passwordModal: document.getElementById('password-modal'),
    passwordForm: document.getElementById('password-form'),
    passwordTitle: document.getElementById('password-title'),
    passwordHint: document.getElementById('password-hint'),
    oldPasswordRow: document.getElementById('old-password-row'),
    oldPassword: document.getElementById('old-password'),
    newPassword: document.getElementById('new-password'),
    newPassword2: document.getElementById('new-password-2'),
    passwordError: document.getElementById('password-error'),
    passwordCancel: document.getElementById('password-cancel'),

    userRows: document.getElementById('user-rows'),
    userEmpty: document.getElementById('user-empty'),
    addUser: document.getElementById('add-user'),
    changeMyPassword: document.getElementById('change-my-password'),
    userModal: document.getElementById('user-modal'),
    userForm: document.getElementById('user-form'),
    userModalTitle: document.getElementById('user-modal-title'),
    userName: document.getElementById('user-name'),
    userDisplay: document.getElementById('user-display'),
    userRole: document.getElementById('user-role'),
    userPasswordLabel: document.getElementById('user-password-label'),
    userPassword: document.getElementById('user-password'),
    userError: document.getElementById('user-error'),
    userCancel: document.getElementById('user-cancel'),

    resetModal: document.getElementById('reset-modal'),
    resetForm: document.getElementById('reset-form'),
    resetHint: document.getElementById('reset-hint'),
    resetPassword: document.getElementById('reset-password'),
    resetError: document.getElementById('reset-error'),
    resetCancel: document.getElementById('reset-cancel'),

    backupRows: document.getElementById('backup-rows'),
    backupEmpty: document.getElementById('backup-empty'),
    backupNow: document.getElementById('backup-now'),
    openDataDir: document.getElementById('open-data-dir'),

    gotoPrint: document.getElementById('goto-print'),
    saveCompany: document.getElementById('save-company'),
    companyName: document.getElementById('company-name'),
    companyPreview: document.getElementById('company-preview'),
    companyHint: document.getElementById('company-hint'),
    companyOver: document.getElementById('company-over'),
    companyFontWarning: document.getElementById('company-font-warning'),
    printFontWarning: document.getElementById('print-font-warning'),
    companyLogoPreview: document.getElementById('company-logo-preview'),
    pickLogo: document.getElementById('pick-logo'),
    resetLogo: document.getElementById('reset-logo'),
    logoFile: document.getElementById('logo-file'),
    logoTime: document.getElementById('logo-time'),
    viewPrint: document.getElementById('view-print'),
    printScope: document.getElementById('print-scope'),
    printHint: document.getElementById('print-hint'),
    printLayout: document.getElementById('print-layout'),
    printBack: document.getElementById('print-back'),
    printExport: document.getElementById('print-export'),
    printPrint: document.getElementById('print-print'),
    printStage: document.getElementById('print-stage'),
    printEmpty: document.getElementById('print-empty'),

    statusbar: document.getElementById('statusbar'),
    statusUser: document.getElementById('status-user'),
    statusMode: document.getElementById('status-mode'),
    statusCount: document.getElementById('status-count'),
    statusVersion: document.getElementById('status-version'),
    statusSchema: document.getElementById('status-schema'),
    statusDatadir: document.getElementById('status-datadir'),
    statusError: document.getElementById('status-error'),
    statusRetry: document.getElementById('status-retry'),

    toast: document.getElementById('toast')
  };

  function escapeHtml(value) {
    return String(value == null ? '' : value)
      .replace(/&/g, '&amp;')
      .replace(/</g, '&lt;')
      .replace(/>/g, '&gt;')
      .replace(/"/g, '&quot;')
      .replace(/'/g, '&#39;');
  }

  function api(method, url, body) {
    var options = {
      method: method,
      headers: { Accept: 'application/json', 'X-Requested-With': 'DevCard' }
    };
    if (body !== undefined) {
      options.headers['Content-Type'] = 'application/json';
      options.body = JSON.stringify(body);
    }
    return fetch(url, options).then(function (response) {
      return response.text().then(function (text) {
        var data = null;
        if (text) {
          try {
            data = JSON.parse(text);
          } catch (error) {
            data = { message: text };
          }
        }
        if (!response.ok) {
          var failure = new Error((data && data.message) || ('请求失败（' + response.status + '）'));
          failure.code = data && data.error;
          failure.status = response.status;
          if (response.status >= 500) setStatusError(failure.message);
          throw failure;
        }
        setStatusOk();
        return data;
      });
    }, function (error) {
      // fetch 直接拒绝 = 本地服务不可达
      setStatusError('无法连接本地服务');
      throw error;
    });
  }

  function handleAuthFailure(error) {
    if (!error) return false;
    if (error.status === 401) {
      showLogin();
      return true;
    }
    if (error.status === 403 && error.code === 'must_change_password') {
      openPasswordModal(true);
      return true;
    }
    return false;
  }

  var toastTimer = null;
  function toast(message, kind) {
    el.toast.textContent = message;
    el.toast.classList.toggle('is-error', kind === 'error');
    el.toast.hidden = false;
    if (toastTimer) clearTimeout(toastTimer);
    toastTimer = setTimeout(function () {
      el.toast.hidden = true;
    }, 2600);
  }

  // ---------- 底部状态栏（M4.2） ----------

  function updateStatusIdentity() {
    if (!state.user) return;
    el.statusUser.textContent = state.user.display_name || state.user.user_name;
    // 有原生桥 = 桌面壳（单机版）；纯浏览器访问 = 网络版。
    el.statusMode.textContent = hasNativeBridge() ? '单机版' : '网络版';
    el.statusVersion.textContent = 'v' + (state.version || '—');
    el.statusSchema.textContent = 'schema ' + (state.schemaVersion || '—');
    el.statusDatadir.textContent = state.dataDir || '—';
  }

  function filtersActive() {
    var f = state.filters;
    return !!(f.keyword || f.field || f.name || f.location);
  }

  // 条数与页脚同源：都用 state.total，不另算一次。
  function updateStatusCount() {
    el.statusCount.textContent = filtersActive()
      ? '筛选后 ' + state.total + ' 条 / 共 ' + state.totalAll + ' 条'
      : '共 ' + state.total + ' 条';
  }

  function setStatusError(message) {
    el.statusbar.classList.add('is-error');
    el.statusError.textContent = '服务连接异常：' + (message || '请检查本地服务是否仍在运行');
    el.statusError.hidden = false;
    el.statusRetry.hidden = false;
    // 不保留上一次的陈旧数字
    el.statusCount.textContent = '—';
    el.statusVersion.textContent = '—';
    el.statusSchema.textContent = '—';
    el.statusDatadir.textContent = '—';
  }

  function setStatusOk() {
    if (!el.statusbar.classList.contains('is-error')) return;
    el.statusbar.classList.remove('is-error');
    el.statusError.hidden = true;
    el.statusRetry.hidden = true;
    updateStatusIdentity();
    updateStatusCount();
  }

  el.statusRetry.addEventListener('click', function () {
    loadBootstrap()
      .then(function () {
        return load();
      })
      .catch(function (error) {
        if (!handleAuthFailure(error)) setStatusError(error.message);
      });
  });

  el.statusDatadir.addEventListener('click', function () {
    if (!state.dataDir) return;
    if (sendToShell('openDataDir', { path: state.dataDir })) return;
    if (navigator.clipboard && navigator.clipboard.writeText) {
      navigator.clipboard.writeText(state.dataDir).then(function () {
        toast('数据目录路径已复制：' + state.dataDir);
      });
    } else {
      toast('数据目录：' + state.dataDir);
    }
  });

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
  var UPDATE_TIMEOUT_MS = 3000;  // 内网客户连不上外网是常态：失败必须秒级返回
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
  // 或自管清单（latest/download_url/notes）。版本号不合法就整条不认。
  function parseRelease(doc) {
    if (!doc || typeof doc !== 'object' || Array.isArray(doc)) return null;
    var version = normalizeVersion(doc.tag_name || doc.latest || doc.version || '');
    if (!version) return null;
    var url = doc.html_url || doc.download_url || '';
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
        sha256: match ? match[1].toLowerCase() : ''
      });
    });
    return assets;
  }

  function pickUpdateAsset(assets) {
    var prefer = /Windows/i.test(navigator.userAgent) ? ['.zip', '.exe'] : ['.dmg'];
    for (var i = 0; i < prefer.length; i++) {
      for (var j = 0; j < assets.length; j++) {
        var name = String(assets[j].name || '').toLowerCase();
        if (name.slice(-prefer[i].length) === prefer[i]) return assets[j];
      }
    }
    return null;
  }

  function formatBytes(value) {
    var bytes = Number(value) || 0;
    if (bytes >= 1024 * 1024) return (bytes / (1024 * 1024)).toFixed(1) + ' MB';
    if (bytes >= 1024) return (bytes / 1024).toFixed(0) + ' KB';
    return bytes + ' B';
  }

  // 一次远端检查。永远 resolve，不抛：失败路径要和「没有新版本」在界面上不可区分。
  function remoteCheck(config) {
    var endpoint = String((config && config.endpoint) || '');
    if (!/^https?:\/\//i.test(endpoint)) {
      return Promise.resolve({ status: 'disabled', endpoint: endpoint, raw: '', release: null });
    }
    var controller = typeof AbortController === 'function' ? new AbortController() : null;
    // 超时不能只靠 abort：没有 AbortController（或传输层不认 signal）时，
    // 那个请求会一直挂着，界面永远等不到结论。所以超时自己是竞速的一方。
    var timer = null;
    var timeout = new Promise(function (resolve) {
      timer = setTimeout(function () {
        if (controller) {
          try { controller.abort(); } catch (error) { /* 已经结束就无所谓 */ }
        }
        resolve({ timedOut: true });
      }, UPDATE_TIMEOUT_MS);
    });
    var options = {
      method: 'GET',
      cache: 'no-store',
      headers: { Accept: 'application/vnd.github+json' }
    };
    if (controller) options.signal = controller.signal;
    var transport = updateTransport || function (url, opts) { return fetch(url, opts); };
    var request = transport(endpoint, options)
      .then(function (response) {
        if (!response || !response.ok) {
          throw new Error('HTTP ' + (response ? response.status : '?'));
        }
        return response.text();
      })
      .then(function (text) {
        var doc = null;
        try {
          doc = JSON.parse(text);
        } catch (error) {
          doc = null;
        }
        var release = parseRelease(doc);
        return {
          status: release ? 'parsed' : (doc ? 'invalid-version' : 'invalid-json'),
          endpoint: endpoint,
          raw: String(text || ''),
          release: release
        };
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
    return Promise.race([request, timeout]).then(function (result) {
      clearTimeout(timer);
      if (result && result.timedOut) {
        return {
          status: 'timeout',
          endpoint: endpoint,
          raw: '',
          release: null,
          detail: '超过 ' + UPDATE_TIMEOUT_MS + ' ms 没有响应'
        };
      }
      return result;
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

  function checkUpdate() {
    return api('GET', '/api/version')
      .then(function (data) {
        state.update = data || null;
        // 本地清单（M5 路径）仍然优先：运维可以直接投放 update.json 指定版本。
        if (data && data.update_available && data.latest) {
          setUpdatePill(data.latest, data.notes || '');
        }
        var check = (data && data.check) || {};
        if (!check.enabled) return;
        var hours = Number(check.interval_hours) > 0 ? Number(check.interval_hours) : 24;
        if (!updateThrottleAllows(hours)) return;
        // 延迟到首屏之后；查不到更新、超时、DNS 失败、返回不是 JSON、
        // 版本号不合法，一律什么都不做——不弹窗、不改任何业务功能。
        return new Promise(function (resolve) {
          setTimeout(resolve, UPDATE_DELAY_MS);
        }).then(function () {
          return remoteCheck(check);
        }).then(function (result) {
          state.updateCheck = result;
          updateThrottleMark();
          if (result.status !== 'parsed' || !result.release) return;
          var current = state.version || (data && data.version) || '';
          if (compareVersion(result.release.version, current) <= 0) return;
          setUpdatePill(result.release.version, result.release.notes || '');
        });
      })
      .catch(function () {
        // 检查失败不打扰台账操作，也不保留上一次的提示。
        state.update = null;
        if (!state.updateCheck) el.updatePill.hidden = true;
      });
  }

  // 手动更新的步骤要按平台写清楚：只说「有新版本」等于没说。
  function updateSteps() {
    var ua = navigator.userAgent || '';
    if (/Macintosh|Mac OS X/i.test(ua)) {
      return [
        '点「下载更新包」把 DMG 下载到数据目录并校验（只下载，不安装）；也可打开下载页手动下载。',
        '双击打开 DMG，把 DevCard 拖进「应用程序」，选「替换」覆盖旧版本。',
        '退出并重新打开 DevCard。台账数据在用户数据目录，覆盖安装不会动它。'
      ];
    }
    if (/Windows/i.test(ua)) {
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

  function openUpdateModal() {
    var manifest = state.update || {};
    var result = state.updateCheck || null;
    var release = result && result.release ? result.release : null;
    var latest = release ? release.version : (manifest.latest || '');
    var url = (release && release.url) || manifest.download_url ||
      manifest.download_page || '';
    state.updateUrl = /^https?:\/\//i.test(url) ? url : '';
    el.updateVersion.textContent = '当前版本 v' + (state.version || '?') +
      (latest ? '，最新版本 v' + latest : '');
    var notes = (release && release.notes) || manifest.notes || '';
    el.updateNotes.textContent = notes;
    el.updateNotes.hidden = !notes;
    el.updateSteps.innerHTML = '';
    updateSteps().forEach(function (text) {
      var item = document.createElement('li');
      item.textContent = text;
      el.updateSteps.appendChild(item);
    });
    el.updateDownload.disabled = !state.updateUrl;

    // 阶段一：只下载 + 校验。按钮是否可用取决于清单里有没有「带 SHA256 与
    // 字节数」的当前平台资产；缺任一条件就退回只提示，不给下载入口。
    var assets = (release && release.assets) || [];
    state.updateAsset = pickUpdateAsset(assets);
    state.updateAssetVersion = release ? release.version : '';
    state.updateDownloading = false;
    if (state.updateTimer) {
      clearTimeout(state.updateTimer);
      state.updateTimer = null;
    }
    var asset = state.updateAsset;
    var isAdmin = state.user && state.user.role === 'admin';
    el.updateDownloadPackage.disabled =
      !(asset && asset.sha256 && asset.size && isAdmin);
    if (!asset) {
      setUpdateProgress('发布清单里没有当前平台的安装包，请用下载页手动更新。');
    } else if (!asset.sha256 || !asset.size) {
      setUpdateProgress('发布清单缺少 SHA256 或字节数，无法校验，已退回只提示；请用下载页手动更新。');
    } else if (!isAdmin) {
      setUpdateProgress('只有管理员可以下载更新包，请用下载页手动更新。');
    } else {
      setUpdateProgress('');
    }
    // 阶段二：只有 Windows 安装版的管理员能看到「安装并重启」。
    // 按钮先禁用，下载并通过校验后才放开。
    var isWindows = /Windows/i.test(navigator.userAgent || '');
    el.updateInstall.hidden = !(isWindows && isAdmin);
    el.updateInstall.disabled = true;
    state.updateInstalling = false;
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
    if (!state.user || state.user.role !== 'admin') return;
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

  el.updateInstall.addEventListener('click', function () {
    if (state.updateInstalling) return;
    state.updateInstalling = true;
    el.updateInstall.disabled = true;
    setUpdateProgress('正在启动安装程序，服务即将停止…');
    api('POST', '/api/update/install', {}).then(function () {
      setUpdateProgress('安装程序已启动：服务正在停止，装完会自动打开新版本。');
    }).catch(function (error) {
      state.updateInstalling = false;
      el.updateInstall.disabled = false;
      setUpdateProgress('安装未启动：' + error.message, true);
    });
  });

  function closeUpdateModal() {
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

  el.updateLater.addEventListener('click', closeUpdateModal);

  el.updateDownload.addEventListener('click', function () {
    if (!state.updateUrl) {
      toast('未配置下载页地址', 'error');
      return;
    }
    openUpdateUrl(state.updateUrl);
    closeUpdateModal();
  });

  el.updateDownloadPackage.addEventListener('click', function () {
    var asset = state.updateAsset;
    if (!asset || !asset.sha256 || !asset.size || state.updateDownloading) return;
    state.updateDownloading = true;
    el.updateDownloadPackage.disabled = true;
    setUpdateProgress('正在准备下载…');
    api('POST', '/api/update/download', {
      version: state.updateAssetVersion,
      file_name: asset.name,
      url: asset.url,
      sha256: asset.sha256,
      size: asset.size
    }).then(function () {
      pollUpdateDownload();
    }).catch(function (error) {
      state.updateDownloading = false;
      el.updateDownloadPackage.disabled = false;
      setUpdateProgress('下载未开始：' + error.message +
        '（可用下载页手动更新）', true);
    });
  });

  // 轮询本地服务拿进度；失败一律回到「只提示」，不禁用台账，也不自动替换。
  function pollUpdateDownload() {
    if (state.updateTimer) clearTimeout(state.updateTimer);
    state.updateTimer = setTimeout(function () {
      api('GET', '/api/update/download').then(function (data) {
        if (data.state === 'done') {
          state.updateDownloading = false;
          state.updateTimer = null;
          el.updateDownloadPackage.disabled = true;
          if (!el.updateInstall.hidden) el.updateInstall.disabled = false;
          setUpdateProgress('已下载并通过 SHA256 + 字节数校验：' + data.path);
          toast('更新包已下载并通过校验');
          return;
        }
        if (data.state === 'failed') {
          state.updateDownloading = false;
          state.updateTimer = null;
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
        state.updateDownloading = false;
        state.updateTimer = null;
        el.updateDownloadPackage.disabled = false;
        setUpdateProgress('无法获取下载进度：' + error.message +
          '（可用下载页手动更新）', true);
      });
    }, 800);
  }

  // ---------- 会话 ----------

  function showLogin(message) {
    el.app.hidden = true;
    el.login.hidden = false;
    el.userChip.hidden = true;
    el.logout.hidden = true;
    if (message) {
      el.loginError.textContent = message;
      el.loginError.hidden = false;
    } else {
      el.loginError.hidden = true;
    }
    el.loginUser.focus();
  }

  function applyBootstrap(data) {
    state.user = data.user;
    el.app.hidden = false;
    el.login.hidden = true;
    el.userChip.hidden = false;
    el.logout.hidden = false;
    el.userChip.textContent =
      data.user.user_name + (data.user.role === 'admin' ? ' · 管理员' : ' · 用户');
    el.navUsers.hidden = data.user.role !== 'admin';
    el.version.textContent = 'v' + data.version;
    el.health.textContent = '服务正常 · schema ' + data.schema_version;
    state.version = data.version || '';
    state.schemaVersion = data.schema_version || '';
    state.dataDir = data.data_dir || '';
    updateStatusIdentity();
    state.companyName = data.company_name || '';
    state.companyNamePrint = data.company_name_print || state.companyName;
    state.companyNameMaxWidth = data.company_name_max_width || 13;
    el.companyName.value = state.companyName;
    el.companyLogoPreview.src = data.company_logo_url || '/company-logo';
    updateCompanyHint();
    updateLogoTime(data.company_logo_updated_at);
    renderLegacyNotice(data.legacy);
    if (data.user.must_change_password) {
      openPasswordModal(true);
    } else {
      enterApp();
    }
  }

  function loadBootstrap() {
    return api('GET', '/api/bootstrap').then(applyBootstrap);
  }

  function tryLocalSession() {
    var meta = document.querySelector('meta[name="devcard-local-token"]');
    var token = meta ? meta.getAttribute('content') : '';
    if (!token) {
      showLogin();
      return Promise.resolve();
    }
    return api('POST', '/api/session/local', { token: token })
      .then(loadBootstrap)
      .catch(function () {
        showLogin();
      });
  }

  function enterApp() {
    loadOptions();
    loadFontCoverage();
    load();
    checkUpdate();
    if (state.view === 'users') {
      loadUsers();
      loadBackups();
    }
    var params = new URLSearchParams(window.location.search);
    if (params.get('view') === 'print') setView('print');
  }

  el.loginForm.addEventListener('submit', function (event) {
    event.preventDefault();
    el.loginError.hidden = true;
    api('POST', '/api/session/login', {
      username: el.loginUser.value.trim(),
      password: el.loginPassword.value
    })
      .then(function () {
        el.loginPassword.value = '';
        return loadBootstrap();
      })
      .catch(function (error) {
        el.loginError.textContent = error.message;
        el.loginError.hidden = false;
      });
  });

  el.logout.addEventListener('click', function () {
    api('POST', '/api/session/logout')
      .then(function () {
        state.user = null;
        showLogin();
      })
      .catch(function (error) {
        toast(error.message, 'error');
      });
  });

  // ---------- 强制改密 ----------

  function openPasswordModal(forced) {
    state.forcedPassword = forced;
    el.passwordTitle.textContent = forced ? '首次登录必须修改密码' : '修改密码';
    el.passwordHint.textContent = forced
      ? '当前账号仍使用初始或重置密码，直接设置新密码即可进入业务页，不需要原密码。'
      : '修改当前账号的密码，需要先输入原密码。';
    el.passwordCancel.hidden = forced;
    // 是否要求原密码由服务端按 must_change_password 判定；这里只同步界面，
    // 隐藏时必须同时解除 required，否则空的原密码框会拦住表单提交。
    el.oldPasswordRow.hidden = forced;
    el.oldPassword.required = !forced;
    el.oldPassword.value = '';
    el.newPassword.value = '';
    el.newPassword2.value = '';
    el.passwordError.hidden = true;
    el.passwordModal.hidden = false;
    (forced ? el.newPassword : el.oldPassword).focus();
  }

  el.changeMyPassword.addEventListener('click', function () {
    openPasswordModal(false);
  });

  el.passwordCancel.addEventListener('click', function () {
    if (state.forcedPassword) return;
    el.passwordModal.hidden = true;
  });

  el.passwordForm.addEventListener('submit', function (event) {
    event.preventDefault();
    el.passwordError.hidden = true;
    if (el.newPassword.value !== el.newPassword2.value) {
      el.passwordError.textContent = '两次输入的新密码不一致';
      el.passwordError.hidden = false;
      return;
    }
    api('POST', '/api/users/password', state.forcedPassword
      ? { new_password: el.newPassword.value }
      : { old_password: el.oldPassword.value, new_password: el.newPassword.value })
      .then(function () {
        var forced = state.forcedPassword;
        state.forcedPassword = false;
        state.user.must_change_password = false;
        el.passwordModal.hidden = true;
        toast('密码已更新');
        if (forced) enterApp();
      })
      .catch(function (error) {
        if (handleAuthFailure(error)) return;
        el.passwordError.textContent = error.message;
        el.passwordError.hidden = false;
      });
  });

  // ---------- 视图切换 ----------

  function setView(view) {
    state.view = view;
    Array.prototype.forEach.call(document.querySelectorAll('.nav-item[data-view]'), function (button) {
      button.classList.toggle('is-active', button.getAttribute('data-view') === view);
    });
    el.app.classList.toggle('is-print', view === 'print');
    el.viewDevices.hidden = view !== 'devices';
    el.viewUsers.hidden = view !== 'users';
    el.viewPrint.hidden = view !== 'print';
    if (view === 'devices') {
      el.pageTitle.textContent = '设备台账';
      el.pageSub.textContent = '维护设备名称、位置、控制范围与维护电话';
    } else if (view === 'users') {
      el.pageTitle.textContent = '用户管理';
      el.pageSub.textContent = '账号维护与数据备份恢复';
      loadUsers();
      loadBackups();
    } else {
      el.pageTitle.textContent = '卡片打印';
      el.pageSub.textContent = 'A4 版式预览、导出 PDF 与直接打印';
      loadPrint();
    }
  }

  document.querySelector('.nav').addEventListener('click', function (event) {
    var button = event.target.closest('button[data-view]');
    if (!button || button.disabled) return;
    setView(button.getAttribute('data-view'));
  });

  function renderLegacyNotice(legacy) {
    if (!legacy) {
      el.legacyNotice.hidden = true;
      return;
    }
    var parts = [
      '已从旧库导入 ' + legacy.devices_imported + ' 条设备、' +
        legacy.users_imported + ' 个账号。',
      '原库备份：' + legacy.backup + '。'
    ];
    if (legacy.archived_as) parts.push('旧库已归档为 ' + legacy.archived_as + '。');
    if (legacy.warnings && legacy.warnings.length) {
      parts.push('共 ' + legacy.warnings.length + ' 条告警（超长截断、空字段跳过等）。');
    }
    el.legacyNotice.textContent = parts.join(' ');
    el.legacyNotice.hidden = false;
  }

  // ---------- 设备台账 ----------

  function totalPages() {
    return Math.max(1, Math.ceil(state.total / PAGE_SIZE));
  }

  function renderRows() {
    var html = state.items
      .map(function (item) {
        return (
          '<tr>' +
          '<td class="col-id">' + item.id + '</td>' +
          '<td>' + escapeHtml(item.dev_name) + '</td>' +
          '<td>' + escapeHtml(item.location) + '</td>' +
          '<td>' + escapeHtml(item.control_range) + '</td>' +
          '<td>' + escapeHtml(item.phone) + '</td>' +
          '<td class="col-actions">' +
          '<button class="row-btn" type="button" data-action="edit" data-id="' + item.id + '">编辑</button>' +
          '<button class="row-btn danger" type="button" data-action="delete" data-id="' + item.id + '">删除</button>' +
          '</td>' +
          '</tr>'
        );
      })
      .join('');
    el.rows.innerHTML = html;
    el.empty.hidden = state.items.length > 0;
  }

  function renderPager() {
    el.total.textContent = String(state.total);
    el.pageLabel.textContent = '第 ' + (state.page + 1) + ' / ' + totalPages() + ' 页';
    el.prev.disabled = state.page <= 0;
    el.next.disabled = state.page + 1 >= totalPages();
    updateStatusCount();
  }

  function queryString() {
    var params = new URLSearchParams();
    var f = state.filters;
    if (f.keyword) params.set('keyword', f.keyword);
    if (f.field) params.set('field', f.field);
    if (f.name) params.set('name', f.name);
    if (f.location) params.set('location', f.location);
    params.set('page', String(state.page));
    params.set('page_size', String(PAGE_SIZE));
    return params.toString();
  }

  function load() {
    return api('GET', '/api/devices?' + queryString())
      .then(function (data) {
        state.items = data.items || [];
        state.total = data.total || 0;
        state.totalAll = data.total_all == null ? state.total : data.total_all;
        state.page = data.page || 0;
        if (state.items.length === 0 && state.page > 0 && state.total > 0) {
          state.page = Math.max(0, totalPages() - 1);
          return load();
        }
        renderRows();
        renderPager();
      })
      .catch(function (error) {
        if (handleAuthFailure(error)) return;
        toast(error.message, 'error');
      });
  }

  function fillSelect(select, values) {
    var current = select.value;
    var head = select.options.length > 0 ? select.options[0].outerHTML : '<option value="">全部</option>';
    select.innerHTML = head;
    values.forEach(function (value) {
      var option = document.createElement('option');
      option.value = value;
      option.textContent = value;
      select.appendChild(option);
    });
    if (values.indexOf(current) >= 0) select.value = current;
  }

  function loadOptions() {
    return api('GET', '/api/devices/options')
      .then(function (data) {
        fillSelect(el.name, data.names || []);
        fillSelect(el.location, data.locations || []);
      })
      .catch(function (error) {
        if (handleAuthFailure(error)) return;
        toast('筛选项加载失败：' + error.message, 'error');
      });
  }

  function openDrawer(device) {
    state.editingId = device ? device.id : null;
    state.editingVersion = device ? device.version : 0;
    el.drawerTitle.textContent = device ? '编辑设备' : '新增设备';
    el.deviceId.value = device ? String(device.id) : '';
    el.devName.value = device ? device.dev_name : '';
    el.devLocation.value = device ? device.location : '';
    el.devRange.value = device ? device.control_range : '';
    el.devPhone.value = device ? device.phone : '';
    el.formError.hidden = true;
    el.formError.textContent = '';
    el.drawer.classList.add('is-open');
    el.drawer.setAttribute('aria-hidden', 'false');
    el.scrim.hidden = false;
    el.devName.focus();
  }

  function closeDrawer() {
    el.drawer.classList.remove('is-open');
    el.drawer.setAttribute('aria-hidden', 'true');
    el.scrim.hidden = true;
    state.editingId = null;
    state.editingVersion = 0;
  }

  function openDelete(device) {
    state.pendingDeleteId = device.id;
    state.pendingDeleteVersion = device.version;
    el.modalText.textContent =
      '确定删除「' + device.dev_name + '」·' + device.location + '？删除后不可恢复。';
    el.modal.hidden = false;
  }

  function closeDelete() {
    state.pendingDeleteId = null;
    state.pendingDeleteVersion = 0;
    el.modal.hidden = true;
  }

  function syncFiltersFromInputs() {
    state.filters.keyword = el.keyword.value.trim();
    state.filters.field = el.field.value;
    state.filters.name = el.name.value;
    state.filters.location = el.location.value;
  }

  el.filters.addEventListener('submit', function (event) {
    event.preventDefault();
    syncFiltersFromInputs();
    state.page = 0;
    load();
  });

  el.reset.addEventListener('click', function () {
    el.keyword.value = '';
    el.field.value = '';
    el.name.value = '';
    el.location.value = '';
    state.filters = { keyword: '', field: '', name: '', location: '' };
    state.page = 0;
    load();
  });

  el.prev.addEventListener('click', function () {
    if (state.page <= 0) return;
    state.page -= 1;
    load();
  });

  el.next.addEventListener('click', function () {
    if (state.page + 1 >= totalPages()) return;
    state.page += 1;
    load();
  });

  el.add.addEventListener('click', function () {
    openDrawer(null);
  });

  el.importBtn.addEventListener('click', function () {
    el.importFile.value = '';
    el.importFile.click();
  });

  el.importFile.addEventListener('change', function () {
    var file = el.importFile.files && el.importFile.files[0];
    if (!file) return;
    el.importBtn.disabled = true;
    fetch('/api/devices/import', {
      method: 'POST',
      headers: {
        'Content-Type': 'application/octet-stream',
        'X-Requested-With': 'DevCard'
      },
      body: file
    })
      .then(function (response) {
        return response.text().then(function (text) {
          var data = {};
          try {
            data = text ? JSON.parse(text) : {};
          } catch (error) {
            data = { message: text };
          }
          if (!response.ok) {
            var failure = new Error(data.message || '导入失败');
            failure.status = response.status;
            failure.code = data.error;
            throw failure;
          }
          return data;
        });
      })
      .then(function (report) {
        var parts = ['导入成功 ' + report.imported + ' 条'];
        if (report.skipped_empty) parts.push('跳过空行 ' + report.skipped_empty);
        if (report.duplicates) parts.push('与现有记录重复 ' + report.duplicates + ' 条（已保留）');
        if (report.truncated) parts.push('有 ' + report.truncated + ' 行字段被截断');
        toast(parts.join('，'));
        return loadOptions().then(load);
      })
      .catch(function (error) {
        if (handleAuthFailure(error)) return;
        toast(error.message, 'error');
      })
      .then(function () {
        el.importBtn.disabled = false;
      });
  });

  // ---------- 打印设置：公司名与图标 ----------

  // 显示宽度：汉字算 1，ASCII 字母/数字/符号算 0.5。
  // 字体覆盖检查：区间来自服务端对随包字体 cmap 的解析结果。
  // 用途是「知情」而不是拦截——名字是真实的，拒绝保存只会逼用户去删字。
  function loadFontCoverage() {
    return api('GET', '/api/fonts/coverage').then(function (data) {
      state.fontRanges = (data && data.available && data.ranges) ? data.ranges : null;
      // 字体的字节数与 SHA256 由构建期从 web/fonts/ 读出、随接口下发，前端不再写死。
      printFontSpec = (data && data.fonts && data.fonts.length) ? data.fonts : null;
    }).catch(function () {
      state.fontRanges = null;
      printFontSpec = null;
    });
  }

  function fontCovers(codepoint) {
    var ranges = state.fontRanges;
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
    Array.from(value).forEach(function (ch) {
      var w = ch.codePointAt(0) < 0x80 ? 0.5 : 1;
      if (width + w > state.companyNameMaxWidth + 1e-9) return;
      width += w;
      out += ch;
    });
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

  function updateCompanyHint() {
    var value = el.companyName.value;
    var width = companyNameWidth(value);
    var over = width > state.companyNameMaxWidth + 1e-9;
    // 录入放宽到 16 个字符，卡面仍按 13 字宽打印：把截断提前展示成用户看得见的
    // 结果，而不是保存后再解释。
    el.companyPreview.textContent = value.trim()
      ? '将打印为：「' + truncateCompanyName(value) + '」' +
        (over ? '（卡面按 ' + state.companyNameMaxWidth + ' 字宽打印，只保留前 ' +
                state.companyNameMaxWidth + ' 字宽）' : '')
      : '将打印为：默认公司名「深圳市安业物业管理有限公司」';
    el.companyHint.textContent = '已用 ' + width + ' / ' + state.companyNameMaxWidth +
      '（汉字算 1，字母数字算 0.5），最多录入 16 个字符';
    el.companyHint.classList.toggle('is-over', over);
    el.companyOver.hidden = !over;
    // 字体覆盖是另一件事：字放得下（宽度上限），但打印子集里没有这个字。
    // 口径见 companyFontCheck 上方：字少且塞得进就告知，塞不进就阻止导出。
    var fontCheck = companyFontCheck(value);
    el.companyFontWarning.hidden = fontCheck.missing.length === 0;
    if (fontCheck.missing.length) {
      el.companyFontWarning.textContent = fontCheck.overflow
        ? '这些字不在打印字体范围内：' + fontCheck.missing.join('、') +
          '。它们会由系统字体渲染且实测超出公司名框，导出会被阻止；请换字或扩展字体子集。'
        : '这些字不在打印字体范围内：' + fontCheck.missing.join('、') +
          '。它们会由系统字体渲染（已逐个列出），实测仍能放进公司名框，可以正常导出。';
    }
  }

  function updateLogoTime(value) {
    el.logoTime.textContent = value ? '图标更新时间：' + value
                                    : '当前使用内置默认图标';
  }

  el.companyName.addEventListener('input', updateCompanyHint);

  el.saveCompany.addEventListener('click', function () {
    el.saveCompany.disabled = true;
    api('PUT', '/api/system/company', { company_name: el.companyName.value.trim() })
      .then(function (data) {
        state.companyName = data.company_name;
        state.companyNamePrint = data.company_name_print;
        state.companyNameMaxWidth = data.company_name_max_width || 13;
        el.companyName.value = data.company_name;
        updateCompanyHint();
        if (state.view === 'print') renderPrint();
        toast(data.company_name_width > state.companyNameMaxWidth
          ? '已保存；卡面只打印前 ' + state.companyNameMaxWidth + ' 字宽，实际内容见预览'
          : '公司名已保存');
      })
      .catch(function (error) {
        if (handleAuthFailure(error)) return;
        toast(error.message, 'error');
      })
      .then(function () {
        el.saveCompany.disabled = false;
      });
  });

  el.pickLogo.addEventListener('click', function () {
    el.logoFile.click();
  });

  el.logoFile.addEventListener('change', function () {
    var file = el.logoFile.files && el.logoFile.files[0];
    el.logoFile.value = '';
    if (!file) return;
    if (file.size > 2 * 1024 * 1024) {
      toast('图标不能超过 2 MB', 'error');
      return;
    }
    if (file.type !== 'image/png' && file.type !== 'image/jpeg') {
      toast('只接受 PNG 或 JPEG 图片（不支持 SVG）', 'error');
      return;
    }
    fetch('/api/system/company-logo', {
      method: 'POST',
      headers: { 'Content-Type': file.type, 'X-Requested-With': 'DevCard' },
      body: file
    })
      .then(function (response) {
        return response.json().then(function (body) {
          if (!response.ok) throw new Error(body.message || '上传失败');
          return body;
        });
      })
      .then(function (data) {
        el.companyLogoPreview.src = data.logo_url + '?t=' + Date.now();
        updateLogoTime(data.updated_at);
        toast(data.message || '图标已更新');
      })
      .catch(function (error) {
        toast(error.message, 'error');
      });
  });

  el.resetLogo.addEventListener('click', function () {
    api('DELETE', '/api/system/company-logo')
      .then(function () {
        el.companyLogoPreview.src = '/company-logo?t=' + Date.now();
        updateLogoTime('');
        toast('已恢复默认图标');
      })
      .catch(function (error) {
        if (handleAuthFailure(error)) return;
        toast(error.message, 'error');
      });
  });

  el.exportBtn.addEventListener('click', function () {
    el.exportBtn.disabled = true;
    fetch('/api/devices/export')
      .then(function (response) {
        if (!response.ok) throw new Error('导出失败（' + response.status + '）');
        return response.blob();
      })
      .then(function (blob) {
        var url = URL.createObjectURL(blob);
        var link = document.createElement('a');
        link.href = url;
        link.download = '设备台账.xlsx';
        document.body.appendChild(link);
        link.click();
        document.body.removeChild(link);
        setTimeout(function () { URL.revokeObjectURL(url); }, 1000);
        toast('已导出 设备台账.xlsx');
      })
      .catch(function (error) {
        toast(error.message, 'error');
      })
      .then(function () {
        el.exportBtn.disabled = false;
      });
  });

  el.templateBtn.addEventListener('click', function () {
    el.templateBtn.disabled = true;
    fetch('/api/devices/template')
      .then(function (response) {
        if (!response.ok) throw new Error('下载模板失败（' + response.status + '）');
        return response.blob();
      })
      .then(function (blob) {
        var url = URL.createObjectURL(blob);
        var link = document.createElement('a');
        link.href = url;
        link.download = '设备导入模板.xlsx';
        document.body.appendChild(link);
        link.click();
        document.body.removeChild(link);
        setTimeout(function () { URL.revokeObjectURL(url); }, 1000);
        toast('已下载 设备导入模板.xlsx，可直接上传（空模板导入 0 行）');
      })
      .catch(function (error) {
        toast(error.message, 'error');
      })
      .then(function () {
        el.templateBtn.disabled = false;
      });
  });

  el.drawerClose.addEventListener('click', closeDrawer);
  el.drawerCancel.addEventListener('click', closeDrawer);
  el.scrim.addEventListener('click', closeDrawer);

  el.rows.addEventListener('click', function (event) {
    var button = event.target.closest('button[data-action]');
    if (!button) return;
    var id = Number(button.getAttribute('data-id'));
    var device = state.items.filter(function (item) { return item.id === id; })[0];
    if (!device) return;
    if (button.getAttribute('data-action') === 'edit') {
      openDrawer(device);
    } else {
      openDelete(device);
    }
  });

  el.form.addEventListener('submit', function (event) {
    event.preventDefault();
    var payload = {
      dev_name: el.devName.value.trim(),
      location: el.devLocation.value.trim(),
      control_range: el.devRange.value.trim(),
      phone: el.devPhone.value.trim()
    };
    var editingId = state.editingId;
    // 乐观锁：编辑必须带回打开抽屉时的版本号，服务端版本不一致会返回 409。
    if (editingId) payload.version = state.editingVersion;
    // 生僻字只提示、不拦截：设备名是真实的，拒绝保存只会让用户回去删字。
    var missing = uncoveredChars(payload.dev_name + payload.location + payload.control_range);
    var request = editingId
      ? api('PUT', '/api/devices/' + editingId, payload)
      : api('POST', '/api/devices', payload);

    request
      .then(function () {
        closeDrawer();
        toast(editingId ? '已保存修改' : '已新增设备');
        if (missing.length) {
          toast('这些字不在打印字体范围内：' + missing.join('、') +
                '，打印时会由系统字体渲染（已逐个列出，不阻断保存）', 'error');
        }
        return loadOptions().then(function () {
          if (!editingId) state.page = 0;
          return load();
        });
      })
      .catch(function (error) {
        if (handleAuthFailure(error)) {
          closeDrawer();
          return;
        }
        if (error.status === 409) {
          // 只有管理员在冲突时看到强制覆盖入口；普通用户仍然只能刷新重填。
          // 覆盖会抹掉别人刚保存的内容，所以文案必须写清后果。
          var canForce = editingId && state.user && state.user.role === 'admin';
          if (canForce && window.confirm(
                '数据已被他人修改。\n\n' +
                '强制覆盖会用你刚才填写的内容覆盖别人刚保存的修改；' +
                '被覆盖的版本、修改人和时间会写入审计记录。\n\n' +
                '确定强制覆盖吗？')) {
            return api('POST', '/api/devices/' + editingId + '/force', payload)
              .then(function () {
                closeDrawer();
                toast('已强制覆盖（已写入审计）');
                return loadOptions().then(load);
              })
              .catch(function (forceError) {
                if (handleAuthFailure(forceError)) {
                  closeDrawer();
                  return;
                }
                el.formError.textContent = forceError.message;
                el.formError.hidden = false;
              });
          }
          closeDrawer();
          toast('数据已被他人修改，请刷新后重试', 'error');
          return loadOptions().then(load);
        }
        el.formError.textContent = error.message;
        el.formError.hidden = false;
      });
  });

  el.cancelDelete.addEventListener('click', closeDelete);

  el.confirmDelete.addEventListener('click', function () {
    var id = state.pendingDeleteId;
    var version = state.pendingDeleteVersion;
    if (!id) return;
    el.confirmDelete.disabled = true;
    api('DELETE', '/api/devices/' + id + '?version=' + version)
      .then(function () {
        closeDelete();
        toast('已删除设备');
        return loadOptions().then(load);
      })
      .catch(function (error) {
        closeDelete();
        if (handleAuthFailure(error)) return;
        if (error.status === 409) {
          toast('数据已被他人修改，请刷新后重试', 'error');
          return load();
        }
        toast(error.message, 'error');
      })
      .then(function () {
        el.confirmDelete.disabled = false;
      });
  });

  // ---------- 用户 ----------

  function roleLabel(role) {
    return role === 'admin' ? '管理员' : '普通用户';
  }

  function renderUsers() {
    el.userRows.innerHTML = state.users
      .map(function (user) {
        var status = [];
        if (user.must_change_password) status.push('需改密');
        if (user.locked_until) status.push('锁定至 ' + user.locked_until);
        var statusText = status.length ? status.join(' / ') : '正常';
        return (
          '<tr>' +
          '<td>' + escapeHtml(user.user_name) + '</td>' +
          '<td>' + escapeHtml(user.display_name) + '</td>' +
          '<td>' + roleLabel(user.role) + '</td>' +
          '<td class="muted">' + escapeHtml(statusText) + '</td>' +
          '<td class="col-actions">' +
          '<button class="row-btn" type="button" data-user-action="edit" data-name="' + escapeHtml(user.user_name) + '">编辑</button>' +
          '<button class="row-btn" type="button" data-user-action="reset" data-name="' + escapeHtml(user.user_name) + '">重置密码</button>' +
          (state.user && state.user.user_name === user.user_name
            ? ''
            : '<button class="row-btn danger" type="button" data-user-action="delete" data-name="' + escapeHtml(user.user_name) + '">删除</button>') +
          '</td>' +
          '</tr>'
        );
      })
      .join('');
    el.userEmpty.hidden = state.users.length > 0;
  }

  function loadUsers() {
    return api('GET', '/api/users')
      .then(function (data) {
        state.users = data.items || [];
        renderUsers();
      })
      .catch(function (error) {
        if (handleAuthFailure(error)) return;
        toast(error.message, 'error');
      });
  }

  function openUserModal(user) {
    state.editingUser = user;
    el.userModalTitle.textContent = user ? '编辑账号' : '新增账号';
    el.userName.value = user ? user.user_name : '';
    el.userName.disabled = !!user;
    el.userDisplay.value = user ? user.display_name : '';
    el.userRole.value = user ? user.role : 'user';
    el.userPasswordLabel.hidden = !!user;
    el.userPassword.required = !user;
    el.userPassword.value = '';
    el.userError.hidden = true;
    el.userModal.hidden = false;
    if (user) el.userDisplay.focus();
    else el.userName.focus();
  }

  el.addUser.addEventListener('click', function () {
    openUserModal(null);
  });

  el.userCancel.addEventListener('click', function () {
    el.userModal.hidden = true;
    state.editingUser = null;
  });

  el.userForm.addEventListener('submit', function (event) {
    event.preventDefault();
    el.userError.hidden = true;
    var editing = state.editingUser;
    var request = editing
      ? api('PUT', '/api/users', {
          user_name: editing.user_name,
          display_name: el.userDisplay.value.trim(),
          role: el.userRole.value
        })
      : api('POST', '/api/users', {
          user_name: el.userName.value.trim(),
          display_name: el.userDisplay.value.trim(),
          role: el.userRole.value,
          password: el.userPassword.value
        });
    request
      .then(function () {
        el.userModal.hidden = true;
        state.editingUser = null;
        toast(editing ? '账号已更新' : '账号已创建');
        return loadUsers();
      })
      .catch(function (error) {
        if (handleAuthFailure(error)) {
          el.userModal.hidden = true;
          return;
        }
        el.userError.textContent = error.message;
        el.userError.hidden = false;
      });
  });

  el.userRows.addEventListener('click', function (event) {
    var button = event.target.closest('button[data-user-action]');
    if (!button) return;
    var name = button.getAttribute('data-name');
    var action = button.getAttribute('data-user-action');
    var user = state.users.filter(function (item) { return item.user_name === name; })[0];
    if (!user) return;
    if (action === 'edit') {
      openUserModal(user);
    } else if (action === 'reset') {
      state.resetUser = name;
      el.resetHint.textContent = '为「' + name + '」设置新密码，重置后下次登录必须修改。';
      el.resetPassword.value = '';
      el.resetError.hidden = true;
      el.resetModal.hidden = false;
      el.resetPassword.focus();
    } else if (action === 'delete') {
      if (!window.confirm('确定删除账号「' + name + '」？删除后不可恢复，也不会自动重建。')) return;
      api('DELETE', '/api/users?name=' + encodeURIComponent(name))
        .then(function () {
          toast('账号已删除');
          return loadUsers();
        })
        .catch(function (error) {
          if (handleAuthFailure(error)) return;
          toast(error.message, 'error');
        });
    }
  });

  el.resetCancel.addEventListener('click', function () {
    el.resetModal.hidden = true;
    state.resetUser = null;
  });

  el.resetForm.addEventListener('submit', function (event) {
    event.preventDefault();
    el.resetError.hidden = true;
    if (!state.resetUser) return;
    api('POST', '/api/users/password', {
      user_name: state.resetUser,
      new_password: el.resetPassword.value
    })
      .then(function () {
        el.resetModal.hidden = true;
        state.resetUser = null;
        toast('密码已重置');
        return loadUsers();
      })
      .catch(function (error) {
        if (handleAuthFailure(error)) {
          el.resetModal.hidden = true;
          return;
        }
        el.resetError.textContent = error.message;
        el.resetError.hidden = false;
      });
  });

  // ---------- 备份与恢复 ----------

  function formatBytes(bytes) {
    if (!bytes) return '0 B';
    if (bytes < 1024) return bytes + ' B';
    if (bytes < 1024 * 1024) return (bytes / 1024).toFixed(1) + ' KiB';
    return (bytes / 1024 / 1024).toFixed(2) + ' MiB';
  }

  function renderBackups() {
    el.backupRows.innerHTML = state.backups
      .map(function (item) {
        return (
          '<tr>' +
          '<td>' + escapeHtml(item.name) + '</td>' +
          '<td class="muted">' + escapeHtml(item.created_at) + '</td>' +
          '<td class="muted">' + formatBytes(item.bytes) + '</td>' +
          '<td class="col-actions">' +
          '<button class="row-btn danger" type="button" data-backup="' + escapeHtml(item.name) + '">恢复</button>' +
          '</td>' +
          '</tr>'
        );
      })
      .join('');
    el.backupEmpty.hidden = state.backups.length > 0;
  }

  function loadBackups() {
    return api('GET', '/api/system/backups')
      .then(function (data) {
        state.backups = data.items || [];
        renderBackups();
      })
      .catch(function (error) {
        if (handleAuthFailure(error)) return;
        toast(error.message, 'error');
      });
  }

  // 数据目录由服务端判定并打开，前端不传路径。
  el.openDataDir.addEventListener('click', function () {
    el.openDataDir.disabled = true;
    api('POST', '/api/system/open-data-dir', {})
      .then(function (data) {
        toast('已打开数据目录：' + (data && data.path ? data.path : ''));
      })
      .catch(function (error) {
        if (handleAuthFailure(error)) return;
        toast(error.message, 'error');
      })
      .then(function () { el.openDataDir.disabled = false; });
  });

  el.backupNow.addEventListener('click', function () {
    el.backupNow.disabled = true;
    api('POST', '/api/system/backup', {})
      .then(function (entry) {
        toast('备份完成：' + entry.name);
        return loadBackups();
      })
      .catch(function (error) {
        if (handleAuthFailure(error)) return;
        toast(error.message, 'error');
      })
      .then(function () {
        el.backupNow.disabled = false;
      });
  });

  el.backupRows.addEventListener('click', function (event) {
    var button = event.target.closest('button[data-backup]');
    if (!button) return;
    var name = button.getAttribute('data-backup');
    if (!window.confirm('确定用「' + name + '」覆盖当前数据库？恢复前会自动备份当前库。')) return;
    api('POST', '/api/system/restore', { name: name, confirm: 'RESTORE' })
      .then(function () {
        window.alert('恢复完成，页面将重新加载。');
        window.location.reload();
      })
      .catch(function (error) {
        if (handleAuthFailure(error)) return;
        toast(error.message, 'error');
      });
  });

  document.addEventListener('keydown', function (event) {
    if (event.key !== 'Escape') return;
    if (!el.modal.hidden) closeDelete();
    else if (el.drawer.classList.contains('is-open')) closeDrawer();
    else if (!el.userModal.hidden) el.userModal.hidden = true;
    else if (!el.resetModal.hidden) el.resetModal.hidden = true;
    else if (!el.updateModal.hidden) el.updateModal.hidden = true;
  });

  // ---------- 卡片打印 ----------

  function hasNativeBridge() {
    if (window.chrome && window.chrome.webview && window.chrome.webview.postMessage) {
      return true;
    }
    return !!(window.webkit && window.webkit.messageHandlers &&
      window.webkit.messageHandlers.devcard);
  }

  // 壳层的两条输出通道共用同一份打印 DOM，只改变输出目标。
  function sendToShell(action, payload) {
    var message = payload || {};
    message.action = action;
    // WebView2 原生桥优先：不依赖注入脚本的时机（注入晚于点击时老写法会静默落空）。
    if (window.chrome && window.chrome.webview && window.chrome.webview.postMessage) {
      window.chrome.webview.postMessage(message);
      return true;
    }
    if (window.webkit && window.webkit.messageHandlers &&
        window.webkit.messageHandlers.devcard) {
      window.webkit.messageHandlers.devcard.postMessage(message);
      return true;
    }
    return false;
  }

  // 壳层回传导出/打印结果（见 shell/mac/main.mm）。
  window.__devcardNotify = function (kind, message) {
    toast(message, kind === 'error' ? 'error' : undefined);
  };

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
      '<div class="card-company"><span>' + escapeHtml(state.companyNamePrint) + '</span></div>' +
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
    var f = state.filters;
    if (f.keyword) params.set('keyword', f.keyword);
    if (f.field) params.set('field', f.field);
    if (f.name) params.set('name', f.name);
    if (f.location) params.set('location', f.location);
    return params.toString();
  }

  function printLayoutSize() {
    return Number(el.printLayout.value) || 12;
  }

  function updatePrintControls() {
    var empty = state.printItems.length === 0;
    el.printExport.disabled = state.printBusy || empty;
    el.printPrint.disabled = state.printBusy || empty;
    el.printEmpty.hidden = !empty || state.printBusy;
    var layout = printLayoutSize();
    var pages = empty ? 0 : Math.ceil(state.printItems.length / layout);
    el.printScope.textContent = '当前筛选结果 ' + state.printItems.length + ' 条 · 每页 ' +
      layout + ' 张 · 共 ' + pages + ' 页' +
      (state.printTruncated ? '（超过 10000 条，已截断）' : '');
  }

  function renderPrint() {
    var layout = printLayoutSize();
    var items = state.printItems;
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
    scan(state.companyNamePrint, '公司名');
    state.printItems.forEach(function (item) {
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

  // 打印/导出前必须确认子集字体已就绪。字体走 /fonts/ HTTP 路由，
  // 未加载完就截图会静默退回系统字体：屏幕上「差不多」，但字宽变了，叠图会超 1 mm。
  // 这里既等待 document.fonts.ready，也用 check() 断言两个字重确实可用；
  // 失败时把错误交给调用方中止，绝不产出用系统字体「顶替」的 PDF。
  var printFonts = { ready: false, error: '' };

  // 期望的字体文件、字节数与 SHA256 来自构建期（/api/fonts/coverage 的 fonts 字段），
  // 不在 JS 里写死：重新子集后忘了同步，导出会以「字节数不符」永久中止，而真正的原因是清单过期。
  // 判据用 SHA256：重子集后大小碰巧相同、内容变了，只比大小会判「通过」再拿错字体排版。
  var printFontSpec = null;

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
    state.printBusy = true;
    updatePrintControls();
    return api('GET', '/api/print/cards?' + printQuery())
      .then(function (data) {
        state.printItems = data.items || [];
        state.printTruncated = !!data.truncated;
        if (data.company_name) state.companyNamePrint = data.company_name;
        // 打印页初始化就等字体：用户点导出时字体必须已经就位，
        // 而不是在截图那一瞬间才去加载。
        return ensurePrintFonts();
      })
      .then(function (fontsOk) {
        state.printBusy = false;
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
        state.printBusy = false;
        state.printItems = [];
        state.printTruncated = false;
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
    // 这里查 state.companyNamePrint（打印实际用的那份），不是输入框里的原文。
    var fontCheck = companyFontCheck(state.companyNamePrint || '');
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

  el.gotoPrint.addEventListener('click', function () {
    setView('print');
  });
  el.printBack.addEventListener('click', function () {
    setView('devices');
  });
  el.printLayout.addEventListener('change', renderPrint);
  window.addEventListener('resize', updatePrintScale);

  el.printExport.addEventListener('click', function () {
    if (!state.printItems.length) return;
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
    if (!state.printItems.length) return;
    if (sendToShell('print')) return;
    if (!printFonts.ready) {
      toast(printFonts.error || '子集字体未就绪，已中止打印', 'error');
      return;
    }
    window.print();
  });

  // ---------- 自动更新自检（M6） ----------
  //
  // 四种失败路径（超时 / DNS 失败 / 返回非 JSON / 版本号格式不对）必须和
  // 「没有新版本」在界面上不可区分：不弹窗、不显示提示、不抛异常。
  // 另留一条正向对照：没有它，「永远不提示」也能让四条失败用例全绿。
  // 只替换传输层，版本比较、状态判定仍是导出路径上的同一份代码。

  window.__devcardUpdateState = function () {
    return {
      version: state.version,
      pill: el.updatePill.hidden ? '' : el.updatePill.textContent,
      check: state.updateCheck
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
    state.updateCheck = {
      status: 'parsed',
      endpoint: '(fixture)',
      raw: raw,
      release: release
    };
    var current = state.version || '';
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
      user: (state.user && state.user.user_name) || '',
      version: state.version || '',
      manifest: state.update ? 'loaded' : 'missing',
      check_config: (state.update && state.update.check) || null,
      last_check_at: last,
      status: (state.updateCheck && state.updateCheck.status) || ''
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

  // ---------- 密码可见性切换 ----------
  // 只改变本地输入框的显示方式，不动提交内容。
  var EYE_ICON =
    '<svg class="eye-open" viewBox="0 0 16 16" aria-hidden="true">' +
    '<path d="M1.2 8S3.8 3.8 8 3.8 14.8 8 14.8 8 12.2 12.2 8 12.2 1.2 8 1.2 8z"/>' +
    '<circle cx="8" cy="8" r="2.2"/></svg>' +
    '<svg class="eye-off" viewBox="0 0 16 16" aria-hidden="true">' +
    '<path d="M2 2l12 12"/>' +
    '<path d="M6.6 3.9A7.7 7.7 0 0 1 8 3.8C12.2 3.8 14.8 8 14.8 8a13.5 13.5 0 0 1-2.3 2.8"/>' +
    '<path d="M4.4 5.2A13.3 13.3 0 0 0 1.2 8S3.8 12.2 8 12.2c.7 0 1.4-.1 2-.4"/>' +
    '<path d="M6.6 6.6a2.2 2.2 0 0 0 3.1 3.1"/></svg>';

  Array.prototype.forEach.call(
    document.querySelectorAll('.password-eye[data-password-toggle]'),
    function (button) {
      button.innerHTML = EYE_ICON;
      button.addEventListener('click', function () {
        var input = document.getElementById(
          button.getAttribute('data-password-toggle'));
        if (!input) return;
        var visible = input.type === 'password';
        input.type = visible ? 'text' : 'password';
        button.classList.toggle('is-visible', visible);
        button.setAttribute('aria-pressed', visible ? 'true' : 'false');
        button.setAttribute('aria-label', visible ? '隐藏密码' : '显示密码');
        input.focus();
      });
    });

  // ---------- 启动 ----------

  api('GET', '/api/bootstrap')
    .then(applyBootstrap)
    .catch(function (error) {
      if (error.status === 401) {
        tryLocalSession();
        return;
      }
      showLogin('无法连接本地服务：' + error.message);
    });
})();
