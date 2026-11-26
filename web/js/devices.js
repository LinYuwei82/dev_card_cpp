import { PAGE_SIZE, api, appState, devicesState, el, escapeHtml, handleAuthFailure, printState, toast, updateStatusCount } from './core.js';
import { uncoveredChars } from './fonts.js';
import { setView } from './session.js';

  // ---------- 设备台账 ----------
  function totalPages() {
    return Math.max(1, Math.ceil(devicesState.total / PAGE_SIZE));
  }

  function selectionCount() {
    return devicesState.selectedIds.length;
  }

  function updateSelectionControls() {
    var items = devicesState.items;
    var chosen = 0;
    for (var i = 0; i < items.length; i++) {
      if (devicesState.selectedIds.indexOf(items[i].id) !== -1) chosen++;
    }
    el.selectAll.checked = items.length > 0 && chosen === items.length;
    el.selectAll.indeterminate = chosen > 0 && chosen < items.length;
    el.printSelected.disabled = selectionCount() === 0;
    el.printSelected.textContent = selectionCount() > 0
      ? '打印选中（' + selectionCount() + '）'
      : '打印选中';
  }

  function toggleSelection(id, checked) {
    var index = devicesState.selectedIds.indexOf(id);
    if (checked && index === -1) devicesState.selectedIds.push(id);
    if (!checked && index !== -1) devicesState.selectedIds.splice(index, 1);
    updateSelectionControls();
  }

  function renderRows() {
    var html = devicesState.items
      .map(function (item) {
        var checked = devicesState.selectedIds.indexOf(item.id) !== -1;
        return (
          '<tr>' +
          '<td class="col-select"><input type="checkbox" class="row-check" ' +
          'aria-label="选择设备 ' + item.id + '" data-id="' + item.id + '"' +
          (checked ? ' checked' : '') + ' /></td>' +
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
    el.empty.hidden = devicesState.items.length > 0;
    updateSelectionControls();
  }

  function renderPager() {
    el.total.textContent = String(devicesState.total);
    el.pageLabel.textContent = '第 ' + (devicesState.page + 1) + ' / ' + totalPages() + ' 页';
    el.prev.disabled = devicesState.page <= 0;
    el.next.disabled = devicesState.page + 1 >= totalPages();
    updateStatusCount();
  }

  function queryString() {
    var params = new URLSearchParams();
    var f = devicesState.filters;
    if (f.keyword) params.set('keyword', f.keyword);
    if (f.field) params.set('field', f.field);
    if (f.name) params.set('name', f.name);
    if (f.location) params.set('location', f.location);
    params.set('page', String(devicesState.page));
    params.set('page_size', String(PAGE_SIZE));
    return params.toString();
  }

  function load() {
    return api('GET', '/api/devices?' + queryString())
      .then(function (data) {
        devicesState.items = data.items || [];
        devicesState.total = data.total || 0;
        devicesState.totalAll = data.total_all == null ? devicesState.total : data.total_all;
        devicesState.page = data.page || 0;
        if (devicesState.items.length === 0 && devicesState.page > 0 && devicesState.total > 0) {
          devicesState.page = Math.max(0, totalPages() - 1);
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
    devicesState.editingId = device ? device.id : null;
    devicesState.editingVersion = device ? device.version : 0;
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
    devicesState.editingId = null;
    devicesState.editingVersion = 0;
  }

  function openDelete(device) {
    devicesState.pendingDeleteId = device.id;
    devicesState.pendingDeleteVersion = device.version;
    el.modalText.textContent =
      '确定删除「' + device.dev_name + '」·' + device.location + '？删除后不可恢复。';
    el.modal.hidden = false;
  }

  function closeDelete() {
    devicesState.pendingDeleteId = null;
    devicesState.pendingDeleteVersion = 0;
    el.modal.hidden = true;
  }

  function syncFiltersFromInputs() {
    devicesState.filters.keyword = el.keyword.value.trim();
    devicesState.filters.field = el.field.value;
    devicesState.filters.name = el.name.value;
    devicesState.filters.location = el.location.value;
  }

  el.filters.addEventListener('submit', function (event) {
    event.preventDefault();
    syncFiltersFromInputs();
    devicesState.page = 0;
    load();
  });

  el.reset.addEventListener('click', function () {
    el.keyword.value = '';
    el.field.value = '';
    el.name.value = '';
    el.location.value = '';
    devicesState.filters = { keyword: '', field: '', name: '', location: '' };
    devicesState.page = 0;
    load();
  });

  el.prev.addEventListener('click', function () {
    if (devicesState.page <= 0) return;
    devicesState.page -= 1;
    load();
  });

  el.next.addEventListener('click', function () {
    if (devicesState.page + 1 >= totalPages()) return;
    devicesState.page += 1;
    load();
  });

  el.add.addEventListener('click', function () {
    openDrawer(null);
  });

  // 合并重复：三字段（名称 + 位置 + 控制范围）完全相同的记录，每组只保留最早一条。
  // 不可撤销，模型与恢复备份一致，用浏览器原生确认框，不再新造一层弹窗。
  el.dedupeBtn.addEventListener('click', function () {
    if (!window.confirm(
      '将删除名称、位置、控制范围完全相同的重复记录，每组只保留最早一条；' +
      '保留行电话为空时，会从重复行里补齐。此操作不可撤销，建议先备份。确定继续？'
    )) return;
    el.dedupeBtn.disabled = true;
    api('POST', '/api/devices/dedupe', { confirm: 'MERGE_DUPLICATES' })
      .then(function (data) {
        toast('已合并重复：删除 ' + data.removed + ' 条，剩余 ' + data.remaining + ' 条');
        return loadOptions().then(load);
      })
      .catch(function (error) {
        if (handleAuthFailure(error)) return;
        toast(error.message, 'error');
      })
      .then(function () {
        el.dedupeBtn.disabled = false;
      });
  });

  el.importBtn.addEventListener('click', function () {
    el.importFile.value = '';
    el.importFile.click();
  });

  el.importFile.addEventListener('change', handleImportFile);

  // 导入：直接把文件字节 POST 给服务端解析，前端只负责展示统计结果。
  function handleImportFile() {
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
        if (report.skipped_invalid) {
          var badRows = (report.invalid_rows || []).join('、');
          parts.push('跳过必填项缺失 ' + report.skipped_invalid + ' 行' +
                     (badRows ? '（第 ' + badRows + ' 行）' : ''));
        }
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
  }

  // 导出/模板统一出口：内容先由页面取好（带会话），落到哪里由用户决定。
  // 壳内交给壳层弹 NSSavePanel（原子写入，取消不落文件）；浏览器里退回原本的下载。
  function bytesToBase64(buffer) {
    var bytes = new Uint8Array(buffer);
    var chunks = [];
    var step = 0x8000;
    for (var i = 0; i < bytes.length; i += step) {
      chunks.push(String.fromCharCode.apply(null, bytes.subarray(i, i + step)));
    }
    return btoa(chunks.join(''));
  }

  function shellSaveFile(filename, blob, title) {
    return blob.arrayBuffer().then(function (buffer) {
      return new Promise(function (resolve) {
        window.__devcardSaveResult = function (result) {
          window.__devcardSaveResult = null;
          resolve(result || { status: 'error', message: '没有拿到保存结果' });
        };
        window.webkit.messageHandlers.devcard.postMessage({
          action: 'saveFile',
          filename: filename,
          title: title,
          contentBase64: bytesToBase64(buffer)
        });
      });
    });
  }

  function downloadInBrowser(blob, filename) {
    var url = URL.createObjectURL(blob);
    var link = document.createElement('a');
    link.href = url;
    link.download = filename;
    document.body.appendChild(link);
    link.click();
    document.body.removeChild(link);
    setTimeout(function () { URL.revokeObjectURL(url); }, 1000);
  }

  function saveXlsx(options) {
    var bridge = window.webkit && window.webkit.messageHandlers &&
                 window.webkit.messageHandlers.devcard;
    return fetch(options.url)
      .then(function (response) {
        if (!response.ok) throw new Error(options.fail + '（' + response.status + '）');
        return response.blob();
      })
      .then(function (blob) {
        // Windows 壳没有 saveFile 桥（NSSavePanel 仅 macOS 实现）：走浏览器下载
        // 通道，WebView2 的 DownloadStarting 会接管落点到下载目录并提示完成；
        // 若误走 shellSaveFile，消息被壳丢弃、Promise 永不 resolve，按钮会卡死。
        if (!window.__devcardShell || !bridge ||
            (window.chrome && window.chrome.webview)) {
          downloadInBrowser(blob, options.filename);
          toast(options.okText);
          return null;
        }
        return shellSaveFile(options.filename, blob, options.title).then(function (result) {
          if (result.status === 'cancel') return null;   // 取消：安静退出，不提示不落文件
          if (result.status !== 'ok') throw new Error(result.message || '写入文件失败');
          toast(options.okText + '：' + result.path);
          return null;
        });
      });
  }

  el.exportBtn.addEventListener('click', function () {
    el.exportBtn.disabled = true;
    saveXlsx({
      url: '/api/devices/export',
      filename: '设备台账.xlsx',
      title: '导出设备台账',
      fail: '导出失败',
      okText: '已导出 设备台账.xlsx'
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
    saveXlsx({
      url: '/api/devices/template',
      filename: '设备导入模板.xlsx',
      title: '保存设备导入模板',
      fail: '下载模板失败',
      okText: '已下载 设备导入模板.xlsx，可直接上传（空模板导入 0 行）'
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
    var device = devicesState.items.filter(function (item) { return item.id === id; })[0];
    if (!device) return;
    if (button.getAttribute('data-action') === 'edit') {
      openDrawer(device);
    } else {
      openDelete(device);
    }
  });

  el.rows.addEventListener('change', function (event) {
    var check = event.target;
    if (!check.classList || !check.classList.contains('row-check')) return;
    toggleSelection(Number(check.getAttribute('data-id')), check.checked);
  });

  el.selectAll.addEventListener('change', function () {
    var checked = el.selectAll.checked;
    devicesState.items.forEach(function (item) {
      toggleSelection(item.id, checked);
    });
  });

  el.printSelected.addEventListener('click', function () {
    if (selectionCount() === 0) return;
    // 快照一份再进打印页：返回台账继续勾选不会影响已经生成的打印任务。
    printState.ids = devicesState.selectedIds.slice();
    setView('print', true);
  });

  el.form.addEventListener('submit', submitDeviceForm);

  // 新增/编辑共用一个表单；409 冲突时仅管理员可强制覆盖，覆盖会写审计。
  function submitDeviceForm(event) {
    event.preventDefault();
    var payload = {
      dev_name: el.devName.value.trim(),
      location: el.devLocation.value.trim(),
      control_range: el.devRange.value.trim(),
      phone: el.devPhone.value.trim()
    };
    var editingId = devicesState.editingId;
    // 乐观锁：编辑必须带回打开抽屉时的版本号，服务端版本不一致会返回 409。
    if (editingId) payload.version = devicesState.editingVersion;
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
          if (!editingId) devicesState.page = 0;
          return load();
        });
      })
      .catch(function (error) {
        if (handleAuthFailure(error)) {
          closeDrawer();
          return;
        }
        if (error.status === 409) {
          return handleDeviceConflict(editingId, payload);
        }
        el.formError.textContent = error.message;
        el.formError.hidden = false;
      });
  }

  // 409：只有管理员能强制覆盖；覆盖会抹掉别人刚保存的内容，所以文案要写清后果并写审计。
  function handleDeviceConflict(editingId, payload) {
    var canForce = editingId && appState.user && appState.user.role === 'admin';
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

  el.cancelDelete.addEventListener('click', closeDelete);

  el.confirmDelete.addEventListener('click', handleConfirmDelete);

  function handleConfirmDelete() {
    var id = devicesState.pendingDeleteId;
    var version = devicesState.pendingDeleteVersion;
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
  }

export { bytesToBase64, closeDelete, closeDrawer, downloadInBrowser, fillSelect, handleConfirmDelete, handleDeviceConflict, handleImportFile, load, loadOptions, openDelete, openDrawer, queryString, renderPager, renderRows, saveXlsx, shellSaveFile, submitDeviceForm, syncFiltersFromInputs, totalPages };
