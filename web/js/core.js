  // 认证失败后的页面动作由 session 注册：core 不反向依赖会话模块。
  var authHooks = { showLogin: null, openPasswordModal: null };
  function setAuthHooks(hooks) { authHooks = hooks; }

  var PAGE_SIZE = 20;

  // 状态按域拆分：每个对象只被对应视图读写，改一处不会误伤其他视图。
  // 新增字段放进对应域，不要再往扁平对象上挂。
  // 会话与应用元信息：登录用户、版本号、数据目录、当前视图。
  var appState = {
    user: null,
    version: '',
    schemaVersion: '',
    dataDir: '',
    view: 'devices',
    forcedPassword: false,
    // 打印字体能覆盖的码位区间 [[start,end], ...]，来自 /api/fonts/coverage，
    // 也就是随包字体 cmap 的真实内容，不是手写表。台账与打印共用。
    fontRanges: null
  };

  // 设备台账：分页、筛选、编辑与删除的乐观锁现场。
  var devicesState = {
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
    // 跨页保留的勾选集合：进入打印页时快照给 printState.ids。
    selectedIds: []
  };

  var usersState = {
    users: [],
    editingUser: null,
    resetUser: null
  };

  // 系统设置：公司名/图标（「账号」页的打印设置区）与备份列表。
  var systemState = {
    companyName: '',
    companyNamePrint: '',
    companyNameMaxWidth: 13,
    backups: []
  };

  // 卡片打印：当前筛选命中的数据快照与导出状态。
  var printState = {
    items: [],
    truncated: false,
    busy: false,
    // 非空时 /api/print/cards 带 ids，只打印台账里勾选的那几台。
    ids: []
  };

  // 自更新：远端检查结论、下载/安装进度与一次性定时器。
  var updateState = {
    // 本地清单通道下发的更新信息（/api/bootstrap 的 update 段）。
    info: null,
    // 远端检查的原始结论（含接口原文），自检与核对用；
    // 这层出过「拿假数据截图」的事，解析结果必须能对回原文。
    check: null,
    url: '',
    timer: null,
    downloading: false,
    asset: null,
    assetVersion: '',
    force: false,
    installing: false
  };

  var el = {
    app: document.getElementById('app'),
    health: document.getElementById('health'),
    version: document.getElementById('version-pill'),
    updatePill: document.getElementById('update-pill'),
    updateCheckBtn: document.getElementById('update-check-btn'),
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
    updateTitle: document.getElementById('update-title'),
    updateMandatory: document.getElementById('update-mandatory'),
    userChip: document.getElementById('user-chip'),
    logout: document.getElementById('logout'),
    navUsers: document.getElementById('nav-users'),
    pageTitle: document.getElementById('page-title'),
    pageSub: document.getElementById('page-sub'),
    viewDevices: document.getElementById('view-devices'),
    viewUsers: document.getElementById('view-users'),
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
    dedupeBtn: document.getElementById('dedupe-btn'),
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
    printSelected: document.getElementById('print-selected'),
    selectAll: document.getElementById('select-all'),
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
    printClearSelection: document.getElementById('print-clear-selection'),
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

  // 字节数统一按 1024 进制显示（KiB/MiB）。更新包进度与备份大小共用这一份：
  // 两处各写一次时，同一屏会出现两套单位，且同作用域的后定义会静默覆盖前一个。
  function formatBytes(bytes) {
    if (!bytes) return '0 B';
    if (bytes < 1024) return bytes + ' B';
    if (bytes < 1024 * 1024) return (bytes / 1024).toFixed(1) + ' KiB';
    return (bytes / 1024 / 1024).toFixed(2) + ' MiB';
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
      if (authHooks.showLogin) authHooks.showLogin();
      return true;
    }
    if (error.status === 403 && error.code === 'must_change_password') {
      if (authHooks.openPasswordModal) authHooks.openPasswordModal(true);
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
    if (!appState.user) return;
    el.statusUser.textContent = appState.user.display_name || appState.user.user_name;
    // 有原生桥 = 桌面壳（单机版）；纯浏览器访问 = 网络版。
    el.statusMode.textContent = hasNativeBridge() ? '单机版' : '网络版';
    el.statusVersion.textContent = 'v' + (appState.version || '—');
    el.statusSchema.textContent = 'schema ' + (appState.schemaVersion || '—');
    el.statusDatadir.textContent = appState.dataDir || '—';
    el.statusDatadir.title = '打开数据目录：' + (appState.dataDir || '—');
  }

  function filtersActive() {
    var f = devicesState.filters;
    return !!(f.keyword || f.field || f.name || f.location);
  }

  // 条数与页脚同源：都用 devicesState.total，不另算一次。
  function updateStatusCount() {
    el.statusCount.textContent = filtersActive()
      ? '筛选后 ' + devicesState.total + ' 条 / 共 ' + devicesState.totalAll + ' 条'
      : '共 ' + devicesState.total + ' 条';
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

  // ---------- 卡片打印 ----------
  function hasNativeBridge() {
    if (window.chrome && window.chrome.webview && window.chrome.webview.postMessage) {
      return true;
    }
    return !!(window.webkit && window.webkit.messageHandlers &&
      window.webkit.messageHandlers.devcard);
  }

export { PAGE_SIZE, api, appState, authHooks, devicesState, el, escapeHtml, filtersActive, formatBytes, handleAuthFailure, hasNativeBridge, printState, setAuthHooks, setStatusError, setStatusOk, systemState, toast, toastTimer, updateState, updateStatusCount, updateStatusIdentity, usersState };
