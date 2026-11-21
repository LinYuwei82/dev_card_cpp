import { api, appState, el, handleAuthFailure, systemState, toast } from './core.js';
import { companyFontCheck, companyNameWidth, truncateCompanyName } from './fonts.js';
import { renderPrint } from './print.js';

  function updateCompanyHint() {
    var value = el.companyName.value;
    var width = companyNameWidth(value);
    var over = width > systemState.companyNameMaxWidth + 1e-9;
    // 录入放宽到 16 个字符，卡面仍按 13 字宽打印：把截断提前展示成用户看得见的
    // 结果，而不是保存后再解释。
    el.companyPreview.textContent = value.trim()
      ? '将打印为：「' + truncateCompanyName(value) + '」' +
        (over ? '（卡面按 ' + systemState.companyNameMaxWidth + ' 字宽打印，只保留前 ' +
                systemState.companyNameMaxWidth + ' 字宽）' : '')
      : '将打印为：默认公司名「深圳市安业物业管理有限公司」';
    el.companyHint.textContent = '已用 ' + width + ' / ' + systemState.companyNameMaxWidth +
      '（汉字算 1，字母数字算 0.5），最多录入 16 个字符';
    el.companyHint.classList.toggle('is-over', over);
    el.companyOver.hidden = !over;
    // 字体覆盖是另一件事：字放得下（宽度上限），但打印子集里没有这个字。
    // 只查真正会打印的那一段（截断后），否则会为永远打不出的字报警，
    // 且与导出前 companyFontCheck(systemState.companyNamePrint) 的判定自相矛盾。
    var fontCheck = companyFontCheck(truncateCompanyName(value));
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

  el.saveCompany.addEventListener('click', saveCompanyName);

  function saveCompanyName() {
    el.saveCompany.disabled = true;
    api('PUT', '/api/system/company', { company_name: el.companyName.value.trim() })
      .then(function (data) {
        systemState.companyName = data.company_name;
        systemState.companyNamePrint = data.company_name_print;
        systemState.companyNameMaxWidth = data.company_name_max_width || 13;
        el.companyName.value = data.company_name;
        updateCompanyHint();
        if (appState.view === 'print') renderPrint();
        toast(data.company_name_width > systemState.companyNameMaxWidth
          ? '已保存；卡面只打印前 ' + systemState.companyNameMaxWidth + ' 字宽，实际内容见预览'
          : '公司名已保存');
      })
      .catch(function (error) {
        if (handleAuthFailure(error)) return;
        toast(error.message, 'error');
      })
      .then(function () {
        el.saveCompany.disabled = false;
      });
  }

  el.pickLogo.addEventListener('click', function () {
    el.logoFile.click();
  });

  el.logoFile.addEventListener('change', handleLogoFile);

  function handleLogoFile() {
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
  }

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

export { handleLogoFile, saveCompanyName, updateCompanyHint, updateLogoTime };
