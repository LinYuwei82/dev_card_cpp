/* DevCard 设计样张：原生 JS，无框架、无构建、无外网依赖 */
(function () {
  "use strict";

  /* ---------- 图标（内联 SVG，currentColor） ---------- */
  var paths = {
    search: '<circle cx="11" cy="11" r="7"/><path d="m20 20-3.6-3.6"/>',
    upload: '<path d="M12 16V4"/><path d="m7 9 5-5 5 5"/><path d="M4 16v3a1 1 0 0 0 1 1h14a1 1 0 0 0 1-1v-3"/>',
    download: '<path d="M12 4v12"/><path d="m7 11 5 5 5-5"/><path d="M4 18v1a1 1 0 0 0 1 1h14a1 1 0 0 0 1-1v-1"/>',
    plus: '<path d="M12 5v14M5 12h14"/>',
    edit: '<path d="M4 20h4l10-10-4-4L4 16v4Z"/><path d="m13.5 6.5 4 4"/>',
    trash: '<path d="M4 7h16"/><path d="M9 7V5h6v2"/><path d="m6 7 1 13h10l1-13"/><path d="M10 11v5M14 11v5"/>',
    print: '<path d="M7 8V3h10v5"/><rect x="4" y="8" width="16" height="9" rx="2"/><path d="M7 17v4h10v-4"/>',
    file: '<path d="M14 3H7a2 2 0 0 0-2 2v14a2 2 0 0 0 2 2h10a2 2 0 0 0 2-2V8Z"/><path d="M14 3v5h5"/><path d="M9 13h6M9 17h4"/>',
    users: '<circle cx="9" cy="8" r="3"/><path d="M3 20c0-3 2.7-5 6-5s6 2 6 5"/><path d="M16 5a3 3 0 0 1 0 6"/><path d="M18 20c0-2.4-.9-4.1-2.3-5"/>',
    card: '<rect x="3" y="5" width="18" height="14" rx="2"/><path d="M7 9h4M7 13h3"/><path d="M16 10h1" stroke-width="3"/>',
    chevronL: '<path d="m15 5-7 7 7 7"/>',
    chevronR: '<path d="m9 5 7 7-7 7"/>',
    x: '<path d="M6 6l12 12M18 6 6 18"/>',
    check: '<path d="m5 12 5 5 9-10"/>',
    alert: '<path d="M12 4 3 20h18L12 4Z"/><path d="M12 10v4M12 17h.01"/>',
    moon: '<path d="M20 14.5A8 8 0 0 1 9.5 4 8.5 8.5 0 1 0 20 14.5Z"/>',
    sun: '<circle cx="12" cy="12" r="4"/><path d="M12 2v2M12 20v2M2 12h2M20 12h2M4.9 4.9l1.4 1.4M17.7 17.7l1.4 1.4M19.1 4.9l-1.4 1.4M6.3 17.7l-1.4 1.4"/>',
    db: '<ellipse cx="12" cy="5" rx="8" ry="3"/><path d="M4 5v7c0 1.7 3.6 3 8 3s8-1.3 8-3V5"/><path d="M4 12v7c0 1.7 3.6 3 8 3s8-1.3 8-3v-7"/>',
    info: '<circle cx="12" cy="12" r="9"/><path d="M12 11v5M12 8h.01"/>'
  };
  function icon(name, cls) {
    var p = paths[name] || "";
    return '<svg class="icon ' + (cls || "") + '" viewBox="0 0 24 24" fill="none" stroke="currentColor" ' +
      'stroke-width="1.7" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true">' + p + "</svg>";
  }
  function paintIcons(root) {
    (root || document).querySelectorAll("[data-icon]").forEach(function (el) {
      el.innerHTML = icon(el.getAttribute("data-icon"), el.getAttribute("data-icon-class") || "");
    });
  }

  /* ---------- 数据（脱敏样例） ---------- */
  var devices = [
    ["消防远程断电箱", "1栋1楼", "1-3层", "13800000001"],
    ["生活水泵控制柜", "1栋2楼", "整栋", "13800000002"],
    ["排烟风机控制箱", "2栋1楼", "B1-2层", "13800000003"],
    ["稳压泵控制箱", "2栋3楼", "1-5层", "13800000004"],
    ["电梯机房配电箱", "3栋B1", "B1-1层", "13800000005"],
    ["消防喷淋泵", "3栋1楼", "整栋", "13800000006"],
    ["应急照明配电箱", "5栋2楼", "1-3层", "13800000007"],
    ["防火卷帘控制箱", "5栋3楼", "1-2层", "13800000008"],
    ["生活水箱液位控制箱", "6栋1楼", "B1-2层", "13800000009"],
    ["空气源热泵机组", "6栋2楼", "整栋", "13800000010"],
    ["柴油发电机组", "7栋B1", "B1-1层", "13800000011"],
    ["门禁控制器", "7栋1楼", "1-2层", "13800000012"]
  ].map(function (d, i) { return { id: i + 1, name: d[0], loc: d[1], range: d[2], phone: d[3] }; });

  /* ---------- 主题 ---------- */
  var themeBtn = document.getElementById("themeBtn");
  function applyTheme(t) {
    document.documentElement.dataset.theme = t;
    themeBtn.innerHTML = icon(t === "dark" ? "sun" : "moon");
    try { localStorage.setItem("dc-theme", t); } catch (e) {}
  }
  var savedTheme = "light";
  try { savedTheme = localStorage.getItem("dc-theme") || "light"; } catch (e) {}
  applyTheme(savedTheme);
  themeBtn.addEventListener("click", function () {
    applyTheme(document.documentElement.dataset.theme === "dark" ? "light" : "dark");
  });

  /* ---------- 路由 ---------- */
  var loginView = document.getElementById("loginView");
  var appView = document.getElementById("appView");
  var pages = ["devices", "users", "print", "design"];
  function route() {
    var name = (location.hash || "").replace("#", "");
    if (name === "login" || !name) {
      loginView.classList.remove("hidden");
      appView.classList.add("hidden");
      return;
    }
    if (pages.indexOf(name) < 0) name = "devices";
    loginView.classList.add("hidden");
    appView.classList.remove("hidden");
    var titles = { devices: "设备台账", users: "用户管理", print: "卡片打印", design: "设计说明" };
    document.getElementById("topTitle").textContent = titles[name] || "";
    document.querySelectorAll(".page").forEach(function (p) {
      p.classList.toggle("active", p.dataset.page === name);
    });
    document.querySelectorAll(".nav-item").forEach(function (b) {
      b.classList.toggle("active", b.dataset.nav === name);
    });
    if (name === "print") fitPaper();
  }
  window.addEventListener("hashchange", route);

  document.querySelectorAll("[data-nav]").forEach(function (b) {
    b.addEventListener("click", function () { location.hash = b.dataset.nav; });
  });
  document.getElementById("loginForm").addEventListener("submit", function (e) {
    e.preventDefault();
    location.hash = "devices";
    toast("样张：登录成功（未连接后端）");
  });
  document.querySelectorAll("[data-toast]").forEach(function (el) {
    el.addEventListener("click", function () { toast(el.dataset.toast); });
  });

  /* ---------- toast ---------- */
  var toastEl = document.getElementById("toast");
  var toastTimer;
  function toast(msg, kind) {
    toastEl.textContent = msg;
    toastEl.className = "toast show" + (kind ? " " + kind : "");
    clearTimeout(toastTimer);
    toastTimer = setTimeout(function () { toastEl.className = "toast"; }, 2600);
  }

  /* ---------- 台账 ---------- */
  var tbody = document.getElementById("deviceRows");
  var countEl = document.getElementById("deviceCount");
  var searchEl = document.getElementById("deviceSearch");
  var nameFilter = document.getElementById("nameFilter");
  var locFilter = document.getElementById("locFilter");
  var currentRows = devices.slice();

  function phoneText(p) {
    return p.length === 11 ? p.slice(0, 3) + " " + p.slice(3, 7) + " " + p.slice(7) : p;
  }
  function renderTable() {
    var kw = (searchEl.value || "").trim().toLowerCase();
    var nf = nameFilter.value, lf = locFilter.value;
    currentRows = devices.filter(function (d) {
      if (nf && d.name !== nf) return false;
      if (lf && d.loc !== lf) return false;
      if (!kw) return true;
      return (d.name + d.loc + d.range + d.phone).toLowerCase().indexOf(kw) >= 0;
    });
    countEl.textContent = currentRows.length;
    if (!currentRows.length) {
      tbody.innerHTML = '<tr><td colspan="6"><div class="empty"><strong>没有匹配的设备</strong>换个关键词，或清空筛选条件</div></td></tr>';
      return;
    }
    tbody.innerHTML = currentRows.map(function (d, i) {
      return "<tr data-id=\"" + d.id + "\">" +
        '<td class="num">' + String(i + 1).padStart(2, "0") + "</td>" +
        "<td><strong>" + d.name + "</strong></td>" +
        "<td>" + d.loc + "</td>" +
        "<td>" + d.range + "</td>" +
        '<td class="num">' + phoneText(d.phone) + "</td>" +
        '<td class="actions"><span class="row-actions">' +
          '<button class="icon-btn" data-act="edit" title="编辑">' + icon("edit") + "</button>" +
          '<button class="icon-btn danger" data-act="del" title="删除">' + icon("trash") + "</button>" +
        "</span></td></tr>";
    }).join("");
  }
  function fillOptions() {
    var names = Array.from(new Set(devices.map(function (d) { return d.name; })));
    var locs = Array.from(new Set(devices.map(function (d) { return d.loc; })));
    nameFilter.innerHTML = '<option value="">全部名称</option>' + names.map(function (n) { return '<option>' + n + "</option>"; }).join("");
    locFilter.innerHTML = '<option value="">全部位置</option>' + locs.map(function (n) { return '<option>' + n + "</option>"; }).join("");
  }
  searchEl.addEventListener("input", renderTable);
  nameFilter.addEventListener("change", renderTable);
  locFilter.addEventListener("change", renderTable);
  document.getElementById("clearFilter").addEventListener("click", function () {
    searchEl.value = ""; nameFilter.value = ""; locFilter.value = ""; renderTable();
  });

  /* ---------- 抽屉 ---------- */
  var drawer = document.getElementById("deviceDrawer");
  var editingId = null;
  function openDrawer(d) {
    editingId = d ? d.id : null;
    document.getElementById("drawerTitle").textContent = d ? "编辑设备" : "新增设备";
    document.getElementById("fName").value = d ? d.name : "";
    document.getElementById("fLoc").value = d ? d.loc : "";
    document.getElementById("fRange").value = d ? d.range : "";
    document.getElementById("fPhone").value = d ? d.phone : "";
    drawer.classList.add("open");
    document.getElementById("fName").focus();
  }
  function closeDrawer() { drawer.classList.remove("open"); }
  document.getElementById("addDevice").addEventListener("click", function () { openDrawer(null); });
  document.getElementById("drawerClose").addEventListener("click", closeDrawer);
  document.getElementById("drawerCancel").addEventListener("click", closeDrawer);
  document.getElementById("drawerSave").addEventListener("click", function () {
    var name = document.getElementById("fName").value.trim();
    if (!name) { toast("请填写设备名称", "error"); return; }
    if (editingId) {
      var d = devices.find(function (x) { return x.id === editingId; });
      d.name = name;
      d.loc = document.getElementById("fLoc").value.trim();
      d.range = document.getElementById("fRange").value.trim();
      d.phone = document.getElementById("fPhone").value.trim();
      toast("已保存（样张：未写库）");
    } else {
      toast("已新增（样张：未写库）");
    }
    closeDrawer(); renderTable(); renderCards();
  });
  tbody.addEventListener("click", function (e) {
    var btn = e.target.closest("button[data-act]");
    if (!btn) return;
    var tr = btn.closest("tr");
    var d = devices.find(function (x) { return x.id === Number(tr.dataset.id); });
    if (btn.dataset.act === "edit") openDrawer(d);
    else confirmDelete(d);
  });

  /* ---------- 删除确认 ---------- */
  var confirmModal = document.getElementById("confirmModal");
  var pendingDelete = null;
  function confirmDelete(d) {
    pendingDelete = d;
    document.getElementById("confirmText").textContent = "将删除「" + d.name + "」（" + d.loc + "）。此操作不可撤销。";
    confirmModal.classList.add("open");
  }
  document.getElementById("confirmCancel").addEventListener("click", function () {
    confirmModal.classList.remove("open"); pendingDelete = null;
  });
  document.getElementById("confirmOk").addEventListener("click", function () {
    if (pendingDelete) {
      devices = devices.filter(function (x) { return x.id !== pendingDelete.id; });
      toast("已删除（样张：未写库）");
      renderTable(); renderCards();
    }
    confirmModal.classList.remove("open"); pendingDelete = null;
  });

  /* ---------- 导入 ---------- */
  var importModal = document.getElementById("importModal");
  var fileInput = document.getElementById("importFile");
  var fileName = document.getElementById("importFileName");
  document.getElementById("importBtn").addEventListener("click", function () {
    importModal.classList.add("open");
  });
  document.getElementById("importClose").addEventListener("click", function () {
    importModal.classList.remove("open");
  });
  fileInput.addEventListener("change", function () {
    fileName.textContent = fileInput.files.length ? fileInput.files[0].name : "未选择文件（样张不会读取内容）";
  });
  document.getElementById("importStart").addEventListener("click", function () {
    importModal.classList.remove("open");
    toast("样张：解析 130 行，成功 128，跳过 2（空行）");
  });

  /* ---------- 用户页 ---------- */
  var userRows = [
    ["root", "管理员", "启用", "2026-11-14 09:12"],
    ["安业物业", "管理员", "启用", "2026-11-13 17:40"],
    ["张伟", "普通用户", "启用", "2026-11-12 08:55"],
    ["李敏", "普通用户", "已停用", "2026-10-30 16:21"]
  ];
  var userBody = document.getElementById("userRows");
  function renderUsers() {
    userBody.innerHTML = userRows.map(function (u) {
      return "<tr><td><strong>" + u[0] + "</strong></td>" +
        '<td><span class="chip ' + (u[1] === "管理员" ? "role" : "") + '">' + u[1] + "</span></td>" +
        '<td><span class="chip ' + (u[2] === "启用" ? "ok" : "warn") + '">' + u[2] + "</span></td>" +
        '<td class="num">' + u[3] + "</td>" +
        '<td class="actions"><span class="row-actions">' +
        '<button class="icon-btn" data-user="reset" title="重置口令">' + icon("edit") + "</button>" +
        '<button class="icon-btn danger" data-user="del" title="删除">' + icon("trash") + "</button>" +
        "</span></td></tr>";
    }).join("");
  }
  var userModal = document.getElementById("userModal");
  document.getElementById("addUser").addEventListener("click", function () { userModal.classList.add("open"); });
  document.getElementById("userClose").addEventListener("click", function () { userModal.classList.remove("open"); });
  document.getElementById("userCancel").addEventListener("click", function () { userModal.classList.remove("open"); });
  document.getElementById("userSave").addEventListener("click", function () {
    var n = document.getElementById("uName").value.trim();
    if (!n) { toast("请填写用户名", "error"); return; }
    userRows.push([n, "普通用户", "启用", "—"]);
    renderUsers(); userModal.classList.remove("open");
    toast("已新增用户（样张：未写库）");
  });
  userBody.addEventListener("click", function (e) {
    var b = e.target.closest("button[data-user]");
    if (!b) return;
    if (b.dataset.user === "reset") toast("样张：打开重置口令（命令行 --set-password）");
    else toast("样张：删除用户需二次确认", "warn");
  });

  /* ---------- 打印 ---------- */
  function renderCards() {
    var paper = document.getElementById("paper");
    var pageData = devices.slice(0, 12);
    paper.innerHTML = pageData.map(function (d) {
      return '<div class="card">' +
        '<div class="card-top"><div class="card-logo">安</div>' +
        '<div class="card-company">深圳市安业物业管理有限公司</div></div>' +
        '<div class="card-title">设备管理卡</div>' +
        '<div class="card-rows">' +
        '<div class="card-row"><span class="k">设备名称</span><span class="v">' + d.name + "</span></div>" +
        '<div class="card-row"><span class="k">安装位置</span><span class="v">' + d.loc + "</span></div>" +
        '<div class="card-row"><span class="k">控制范围</span><span class="v">' + d.range + "</span></div>" +
        '<div class="card-row"><span class="k">维护电话</span><span class="v">' + phoneText(d.phone) + "</span></div>" +
        "</div>" +
        '<div class="card-foot"><span>设备编号 ' + String(d.id).padStart(3, "0") + "</span><span>安业物业</span></div>" +
        "</div>";
    }).join("");
  }
  function fitPaper() {
    var box = document.getElementById("previewBox");
    var scaler = document.getElementById("paperScaler");
    if (!box || !scaler) return;
    var mm = 96 / 25.4;
    var w = 210 * mm, h = 297 * mm;
    var scale = Math.min(1, (box.clientWidth - 48) / w);
    scale = Math.max(0.35, scale);
    scaler.style.transform = "scale(" + scale + ")";
    scaler.style.height = (h * scale) + "px";
    scaler.style.width = w + "px";
  }
  window.addEventListener("resize", fitPaper);
  document.getElementById("exportPdf").addEventListener("click", function () {
    toast("样张：壳层 PrintToPdf / createPDF 生成 PDF");
  });
  document.getElementById("directPrint").addEventListener("click", function () {
    toast("样张：调用系统打印面板（此处改为浏览器打印预览）");
    setTimeout(function () { window.print(); }, 350);
  });
  document.querySelectorAll("[data-pagebtn]").forEach(function (b) {
    b.addEventListener("click", function () { toast("样张：翻页（共 11 页）"); });
  });

  /* ---------- 启动 ---------- */
  paintIcons();
  fillOptions();
  renderTable();
  renderUsers();
  renderCards();
  route();
  setTimeout(fitPaper, 60);
})();
