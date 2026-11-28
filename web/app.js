import { api, toast } from './js/core.js';
import './js/shell.js';
import './js/fonts.js';
import './js/devices.js';
import './js/settings.js';
import './js/users.js';
import './js/print.js';
import { applyBootstrap, showLogin, tryLocalSession } from './js/session.js';

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
