import { api, appState, el, formatBytes, toast, updateState } from './core.js';
import { isMacShell, sendToShell } from './shell.js';

  // ---------- 自动更新（分层重构后：前端只做渲染与动作） ----------
  //
  // 分层职责（双胞胎 bug 的根治约定，改动前请先读 docs/改造技术方案.md §11）：
  //   - 检测判据：core evaluate_update（版本/build 双键、灰度、跳过、
  //     强制期限——全仓唯一一份，本文件不允许出现任何版本比较或清单解析）；
  //   - 编排：server UpdateService（定时检查、退避、状态持久化），
  //     对外只暴露 /api/update/state 与 check/download/install/skip/options；
  //   - 平台安装：win 走服务端协助器（/api/update/install），mac 走 Sparkle 桥。
  // 本文件出问题时先看 /api/update/state 返回什么，不要在页面里补判定。

  // 手动检查会触发服务端出网拉清单 + 验签，慢网络下要等几秒；
  // 静默检查只读服务端状态快照，不发外网请求。服务端 curl 自带超时，
  // 页面这边不再另设竞速定时器（旧 JS 超时守卫随检测层一起退役）。

  // 强制更新原因文案：与 Mote / 小兔的 mandatoryReasonLabel 逐字一致。
  var MANDATORY_REASON_LABELS = {
    SECURITY: '安全漏洞修复',
    PROTOCOL: '协议不兼容',
    REGULATORY: '法规或税控要求',
    MIN_SUPPORTED: '低于服务端最低支持版本'
  };

  function isAdmin() {
    return !!(appState.user && appState.user.role === 'admin');
  }

  function isWindowsUa() {
    return /Windows/i.test(navigator.userAgent || '');
  }

  function setUpdatePill(version, notes) {
    el.updatePill.textContent = '有新版本 v' + version;
    el.updatePill.title = (notes ? notes + '；' : '') +
      '点击查看更新详情（下载与安装由服务端与平台安装器完成）';
    el.updatePill.hidden = false;
  }

  // 强制是否已生效：判据来自服务端状态快照（core 计算），这里只读结论。
  // 没有可安装途径（产物与下载页都没有）时不阻塞：否则用户既升不了级
  // 又关不掉弹窗。
  function mandatoryForce(s) {
    if (!s || !s.mandatory || !s.mandatory.active) return false;
    return !!(s.artifact || s.download_page);
  }

  // 把一份 /api/update/state 快照渲染到界面。
  // silent 路径只在「强制已到期」时弹窗，其余情况只动顶栏 pill；
  // manual 路径（用户主动点检查）无更新也要给 toast。
  function renderFromState(s, silent) {
    updateState.state = s || null;
    var available = !!(s && s.checked && s.update_available);
    if (!available) {
      el.updatePill.hidden = true;
      updateState.force = false;
      if (!silent && s && s.checked && !s.last_error) {
        toast('已是最新版 v' + (appState.version || s.version || '?'));
      } else if (!silent && s && s.last_error) {
        toast('检查更新失败：无法连接更新服务器');
      } else if (!silent && s && !s.checked) {
        toast('检查更新失败：更新服务未返回结果');
      }
      return;
    }
    setUpdatePill(s.latest || '', '');
    var force = mandatoryForce(s);
    updateState.force = force;
    if (!silent || force) openUpdateModal();
  }

  // checkUpdate(silent)：silent=true（默认，启动时）只读服务端状态；
  // silent=false（用户点「检查更新」）触发服务端立即检查。
  function checkUpdate(silent) {
    if (silent === undefined) silent = true;
    updateState.lastCheckSilent = silent;
    if (silent) {
      return api('GET', '/api/update/state')
        .then(function (s) { renderFromState(s, true); })
        .catch(function () { el.updatePill.hidden = true; });
    }
    setUpdatePillLoading(true);
    return api('POST', '/api/update/check', {})
      .then(function (s) {
        setUpdatePillLoading(false);
        renderFromState(s, false);
      })
      .catch(function () {
        setUpdatePillLoading(false);
        toast('检查更新失败：无法连接服务器');
      });
  }

  function setUpdatePillLoading(loading) {
    if (el.updateCheckBtn) el.updateCheckBtn.disabled = !!loading;
    if (loading && el.updatePill.hidden) {
      el.updatePill.textContent = '正在检查更新…';
      el.updatePill.hidden = false;
    } else if (!loading && el.updatePill.textContent === '正在检查更新…') {
      el.updatePill.hidden = true;
    }
  }

  // 手动更新的步骤要按平台写清楚：只说「有新版本」等于没说。
  function updateSteps() {
    if (isMacShell()) {
      return [
        '点「用 Sparkle 安装并重启」：版本判定已由服务端完成，' +
          'Sparkle 只负责下载、验签、替换与重启。',
        '在 Sparkle 窗口里确认安装，App 会自动替换并重启。',
        '台账数据在用户数据目录，升级不会动它；也可用菜单栏「DevCard → 检查更新…」。'
      ];
    }
    if (isWindowsUa()) {
      return [
        '点「下载更新包」：服务端拉取的是已验签的清单产物，下载后做 SHA256 校验。',
        '下载完成后点「安装并重启」：程序退出，安装协助器静默覆盖旧版并自动重启。',
        '台账数据在用户数据目录，升级不动它；升级结果下次打开时会在下方回读。'
      ];
    }
    return [
      '点「下载更新包」下载并校验，或按下载页说明手动下载对应平台的安装包。',
      '在运行服务的机器上替换旧版本并重启服务；台账数据在服务端数据目录，不会随升级丢失。'
    ];
  }

  // 弹窗数据全部来自状态快照：没有本地/远端两套来源，也没有页面侧判定。
  function updateModalInfo() {
    var s = updateState.state || {};
    var page = /^https:\/\//i.test(String(s.download_page || ''))
      ? String(s.download_page) : '';
    return {
      state: s,
      latest: s.latest || '',
      url: page,
      notes: '',
      mandatory: !!(s.mandatory && s.mandatory.declared),
      active: !!(s.mandatory && s.mandatory.active),
      reason: (s.mandatory && s.mandatory.reason) || '',
      deadline: (s.mandatory && s.mandatory.deadline) || '',
      artifact: s.artifact || null
    };
  }

  // 标题区的版本号、更新说明与操作步骤。
  function renderUpdateSummary(info) {
    updateState.url = info.url;
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

  // 强制更新：截止前仍可「稍后」，到期后弹窗不可关闭。
  // 「已到期」的结论来自服务端（core 判据），这里不做期限比较。
  function renderUpdateMandatory(info) {
    var force = mandatoryForce(info.state) && !!updateState.url;
    updateState.force = force;
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

  // 下载/安装按钮的可用性：产物由服务端判定快照给出（已验签、已选平台），
  // 页面不再挑资产、不再比版本、不再解析文件名。
  function renderUpdateAssets(info) {
    updateState.downloading = false;
    if (updateState.timer) {
      clearTimeout(updateState.timer);
      updateState.timer = null;
    }
    var s = info.state;
    var canSparkle = isMacShell();
    var isWindows = isWindowsUa();
    var hasArtifact = !!info.artifact;
    // macOS 桌面壳的下载与安装都在 Sparkle 里完成，前端不给下载入口。
    el.updateDownloadPackage.disabled = canSparkle ||
      !(hasArtifact && isAdmin());
    if (canSparkle) {
      setUpdateProgress('macOS 走 Sparkle 自动更新：点下方「用 Sparkle 安装并重启」，' +
        '在系统弹出的窗口中完成下载、验签与安装。');
    } else if (!hasArtifact) {
      setUpdateProgress('发布清单里没有当前平台的安装包，请用下载页手动更新。');
    } else if (!isAdmin()) {
      setUpdateProgress('只有管理员可以下载更新包，请用下载页手动更新。');
    } else {
      setUpdateProgress('');
    }
    renderUpdateInstallControls({
      isAdmin: isAdmin(),
      isWindows: isWindows,
      canSparkle: canSparkle,
      hasArtifact: hasArtifact
    });
  }

  // 安装按钮：Windows 走服务端协助器（需先下载完成），macOS 交给 Sparkle。
  function renderUpdateInstallControls(ctx) {
    el.updateInstall.textContent = ctx.canSparkle ? '用 Sparkle 安装并重启' : '安装并重启';
    el.updateInstall.hidden = !(ctx.isAdmin && (ctx.isWindows || ctx.canSparkle));
    // Windows：必须先把包下载完（服务端 install 路由会复核磁盘上的包）。
    el.updateInstall.disabled = !ctx.canSparkle && !ctx.hasArtifact;
    updateState.installing = false;
    if (!ctx.isWindows || !ctx.isAdmin) return;
    // 服务端若已有「下载并校验完成」的包（同一次运行里关掉弹窗再打开），
    // 直接放开「安装并重启」，不必重新下载一遍。
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

  // 打开弹窗：先取一次最新状态，再交给渲染函数，最后展示。
  function openUpdateModal() {
    var show = function () {
      var info = updateModalInfo();
      renderUpdateSummary(info);
      renderUpdateMandatory(info);
      renderUpdateAssets(info);
      loadInstallResult();
      el.updateModal.hidden = false;
    };
    // 弹窗打开时刷新一次状态（读缓存，不触发外网），失败就用现有快照渲染。
    api('GET', '/api/update/state').then(function (s) {
      updateState.state = s || updateState.state;
      show();
    }).catch(show);
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
    if (!isAdmin()) return;
    // 安装结果只属于 Windows 安装器；macOS 的安装进度在 Sparkle 自己的窗口里。
    if (!isWindowsUa()) return;
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
    // macOS：Sparkle 只当安装执行器——版本判定已由服务端完成，
    // 这里触发 Sparkle 完成下载、验签、替换与重启。
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
    // Windows / 服务端安装器（协助器静默覆盖并重启）
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

  // 顶栏「检查更新」按钮：三端统一走服务端编排层立即检查。
  // （macOS 不再把检查转交 Sparkle：Sparkle 只保留安装执行器角色，
  //  避免同一台机器上出现第二个检测源。）
  if (el.updateCheckBtn) {
    el.updateCheckBtn.addEventListener('click', function () {
      checkUpdate(false);
    });
  }

  // 「稍后」= 服务端跳过此版本 24 小时（判据与窗口都在编排层，页面不存状态）。
  el.updateLater.addEventListener('click', function () {
    var s = updateState.state || {};
    var close = function () { closeUpdateModal(); };
    if (!s.latest) { close(); return; }
    api('POST', '/api/update/skip', { version: s.latest, hours: 24 })
      .then(function (next) {
        updateState.state = next || s;
        el.updatePill.hidden = true;
        close();
      })
      .catch(close);
  });

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

  // 下载不再由页面携带 spec：服务端用判定快照里的产物（已验签），
  // 页面最多给一个 save_path。
  function startUpdateDownload(savePath) {
    updateState.downloading = true;
    el.updateDownloadPackage.disabled = true;
    setUpdateProgress('正在准备下载…');
    var payload = {};
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
    if (updateState.downloading) return;
    var s = updateState.state || {};
    var artifact = s.artifact || {};
    var macBridge = window.webkit && window.webkit.messageHandlers &&
                    window.webkit.messageHandlers.devcard;
    // pickSavePath 仅 macOS 壳实现（NSSavePanel）：Windows 壳与浏览器
    // 直接走服务端下载（默认落点 updates/）。
    if (!window.__devcardShell || !macBridge ||
        (window.chrome && window.chrome.webview)) {
      startUpdateDownload('');
      return;
    }
    var has_ext = /\.[A-Za-z0-9]+$/.test(artifact.name || '');
    var filename = has_ext ? artifact.name
                           : 'DevCard-' + (s.latest || '') + '.dmg';
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

  // ---------- 自检支撑（DEVCARD_SELFTEST_UPDATE） ----------
  //
  // 分层重构后自检不再驱动页面内的判定链（判定链已不在页面里）：
  // mac 壳的 update_selftest 改为直接对 /api/update/check 与
  // /api/update/state 断言。这里只保留两个观察口，供壳层截图前取界面状态。
  window.__devcardUpdateState = function () {
    return {
      version: appState.version,
      pill: el.updatePill.hidden ? '' : el.updatePill.textContent,
      state: updateState.state || null
    };
  };

  window.__devcardUpdateDiag = function () {
    var s = updateState.state || {};
    return JSON.stringify({
      user: (appState.user && appState.user.user_name) || '',
      version: appState.version || '',
      checked: !!s.checked,
      verdict: s.verdict || '',
      last_error: s.last_error || null,
      last_check_epoch: s.last_check_epoch || 0
    });
  };

export { MANDATORY_REASON_LABELS, checkUpdate, closeUpdateModal, handleUpdateDownloadPackage, handleUpdateInstall, isAdmin, loadInstallResult, mandatoryForce, openUpdateModal, openUpdateUrl, pickUpdateSavePath, pollUpdateDownload, renderFromState, renderUpdateAssets, renderUpdateInstallControls, renderUpdateMandatory, renderUpdateSummary, setUpdatePill, setUpdateProgress, startUpdateDownload, updateModalInfo, updateSteps };
