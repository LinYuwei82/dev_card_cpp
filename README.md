# DevCard · 设备管理卡制作系统

物业设备台账与设备管理卡打印工具：维护设备名称 / 位置 / 控制范围 / 维护电话，按条件筛选后把结果排成 A4 版面（每页 3 列 × 4 行，共 12 张卡片）导出 PDF 打印。

> **当前状态（重要）**
> 仓库只保留新架构线（C++20 + SQLite + 系统 WebView）：**M0–M4.2 完成**（M4 的 G2 实物叠图与打印面板仍待验），**M5 第一段已落地**（版本检查接口与「有新版本」提示、macOS 一条命令出 DMG 并实测）；**M5 第二段：Windows 壳已编译，`/VERYSILENT` 安装 + 卸载实测通过**（向导页、实物打印、SmartScreen 等仍待补）。**M6 的「检查 + 告知」已交付**（含 CSP `connect-src` 根因修复与断言）；自更新已拍板「下载 + 校验 + 替换 + 重启」，**阶段一（下载 + SHA256/字节数校验）已完成（`61fa9e2` + `f706b76`），阶段二、三未动**，见 [改造技术方案 §10.4.2](docs/改造技术方案.md)。
>
> 开发期版本为 `0.x`（仅内部）；正式对外版本为 **1.2.0**，唯一来源是仓库根目录的 `VERSION.txt`（server / Windows 版本资源 / macOS bundle 都从它读取）。
>
> 改造前的旧 Qt 线已从**仓库历史**删除（不是仅工作区删除）：历史已重写，旧 commit hash 一律不可引用，`v1.1.3` tag（本地与两个远端）已删，`backup-m7-*` 分支已删；`src/`、`resources/`、`dev_card.pro`、`build.bat`、`DevCard_resource.rc`、`packaging/mac/*`、`make_portable_win.ps1` 与 6 份旧线文档在仓库与历史里都取不到。旧版数据不再支持导入，迁移路径已整体删除（详见 [改造技术方案 §0.1](docs/改造技术方案.md)）。

## 1. 项目定位

| 项 | 说明 |
|---|---|
| 使用者 | 物业设备管理员（单机、单人日常使用） |
| 核心流程 | 台账维护 → 按名称/位置筛选或按字段查询 → 生成卡片 PDF → 打印塑封上墙 |
| 数据规模 | 数十到数千条设备记录；单文件 SQLite |
| 部署形态 | Windows 安装版；macOS `.app` / DMG，数据在 `~/Library/Application Support/Anye/DevCard/` |
| 平台 | macOS 15+（Apple Silicon）、Windows 10 1809+ |

功能范围（现状）：

- 设备台账增、删、改、清空、合并重复
- 按名称、位置两个下拉框组合筛选；按设备名称/位置/控制范围/维护电话模糊查询
- Excel（`.xlsx`）导入导出
- 按当前筛选或查询结果生成设备管理卡 PDF（A4，3×4 版式）
- 用户维护（新增、改密、删除）

---

## 2. 构建与运行

```bash
# 构建（macOS / Linux / Windows 的 CMake 工具链）
cmake -S . -B build-new
cmake --build build-new -j8
ctest --test-dir build-new --output-on-failure
```

- **单机版（桌面壳）**：macOS 构建后运行 `build-new/shell/DevCard.app`（或 `open build-new/shell/DevCard.app`）。壳内嵌启动本地 HTTP 服务并加载页面，界面表现为普通桌面窗口。
- **网络版（浏览器）**：运行 `build-new/server/devcard-server --port 18081`，浏览器访问 `http://127.0.0.1:18081/`。目前只有本地监听与浏览器访问，`--server` 独立的局域网服务模式属 M7，尚未实现。
- **数据目录**：macOS `~/Library/Application Support/Anye/DevCard/`（代码实值，见 `server/src/platform_paths.cpp`）；Windows 安装版 `%LOCALAPPDATA%\DevCard\data`、便携版 exe 旁 `data\`（显式覆盖优先，形态默认排最后，候选都不可写时报 `data_dir_unwritable`，不回退临时目录）。库文件 `devicedb.db`，自定义图标在 `assets/` 下（不进程序包、不进仓库）。**旧版数据不再支持导入**：数据目录里若是旧版库（只有 `tb_device`/`tb_user`、无 `schema_migration`），启动直接报 `legacy_db` 并拒绝在原库上建表；导入器与 `--import-legacy` 已删除。
- **版本**：只改根目录 `VERSION.txt`（当前 `1.2.0`；对外发布版本即以此为准），server 版本串、Windows 版本资源、macOS bundle 版本自动跟随。
- **验证打印**：界面上「卡片打印」→「导出 PDF」/「直接打印」；自动化导出可用环境变量 `DEVCARD_PRINT_SELFTEST_PDF=<输出路径>` 启动壳程序，导出后自动退出。
- **版本检查（M5）**：`GET /api/version` 读取数据目录下的 `update.json`（`{"latest":"1.2.0","download_url":"…","notes":"…"}`，由发布方投放），顶栏显示「有新版本 vX」，点击打开下载页；**不自动下载、不自动替换**。清单缺失或非法时只不显示提示，不影响台账。详见 [改造技术方案 §10.4.1](docs/改造技术方案.md)。
- **远端检查（M6，已交付）**：页面侧 `fetch` 走 `/api/version` 下发的 `check{enabled,endpoint,interval_hours}`（同来自 `update.json`），超时 / DNS 失败 / 非 JSON / 版本号非法四类失败与「没有新版本」界面不可区分。`/` 的 CSP 已显式放行 `connect-src 'self' https: http:`——缺它时 WebKit 只报 `TypeError: Load failed`，会被误判成网络故障。自更新阶段一已完成：更新弹窗里的「下载更新包」把包下载到数据目录 `updates/` 并校验 SHA256 + 字节数，**只下载、不替换**；**阶段二、三未动**。边界：下载器是系统 `curl.exe`（8.22.0 / Schannel），不是 WinHTTP/WinINet，只读 `HTTP_PROXY`/`HTTPS_PROXY`/`ALL_PROXY`；实测链路 `github.com` →302→ `release-assets.githubusercontent.com`（第 2 跳换主机）。阶段一只用假 curl + 本地清单验过，真实下载仅验到「真实进程按这条链去拉」，没有完整下完并校验；**换下载器必须重验白名单与代理口径**。
- **账号与密码**：首次启动生成 `root` 与 `安业物业` 两个随机密码账号，写入数据目录 `初始账号密码.txt`（0600），全部改密后自动删除；首次登录与管理员重置不要求旧密码（服务端按 `must_change_password` 判定），自助改密仍验旧密码；密码框支持明文/掩码切换。
- **公司名**：录入上限 16 个字符，卡面仍按 13 字宽截断；界面实时预览「将打印为：……」，超出时明确告知会截断。13 字与 16 字两张样张已实测等价：132,718 B / 2 页 / 12 + 5 / 跨页 0 / 公司名行 `x1-x0` = 44.35 mm，16 字那张截掉末尾「分公司」，整份文本层逐字相同。
- 新增依赖全部内嵌（cpp-httplib、miniz、sqlite amalgamation），不需要 Qt；Windows 侧新架构壳**已编译，`/VERYSILENT` 安装与卸载实测通过**，交互与实物打印仍待补（§10.9）。
- **Windows**：先跑 `powershell -ExecutionPolicy Bypass -File shell/win/fetch-webview2.ps1` 取 WebView2 SDK（固定版本 + SHA256，不入库），再 `cmake -S . -B build-win` + 构建；要求 VS2022 Build Tools 与 WebView2 Runtime。安装/卸载已按 `/VERYSILENT` 跑通；向导页、SmartScreen、WebView2 Runtime 缺失场景、导出桥与打印实物仍按 [§10.9](docs/改造技术方案.md) 逐条验证后再宣称可用。

### 2.1 macOS 打包（一条命令）

```bash
tools/package/make_dmg.sh                # 构建 + 依赖自检 + ad-hoc 签名 + DMG
tools/package/make_dmg.sh --skip-build   # 用已有 build-new 产物重打包
```

- 版本读 `VERSION.txt`，并与 `Info.plist` 的 `CFBundleShortVersionString` 核对；不一致直接失败。
- **依赖自检按 `otool -L` 实测清单**，不写死文件列表：系统库只核对 `/usr/lib/`、`/System/Library/` 前缀（共享缓存里的库磁盘上没有文件），包外依赖必须已存在且收进 `.app`，否则失败。
- 签名按 D13 用 ad-hoc（`codesign --sign -`），`codesign --verify --deep --strict` 通过；不公证。
- **字体随包自检**：`Contents/Resources/fonts/` 必须有两个子集 TTF，且与仓库 `web/fonts/` 逐字节一致（`cmp`），否则打包失败——缺字体就会回落到系统字体，两个平台印出的卡不一致。
- 产物：`dist/DevCard-<版本>-macos-<arch>.dmg`，脚本最后打印字节数、可读体积与 SHA256；DMG 内含应用与「应用程序」快捷方式。
- 本机实测（1.2.0 + 图标，2026-11-15）：`dist/DevCard-1.2.0-macos-arm64.dmg`，**4,774,542 B（4.6M）**，SHA256 `0f026c69956781cd07d472c23e0af2b64054eee57b42820563ef243138c8d440`；App 层身份：可执行文件 `696116f4…edb868`、`Info.plist` `5dee3ad9…837d92`（包内版本 1.2.0/1.2.0）。**该值随构建与工具链变化，只作当次记录，不作跨次基线**；每次出包以脚本末尾打印的字节数与 SHA256 为准。

### 2.2 Windows 打包（绿色版）

```powershell
powershell -ExecutionPolicy Bypass -File tools\package\make_portable_win.ps1
powershell -ExecutionPolicy Bypass -File tools\package\make_portable_win.ps1 -SkipBuild
```

- 产物：`dist\DevCard-<版本>-portable.zip`，包内 `DevCard.exe`、`devcard-server.exe`、`fonts\`、`VERSION.txt`。
- 自检：两个 exe 的 `ProductVersion` 必须等于 `VERSION.txt`（改版本后未重新 configure 会在这里失败）、字体齐全且非空、包内不得出现 `*.db`。
- 绿色版数据目录为 exe 旁 `data\`，首次启动创建；升级时手动替换程序目录并保留 `data\`。
- 安装版仍用 Inno Setup 6 编译 `packaging/windows/DevCard.iss`。

---

## 3. 目录结构

```text
dev_card_cpp/
├── CMakeLists.txt            # 唯一构建入口（新架构）
├── VERSION.txt               # 版本唯一来源
├── core/                     # 业务 / DAO / 迁移 / xlsx / 设置 / 图片元数据
├── server/                   # HTTP 路由与入口 devcard-server
├── shell/                    # macOS 壳（WKWebView + 打印桥，已实测）/ Windows 壳（WebView2，已编译、静默安装实测）
├── web/                      # 原生 JS/CSS 前端（构建期内嵌进二进制）；web/fonts/ 放子集 TTF（随包进 Resources）
├── tools/fonts/              # 字体子集脚本（make_subset.sh：变量字体定字重 + pyftsubset，可 --verify）
├── tests/                    # ctest 4 条（core / web_assets / no_opposans / embed_web）+ fixture
├── tools/package/            # macOS DMG 脚本（make_dmg.sh）
├── 3rdparty/ third_party/    # SQLite amalgamation / 内嵌第三方依赖；3rdparty/fonts/noto-sans-sc/ 存原始 Noto Sans SC 变量字体与 OFL.txt
├── docs/                     # 技术文档、样张
├── dist/                     # 本地打包产物（已 gitignore）
└── build-new/ build-win/     # 构建产物（已 gitignore）
```

目标结构：`core`（纯 C++20 业务 / DAO / 迁移）+ `server`（HTTP 与入口）+ `shell`（macOS 壳已实测；Windows 壳已编译、静默安装实测）+ `web`（原生 JS 前端，构建期内嵌）+ `tests`，详见 [改造技术方案 §4.1](docs/改造技术方案.md)。

---

## 4. 文档索引

| 文档 | 内容 | 状态 |
|---|---|---|
| [docs/改造技术方案.md](docs/改造技术方案.md) | 去 Qt 的目标架构、DAO 与迁移、UI/图标重设计、构建打包、分阶段路径、风险与工作量、Win7/8 可行性结论与决策清单 | 部分已实现（M0–M5、M6 检查 + 告知与自更新阶段一；**Windows 阶段二已端到端实测通过**；阶段三 macOS 不做、Windows 未动） |
| [docs/M4-卡片打印样张-17条-2页.pdf](docs/M4-卡片打印样张-17条-2页.pdf) | 17 条实测数据导出的完整 2 页 A4 卡片样张（新架构打印链路产物） | 实测样张 |

改造决策：D1–D13 **全部已定** —— 不支持 Win7/8（旧线已从仓库历史删除，存量客户不再获得更新，也无法从本仓库重建旧版）；打印双通道（导出 PDF + 直接打印，两通道实物叠图均 ≤ 1 mm 为验收门槛；libharu 已随旧线删除，不再是待删项）；原生 JS/CSS + 两个通用件（实际未拆，全在 `app.js`）；单进程；导入保持现状（同一文件导入两次即翻倍）；macOS 15+（三方库按 15.0 构建，13/14 不支持）；SQL 主要在 DAO（`settings`/`audit`/`backup`/`heartbeat` 仍有裸 SQL，未完全收口）+ 方言 helper；网络版最小可用；Argon2id + 强制改密 + 命令行重置；深青视觉；macOS 只出 DMG；Windows 出安装版 + 绿色版（安装器 `packaging/windows/DevCard.iss` 在 Windows 线 `win/mac-build-scripts`，本分支没有该目录；已实测静默安装；绿色版打包脚本尚未在新架构上实现）；macOS 暂不公证，继续 ad-hoc 签名；字体改用思源黑体 / Noto Sans SC 子集。**旧版数据不再支持导入**（迁移路径与导入器已删除）。详见 [改造技术方案 §14](docs/改造技术方案.md)。

---

## 5. 已知限制

以下均为**现状**，不是目标形态：

- macOS（Apple Silicon，macOS 15+）与 Windows 可运行；macOS 版未公证，首次打开需手动放行。
- macOS 图标已随包（`shell/mac/DevCard.icns`，DC 字标）；Windows `.ico`（`shell/win/DevCard.ico`）与 web favicon（`web/favicon.ico`）也已接入，三者由 `tools/package/make_icons.sh` 从同一份母版生成。
- Windows 安装器 `packaging/windows/DevCard.iss`（每用户安装、`PrivilegesRequired=lowest`、不注册服务、卸载只删 `{app}` 并在结束时提示数据目录）**已编译并实测 `/VERYSILENT` 安装/卸载**；向导页未逐页点，SmartScreen 与 WebView2 Runtime 缺失场景仍待验。绿色版打包脚本为 `tools/package/make_portable_win.ps1`（产物 `DevCard-<版本>-portable.zip`，打包前自检版本与内容）。
- **卸载不删数据目录**：安装版卸载只移除程序文件与快捷方式，数据目录保留，结束页提示路径。
- CI 已接 GitHub Actions（`.github/workflows/ci.yml`）：Windows job 取 WebView2 SDK 后构建、跑 4 条 `ctest`（core / web_assets / no_opposans / embed_web），并打绿色包上传 artifact；macOS job 构建并跑同一组测试。许可证为 **AGPL-3.0**（全文见 [LICENSE](LICENSE)），如需商业授权请另行联系作者。
- 源码注释提到的 Python 原版不在本仓库，历史行为未逐条比对。

**当前已知边界（照实写）**：

- **M0–M4.2 完成、M5 第一段完成（macOS）、M5 第二段 Windows 壳已编译并静默安装、M6 检查 + 告知与自更新阶段一完成**；M4 的 G2「两条通道实物叠图 ≤ 1 mm」尚未验收（**libharu 已随旧线整体删除**，仓库里没有回退实现，G2 只剩验收意义）。旧 Qt 线已从仓库历史删除（历史已重写、tag 与 `backup-m7-*` 分支已删），旧 commit hash 不可引用。
- **更新能力**：`/api/version` 读本地 `update.json`；远端检查在页面侧 `fetch`（`check_*` 三项配置），失败静默。自更新已拍板为「下载 + 校验 + 替换 + 重启」三阶段：**阶段一（下载到数据目录 `updates/` + SHA256 与字节数双校验）已完成，并用真实整包（48,710,744 B）跑通全链路**；**阶段二（Windows 协助器 + Inno Setup 静默升级）已在 Windows 端到端实测通过**（见 §5「已知限制」Windows 侧段落）；**阶段三已决定：macOS 本期不做自动替换**。无 Authenticode / 公证，信任锚只有「固定 HTTPS 域名 + 清单版本与哈希」，挡不住清单连同包一起被伪造，因此不写「安全校验通过」。
- **实物打印未验**：本机没有打印机，只验证了「同一份 2 页 A4 PDF 能构造打印任务」，打印面板确认与出纸需要目标打印机。
- **Windows 侧自更新阶段二已端到端实测**：协助器（`devcard-server.exe --apply-update`）通过 `CreateProcessW(DETACHED_PROCESS)` 拉起，服务器进程停止 300 ms 后 Inno Setup 以 `/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /CLOSEAPPLICATIONS /LOG=` 静默替换，完成后读取 PE 版本写 `install-result.json`，最后拉起新版本。**1.2.0 → 1.2.1 实测闭环**：`install-result.json.exit_code=0`、`version_before=1.2.0`、`version_after=1.2.1`、磁盘 PE 号从 1.2.0 变成 1.2.1。**测试中修过一处路径 bug**：协助器内部两处 `write_apply_state` 传的是 `result.parent_path()`（即 `data_dir/updates/`），但函数会自动拼 `data_dir/updates`，导致 apply-state 被写到 `data_dir/updates/updates/`，服务器重启后读不到 Done 状态。已在 `server/src/apply_update.cpp` 修正为从 result 反推 `data_dir = result.parent_path().parent_path()`。**注意**：`DETACHED_PROCESS` 只有在父进程不处在 Job Object 时才可靠（Windows 壳 DevCard.exe 正常，PowerShell `Start-Process` 前台启动的 devcard-server 会进 Job Object，协助器随即被杀）；改 VERSION.txt 后必须重新 `cmake -S ... -B ...` configure，PE 版本号（ProductVersion）由 configure 阶段注入，增量 build 不更新。仍未做：阶段三（重启后的前端回读 install-result.json 展示）、安装向导页逐页点、实物打印与导出桥端到端、SmartScreen 实际拦截文案、WebView2 Runtime 缺失场景、交互回归、emoji 路径，清单见 [改造技术方案 §10.9](docs/改造技术方案.md)。
- **旧版数据不再支持导入**：启动时若数据目录里是旧版 `devicedb.db`（只有 `tb_device`/`tb_user`、无 `schema_migration`），新版本会直接报 `legacy_db` 并拒绝在原库上建表；迁移路径、旧库导入器与 `--import-legacy` 已整体删除，不提供自动或手工迁移入口。

## 许可证

本项目以 **GNU Affero General Public License v3.0** 发布，官方全文见 [LICENSE](LICENSE)。

Copyright (C) 2026 linyw <458499311@qq.com>

如需商业授权，请另行联系作者。
