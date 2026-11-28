import { api, appState, el, handleAuthFailure, hasNativeBridge, printState, setAuthHooks, setStatusError, systemState, toast, updateStatusIdentity } from './core.js';
import { sendToShell } from './shell.js';
import { loadFontCoverage } from './fonts.js';
import { closeDelete, closeDrawer, load, loadOptions } from './devices.js';
import { updateCompanyHint, updateLogoTime } from './settings.js';
import { loadBackups, loadUsers } from './users.js';
import { loadPrint } from './print.js';

  setAuthHooks({ showLogin: showLogin, openPasswordModal: openPasswordModal });

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
    if (!appState.dataDir) return;
    if (sendToShell('openDataDir', { path: appState.dataDir })) return;
    if (navigator.clipboard && navigator.clipboard.writeText) {
      navigator.clipboard.writeText(appState.dataDir).then(function () {
        toast('数据目录路径已复制：' + appState.dataDir);
      });
    } else {
      toast('数据目录：' + appState.dataDir);
    }
  });

  // 桌面壳：点状态栏版本号打开原生「关于 DevCard」面板；最右端的「检查更新」
  // 按钮直接触发原生检查——两个入口 Windows / macOS 完全一致（浏览器版不显示）。
  if (hasNativeBridge()) {
    el.statusVersion.classList.add('status-link');
    el.statusVersion.title = '关于 DevCard';
    el.statusVersion.addEventListener('click', function () {
      sendToShell('showAbout');
    });
    el.statusCheckUpdate.hidden = false;
    el.statusCheckUpdate.title = '立即检查一次，不受 24 小时间隔限制';
    el.statusCheckUpdate.addEventListener('click', function () {
      sendToShell('checkForUpdates');
    });
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
    appState.user = data.user;
    el.app.hidden = false;
    el.login.hidden = true;
    el.userChip.hidden = false;
    el.logout.hidden = false;
    el.userChip.textContent =
      data.user.user_name + (data.user.role === 'admin' ? ' · 管理员' : ' · 用户');
    el.navUsers.hidden = data.user.role !== 'admin';
    el.dedupeBtn.hidden = data.user.role !== 'admin';
    el.version.textContent = 'v' + data.version;
    el.health.textContent = '服务正常 · schema ' + data.schema_version;
    appState.version = data.version || '';
    appState.schemaVersion = data.schema_version || '';
    appState.dataDir = data.data_dir || '';
    updateStatusIdentity();
    systemState.companyName = data.company_name || '';
    systemState.companyNamePrint = data.company_name_print || systemState.companyName;
    systemState.companyNameMaxWidth = data.company_name_max_width || 13;
    el.companyName.value = systemState.companyName;
    el.companyLogoPreview.src = data.company_logo_url || '/company-logo';
    updateCompanyHint();
    updateLogoTime(data.company_logo_updated_at);
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
    if (appState.view === 'users') {
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

  // 【回归 Bug 修复｜请勿改动】桌面壳里的「退出」必须真正退出应用，不能只是注销。
  // 事故经过：这段逻辑一度被改成只 POST /api/session/logout + showLogin()，
  // macOS / Windows 桌面版点「退出」后停在登录页（截图见 2026-11 的反馈），
  // 用户看起来就是「点了退出没退出」，且已重复出现两次。
  // 正确行为：壳层（WebView）下转交原生 quit（mac: NSApp terminate / win: WM_CLOSE），
  // 只有浏览器 / 网络版才走注销回登录页。不要为了「统一逻辑」删掉下面的壳层分支。
  el.logout.addEventListener('click', function () {
    if (hasNativeBridge() && sendToShell('quit', {})) return;
    api('POST', '/api/session/logout')
      .then(function () {
        appState.user = null;
        showLogin();
      })
      .catch(function (error) {
        toast(error.message, 'error');
      });
  });

  // ---------- 强制改密 ----------
  function openPasswordModal(forced) {
    appState.forcedPassword = forced;
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
    if (appState.forcedPassword) return;
    el.passwordModal.hidden = true;
  });

  el.passwordForm.addEventListener('submit', submitPasswordForm);

  function submitPasswordForm(event) {
    event.preventDefault();
    el.passwordError.hidden = true;
    if (el.newPassword.value !== el.newPassword2.value) {
      el.passwordError.textContent = '两次输入的新密码不一致';
      el.passwordError.hidden = false;
      return;
    }
    api('POST', '/api/users/password', appState.forcedPassword
      ? { new_password: el.newPassword.value }
      : { old_password: el.oldPassword.value, new_password: el.newPassword.value })
      .then(function () {
        var forced = appState.forcedPassword;
        appState.forcedPassword = false;
        appState.user.must_change_password = false;
        el.passwordModal.hidden = true;
        toast('密码已更新');
        if (forced) enterApp();
      })
      .catch(function (error) {
        if (handleAuthFailure(error)) return;
        el.passwordError.textContent = error.message;
        el.passwordError.hidden = false;
      });
  }

  // ---------- 视图切换 ----------
  function setView(view, preservePrintSelection) {
    appState.view = view;
    // 从导航/「打印」按钮进打印页时按当前筛选全打；只有台账「打印选中」会保留 ids。
    if (view === 'print' && !preservePrintSelection) printState.ids = [];
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

  document.addEventListener('keydown', function (event) {
    if (event.key !== 'Escape') return;
    if (!el.modal.hidden) closeDelete();
    else if (el.drawer.classList.contains('is-open')) closeDrawer();
    else if (!el.userModal.hidden) el.userModal.hidden = true;
    else if (!el.resetModal.hidden) el.resetModal.hidden = true;
  });

  el.gotoPrint.addEventListener('click', function () {
    setView('print');
  });

  el.printBack.addEventListener('click', function () {
    setView('devices');
  });

  el.printClearSelection.addEventListener('click', function () {
    printState.ids = [];
    loadPrint();
  });

export { applyBootstrap, enterApp, loadBootstrap, openPasswordModal, setView, showLogin, submitPasswordForm, tryLocalSession };
