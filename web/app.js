(function () {
  'use strict';

  var PAGE_SIZE = 20;

  var state = {
    page: 0,
    total: 0,
    items: [],
    filters: { keyword: '', field: '', name: '', location: '' },
    editingId: null,
    pendingDeleteId: null,
    user: null,
    view: 'devices',
    users: [],
    backups: [],
    editingUser: null,
    resetUser: null,
    forcedPassword: false
  };

  var el = {
    app: document.getElementById('app'),
    health: document.getElementById('health'),
    version: document.getElementById('version-pill'),
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
          throw failure;
        }
        return data;
      });
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
    load();
    if (state.view === 'users') {
      loadUsers();
      loadBackups();
    }
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
    el.passwordTitle.textContent = forced ? '首次登录必须修改口令' : '修改口令';
    el.passwordHint.textContent = forced
      ? '当前账号仍使用初始或重置口令，修改后才能进入业务页。'
      : '修改当前账号的口令。';
    el.passwordCancel.hidden = forced;
    el.oldPassword.value = '';
    el.newPassword.value = '';
    el.newPassword2.value = '';
    el.passwordError.hidden = true;
    el.passwordModal.hidden = false;
    el.oldPassword.focus();
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
      el.passwordError.textContent = '两次输入的新口令不一致';
      el.passwordError.hidden = false;
      return;
    }
    api('POST', '/api/users/password', {
      old_password: el.oldPassword.value,
      new_password: el.newPassword.value
    })
      .then(function () {
        var forced = state.forcedPassword;
        state.forcedPassword = false;
        state.user.must_change_password = false;
        el.passwordModal.hidden = true;
        toast('口令已更新');
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
    el.viewDevices.hidden = view !== 'devices';
    el.viewUsers.hidden = view !== 'users';
    el.pageTitle.textContent = view === 'devices' ? '设备台账' : '用户管理';
    el.pageSub.textContent = view === 'devices'
      ? '维护设备名称、位置、控制范围与维护电话'
      : '账号维护与数据备份恢复';
    if (view === 'users') {
      loadUsers();
      loadBackups();
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
  }

  function openDelete(device) {
    state.pendingDeleteId = device.id;
    el.modalText.textContent =
      '确定删除「' + device.dev_name + '」·' + device.location + '？删除后不可恢复。';
    el.modal.hidden = false;
  }

  function closeDelete() {
    state.pendingDeleteId = null;
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
    var request = editingId
      ? api('PUT', '/api/devices/' + editingId, payload)
      : api('POST', '/api/devices', payload);

    request
      .then(function () {
        closeDrawer();
        toast(editingId ? '已保存修改' : '已新增设备');
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
        el.formError.textContent = error.message;
        el.formError.hidden = false;
      });
  });

  el.cancelDelete.addEventListener('click', closeDelete);

  el.confirmDelete.addEventListener('click', function () {
    var id = state.pendingDeleteId;
    if (!id) return;
    el.confirmDelete.disabled = true;
    api('DELETE', '/api/devices/' + id)
      .then(function () {
        closeDelete();
        toast('已删除设备');
        return loadOptions().then(load);
      })
      .catch(function (error) {
        closeDelete();
        if (handleAuthFailure(error)) return;
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
          '<button class="row-btn" type="button" data-user-action="reset" data-name="' + escapeHtml(user.user_name) + '">重置口令</button>' +
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
      el.resetHint.textContent = '为「' + name + '」设置新口令，重置后下次登录必须修改。';
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
        toast('口令已重置');
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
