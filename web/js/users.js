import { api, appState, el, escapeHtml, formatBytes, handleAuthFailure, systemState, toast, usersState } from './core.js';

  // ---------- 用户 ----------
  function roleLabel(role) {
    return role === 'admin' ? '管理员' : '普通用户';
  }

  function renderUsers() {
    el.userRows.innerHTML = usersState.users
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
          (appState.user && appState.user.user_name === user.user_name
            ? ''
            : '<button class="row-btn danger" type="button" data-user-action="delete" data-name="' + escapeHtml(user.user_name) + '">删除</button>') +
          '</td>' +
          '</tr>'
        );
      })
      .join('');
    el.userEmpty.hidden = usersState.users.length > 0;
  }

  function loadUsers() {
    return api('GET', '/api/users')
      .then(function (data) {
        usersState.users = data.items || [];
        renderUsers();
      })
      .catch(function (error) {
        if (handleAuthFailure(error)) return;
        toast(error.message, 'error');
      });
  }

  function openUserModal(user) {
    usersState.editingUser = user;
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
    usersState.editingUser = null;
  });

  el.userForm.addEventListener('submit', submitUserForm);

  function submitUserForm(event) {
    event.preventDefault();
    el.userError.hidden = true;
    var editing = usersState.editingUser;
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
        usersState.editingUser = null;
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
  }

  el.userRows.addEventListener('click', handleUserRowClick);

  function handleUserRowClick(event) {
    var button = event.target.closest('button[data-user-action]');
    if (!button) return;
    var name = button.getAttribute('data-name');
    var action = button.getAttribute('data-user-action');
    var user = usersState.users.filter(function (item) { return item.user_name === name; })[0];
    if (!user) return;
    if (action === 'edit') {
      openUserModal(user);
    } else if (action === 'reset') {
      usersState.resetUser = name;
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
  }

  el.resetCancel.addEventListener('click', function () {
    el.resetModal.hidden = true;
    usersState.resetUser = null;
  });

  el.resetForm.addEventListener('submit', submitResetPasswordForm);

  function submitResetPasswordForm(event) {
    event.preventDefault();
    el.resetError.hidden = true;
    if (!usersState.resetUser) return;
    api('POST', '/api/users/password', {
      user_name: usersState.resetUser,
      new_password: el.resetPassword.value
    })
      .then(function () {
        el.resetModal.hidden = true;
        usersState.resetUser = null;
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
  }

  // ---------- 备份与恢复 ----------
  function renderBackups() {
    el.backupRows.innerHTML = systemState.backups
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
    el.backupEmpty.hidden = systemState.backups.length > 0;
  }

  function loadBackups() {
    return api('GET', '/api/system/backups')
      .then(function (data) {
        systemState.backups = data.items || [];
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
        // 恢复会重建整库并清空所有会话，reload 后必然回到登录页：
        // 提前说明，避免用户以为登录态丢失是异常。
        window.alert('恢复完成，页面将重新加载。恢复后需要重新登录。');
        window.location.reload();
      })
      .catch(function (error) {
        if (handleAuthFailure(error)) return;
        toast(error.message, 'error');
      });
  });

export { handleUserRowClick, loadBackups, loadUsers, openUserModal, renderBackups, renderUsers, roleLabel, submitResetPasswordForm, submitUserForm };
