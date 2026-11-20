# DevCard · 设备管理卡制作系统

物业设备台账与设备管理卡打印工具：维护设备名称 / 位置 / 控制范围 / 维护电话，按条件筛选后把结果排成 A4 版面（每页 3 列 × 4 行，共 12 张卡片）导出 PDF 打印。

> **当前状态（重要）**
> 仓库只保留新架构线（C++20 + SQLite + 系统 WebView）：**M0–M4.2 完成**（M4 的 G2 实物叠图与打印面板仍待验），**M5 第一段已落地**（版本检查接口与「有新版本」提示、macOS 一条命令出 DMG 并实测）；**M5 第二段：Windows 壳已编译，`/VERYSILENT` 安装 + 卸载实测通过**（向导页、实物打印、SmartScreen 等仍待补）。**M6 的「检查 + 告知」已交付**（含 CSP `connect-src` 根因修复与断言）；自更新已拍板「下载 + 校验 + 替换 + 重启」：**阶段一（下载 + SHA256/字节数校验）已完成（`61fa9e2` + `f706b76`）；阶段二（Windows 替换与重启）已端到端实测通过（1.2.0 → 1.2.1）；macOS 改用 Sparkle 2.10.1，检查 → 下载 → EdDSA 验签 → 替换 → 重启全链路已端到端实测通过（1.2.0 → 1.2.1）**。
>
> **1.2.1（2026-11-17，tag `v1.2.1`）**：macOS 包为 Developer ID 签名 + 公证 + staple，Windows 安装包与绿色包、appcast、update.json 均已上线。
>
> **1.2.2（2026-11-17，tag `v1.2.2`）**：① 全套图标从 SF Pro Black 母版重生，macOS `.icns` **首次由 `tools/package/make_icons.sh` 生成**（此前是旧母版定稿）；② Sparkle 自更新补齐用户入口——macOS 菜单栏「DevCard → 检查更新…」+ 后台定时检查、Web 顶栏「检查更新」按钮（Windows 壳与浏览器可见，macOS 壳隐藏）、Windows 菜单栏「帮助 → 检查更新 / 关于 DevCard」；③ `checkUpdate(silent)` 区分启动静默检查与用户主动触发（主动触发：有更新直接弹窗、无更新 toast，且跳过节流）；④ 修 macOS 点窗口红叉退不出（`PrintBridge` 未挂 `window.delegate` / `NSApp.delegate`）；⑤ 修 Windows 启动失败时 MessageBox 阻塞留下僵尸进程、自更新协助器复制错 exe（应复制 `devcard-server.exe`）与 `DETACHED_PROCESS` 导致子进程卡死；⑥ 修「打开下载页」跳 GitHub Release 导致 404（默认下载页改 `https://dl.xiaotu.online/`）；⑦ 会话落盘 `sessions.json`，进程重启不再要求重新登录；⑧「发现新版本」弹窗紧凑化。详见 [改造技术方案 §10.4.2](docs/改造技术方案.md)。
>
> 开发期版本为 `0.x`（仅内部）；当前版本为 **1.2.8**（2026-11-19 上线：macOS DMG 与 Windows 安装包/绿色包，`manifest.json` 双平台均为 `latest=1.2.8`），唯一来源是仓库根目录的 `VERSION.txt`（server / Windows 版本资源 / macOS bundle 都从它读取）。
>
> 改造前的旧 Qt 线已从**仓库历史**删除（不是仅工作区删除）：历史已重写，旧 commit hash 一律不可引用，`v1.1.3` tag（本地与两个远端）已删，`backup-m7-*` 分支已删；`src/`、`resources/`、`dev_card.pro`、`build.bat`、`DevCard_resource.rc`、`packaging/mac/*`、`package_windows.ps1` 与 6 份旧线文档在仓库与历史里都取不到。旧版数据不再支持导入，迁移路径已整体删除（详见 [改造技术方案 §0.1](docs/改造技术方案.md)）。

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
- **网络版（浏览器）**：运行 `build-new/server/devcard-server --port 18081`，浏览器访问 `http://127.0.0.1:18081/`。`--server`（可配监听地址与来源 IP、HttpOnly 会话、多实例防护、登录限流、每用户数据目录）已实现，部署与防火墙设置见 [docs/部署文档.md](docs/部署文档.md)。
- **数据目录**：macOS `~/Library/Application Support/Anye/DevCard/`（代码实值，见 `server/src/platform_paths.cpp`）；Windows 安装版 `%LOCALAPPDATA%\DevCard\data`、便携版 exe 旁 `data\`（显式覆盖优先，形态默认排最后，候选都不可写时报 `data_dir_unwritable`，不回退临时目录）。库文件 `devicedb.db`，自定义图标在 `assets/` 下（不进程序包、不进仓库）。**旧版数据不再支持导入**：数据目录里若是旧版库（只有 `tb_device`/`tb_user`、无 `schema_migration`），启动直接报 `legacy_db` 并拒绝在原库上建表；导入器与 `--import-legacy` 已删除。
- **版本**：只改根目录 `VERSION.txt`（当前 `1.2.8`，已于 2026-11-19 上线：macOS DMG 与 Windows 安装包/绿色包，`manifest.json` 双平台均为 1.2.8），server 版本串、Windows 版本资源、macOS bundle 版本自动跟随。1.2.2 已于 2026-11-17 发布并打 tag `v1.2.2`（改版本后必须重新 `cmake -S … -B …` configure，PE 与 bundle 版本在 configure 阶段注入，增量 build 不更新）。
- **验证打印**：界面上「卡片打印」→「导出 PDF」/「直接打印」；自动化导出可用环境变量 `DEVCARD_PRINT_SELFTEST_PDF=<输出路径>` 启动壳程序，导出后自动退出。
- **版本检查（M5）**：`GET /api/version` 读取数据目录下的 `manifest.json`（Schema v2 统一清单，由发布方投放：两平台同一份、Ed25519 签名，服务端用编译期内嵌公钥验签后只取 `platforms.<本平台>`），返回本平台 `latest` 与产物资产；顶栏显示「有新版本 vX」，点击打开下载页；**不自动下载、不自动替换**。清单缺失、签名失败或非法时只不显示提示，不影响台账（签名失败还会关闭远端检查）。**1.2.2 起**顶栏另有「检查更新」按钮作为主动入口（**1.2.3 起三端都显示**：macOS 壳点击经原生桥转交 Sparkle，Windows 壳与浏览器走清单检查通道），主动触发时跳过节流、有更新直接弹窗、无更新 toast；「打开下载页」优先用清单资产，没有则回落到后端 `download_page()`（默认 `https://dl.xiaotu.online/`）。详见 [改造技术方案 §10.4.1](docs/改造技术方案.md)。
- **Windows 构建号（1.3.0 起）**：CMake 可选参数 `DEVCARD_BUILD=<UTC yyyyMMddHHmm>` → `cmake/devcard_version.cmake` 的 `devcard_add_build_stamp()` 把数值打入所有 target 编译期宏 `DEVCARD_BUILD_STAMP` / `DEVCARD_BUILD_STAMP_STR` → 客户端 `compiled_build_number()` 读取。同版本重发时清单 `build > 本机构建号` 才提示更新（未注入时返回 0，自动回退到只比 version）。发布脚本三端对齐：UTC 时间戳 → CMake configure 注入 → 二进制 → manifest.json build 字段三值一致；本机 Windows 验证脚本 `tools/release/verify_build_stamp.ps1`（PowerShell 5 零依赖，T1–T7 全链路覆盖）。guard.py preflight 强制 DevCard v2 清单同时含 `platforms.macos + platforms.windows`，缺失任一段直接 `die`。
- **远端检查（M6，已交付）**：页面侧 `fetch` 走 `/api/version` 下发的 `check{enabled,endpoint,interval_hours}`（清单里已无 `check_*` 字段：地址与间隔来自编译期默认值——`…/devcard/updates/stable/manifest.json` 与 24 小时；清单验签失败时 `enabled=false`），超时 / DNS 失败 / 非 JSON / 版本号非法四类失败与「没有新版本」界面不可区分。`/` 的 CSP 已显式放行 `connect-src 'self' https: http:`——缺它时 WebKit 只报 `TypeError: Load failed`，会被误判成网络故障。自更新阶段一已完成：更新弹窗里的「下载更新包」把包下载到数据目录 `updates/` 并校验 SHA256 + 字节数，**阶段二（Windows 替换与重启）已端到端实测通过，macOS 改由 Sparkle 承担替换与重启**。边界：下载器是系统 `curl.exe`（8.22.0 / Schannel），不是 WinHTTP/WinINet，只读 `HTTP_PROXY`/`HTTPS_PROXY`/`ALL_PROXY`；实测链路 `github.com` →302→ `release-assets.githubusercontent.com`（第 2 跳换主机）。阶段一先用假 curl + 本地清单验过链路，2026-11-15 再用真实整包补齐（48,710,744 B 下完并做 SHA256/字节数双校验，45 个进度采样点）；**换下载器必须重验白名单与代理口径**。
- **macOS 自更新（Sparkle 2.10.1，已端到端实测）**：`3rdparty/sparkle/` 内嵌 framework，CMake POST_BUILD 用 `ditto` 嵌入（保留 symlink），并按官方要求删除 `XPCServices`（DevCard 不开 App Sandbox，保留会让 Sparkle 探测 `<bundle-id>-spks` 失败）。构建时 `-DDEVCARD_SPARKLE_FEED_URL=<appcast>` 同时注入 `SUFeedURL` 与 `SUPublicEDKey`，两者缺一则整段不写——普通开发构建不启用更新（此时菜单项自动隐藏、`updaterController` 为 nil）。**1.2.2 补齐的三条用户入口**：① macOS 菜单栏「DevCard → 检查更新…」的 target 绑定 `SPUStandardUpdaterController`（它自带 `checkForUpdates:` 与 `validateMenuItem:`），另有 `initWithStartingUpdater:YES` 的后台定时检查；② Web 顶栏「检查更新」按钮——三端都显示；macOS 壳点击后经原生桥转交 Sparkle（`action=checkForUpdates`，由 Sparkle 原生窗口完成检查/下载/验签/替换），Windows 壳与浏览器点击走前端 `/api/version` 的 manifest 检查通道；③ Windows 菜单栏「帮助 → 检查更新 / 关于 DevCard」，经 WebView2 `ExecuteScript` 调 `window.devcard.checkUpdate()`。前端 `checkUpdate(silent)` 区分两种模式：启动时 `silent=true` 只设顶栏 pill 且尊重 24h 节流；用户主动触发 `silent=false` 跳过节流，有更新直接弹窗、无更新弹 toast。**macOS 壳层不再跑前端清单检查**（`isMacShell` 判定），Windows 与浏览器保持原通道。appcast 由 `tools/release/generate_appcast.py` 生成，EdDSA 私钥存 `~/.config/devcard/sparkle-keys/`（不进仓库）。签名按官方要求从内到外逐层执行并禁用 `--deep`，XPC 组件加 `--preserve-metadata=entitlements`。**实测闭环（1.2.0 → 1.2.1）**：`didFindValidUpdate` → `didDownloadUpdate` → `willInstallUpdate`，重启后 `CFBundleShortVersionString=1.2.1`、`codesign --verify --deep --strict` 通过。界面已中文化（`CFBundleLocalizations=[zh_CN,en]`）。**注意**：本机系统代理对 OSS（香港）大文件限速明显，实测用本地 HTTP 源替代以排除网络因素；下载慢属基础设施问题，与集成无关。
- **账号与密码**：首次启动生成 `root` 与 `安业物业` 两个随机密码账号，写入数据目录 `初始账号密码.txt`（0600），全部改密后自动删除；首次登录与管理员重置不要求旧密码（服务端按 `must_change_password` 判定），自助改密仍验旧密码；密码框支持明文/掩码切换。
- **接口契约测试（1.2.8 起）**：`tests/test_api_contract.cpp` 随 `ctest` 运行——进程内起真实服务，锁死认证闸门顺序（含 `/api/session/*` 免认证 + 免 CSRF 双白名单）、乐观锁 409 响应形状、`/api/version` 字段集，并把前端 `app.js` 的全部 `/api/` 字面量与服务端路由契约表比对；改路由名或响应字段而没同步前端时，测试直接红。
- **会话持久化（1.2.2 起）**：登录会话落盘 `data_dir/sessions.json`（TTL 12 小时 = `kSessionTtlSeconds = 43200`），进程重启后旧 cookie 仍有效，不再每次重启都弹登录页；`last_seen` 由 `steady_clock` 改为 Unix epoch 秒（`steady_clock` 进程重启会重置且无法序列化）。创建 / 销毁 / 按用户失效会话与恢复备份后自动重写该文件；滑动 TTL 只改内存（性能考虑），所以 `last_seen` 落盘值有滞后。
- **公司名**：录入上限 16 个字符，卡面仍按 13 字宽截断；界面实时预览「将打印为：……」，超出时明确告知会截断。13 字与 16 字两张样张已实测等价：132,718 B / 2 页 / 12 + 5 / 跨页 0 / 公司名行 `x1-x0` = 44.35 mm，16 字那张截掉末尾「分公司」，整份文本层逐字相同。
- 新增依赖全部内嵌（cpp-httplib、miniz、sqlite amalgamation），不需要 Qt；Windows 侧新架构壳**已编译，`/VERYSILENT` 安装与卸载实测通过**，交互与实物打印仍待补（§10.9）。
- **Windows**：先跑 `powershell -ExecutionPolicy Bypass -File shell/win/fetch-webview2.ps1` 取 WebView2 SDK（固定版本 + SHA256，不入库），再 `cmake -S . -B build-win` + 构建；要求 VS2022 Build Tools 与 WebView2 Runtime。安装/卸载已按 `/VERYSILENT` 跑通；向导页、SmartScreen、WebView2 Runtime 缺失场景、导出桥与打印实物仍按 [§10.9](docs/改造技术方案.md) 逐条验证后再宣称可用。

### 2.1 macOS 打包（一条命令）

```bash
tools/release/publish_macos.sh           # 出包 → 签名 → appcast/manifest.json → 上传 OSS
tools/release/publish_macos.sh --notes "本次更新内容"
tools/release/publish_macos.sh --dry-run # 只出包与清单，不上传

tools/package/package_macos.sh                # 只出 DMG：构建 + 依赖自检 + Developer ID 签名 + 公证 + staple
tools/package/package_macos.sh --skip-build   # 用已有 build-new 产物重打包
tools/package/package_macos.sh --feed-url https://dl.xiaotu.online/devcard/updates/stable/appcast.xml
```

- 版本读 `VERSION.txt`，并与 `Info.plist` 的 `CFBundleShortVersionString` 核对；不一致直接失败。
- **Sparkle 自检**：feed 默认 `https://dl.xiaotu.online/devcard/updates/stable/appcast.xml`，可用 `--feed-url` 或环境变量 `DEVCARD_SPARKLE_FEED_URL` 覆盖；打包时核对 `Info.plist` 的 `SUFeedURL` 与 `SUPublicEDKey`，缺失或不一致直接失败，避免发出「永远检查不到更新」的包。
- **依赖自检按 `otool -L` 实测清单**，不写死文件列表：系统库只核对 `/usr/lib/`、`/System/Library/` 前缀（共享缓存里的库磁盘上没有文件），包外依赖必须已存在且收进 `.app`，否则失败。
- 签名按 D13（2026-11-17 变更）用 Developer ID Application，逐层签、禁用 `--deep`，`codesign --verify --deep --strict` 通过；DMG 经 `notarytool submit --wait` 公证 + `stapler staple`，`spctl --assess` 显示 `Notarized Developer ID`。公证凭证存 Keychain profile `devcard-notary`，不进脚本与仓库。
- **字体随包自检**：`Contents/Resources/fonts/` 必须有两个子集 TTF，且与仓库 `web/fonts/` 逐字节一致（`cmp`），否则打包失败——缺字体就会回落到系统字体，两个平台印出的卡不一致。
- 产物：`dist/DevCard-<版本>-macos-<arch>.dmg`，脚本最后打印字节数、可读体积与 SHA256；DMG 内含应用与「应用程序」快捷方式。
- 本机实测（1.2.0 + 图标，2026-11-15）：`dist/DevCard-1.2.0-macos-arm64.dmg`，**4,774,542 B（4.6M）**，SHA256 `0f026c69956781cd07d472c23e0af2b64054eee57b42820563ef243138c8d440`；App 层身份：可执行文件 `696116f4…edb868`、`Info.plist` `5dee3ad9…837d92`（包内版本 1.2.0/1.2.0）。**该值随构建与工具链变化，只作当次记录，不作跨次基线**；每次出包以脚本末尾打印的字节数与 SHA256 为准。
- 本机实测（1.2.1，2026-11-17，正式发布物）：`dist/DevCard-1.2.1-macos-arm64.dmg`，**5,924,494 B（5.6 MiB）**，SHA256 `0be2a2e86bd429eaf1f8f65c4967be663fc84dfe42f55beaea71af8a7db189b2`；公证 `Accepted`（id `37039d28-249d-4a15-8a9b-0610a5d4706d`）、`stapler validate` 通过、`spctl` 为 `accepted` / `source=Notarized Developer ID`。比 1.2.0 大约 1.13 MB，原因是 1.2.0 包内没有 `Sparkle.framework`。
- 本机实测（1.2.2，2026-11-17，正式发布物，构建于提交 `1d11686`）：`dist/DevCard-1.2.2-macos-arm64.dmg`，**5,968,008 B（5.69 MiB）**，SHA256 `8e51a830d5a30ae5e68b4dd71ef1cfd205ac56785a2bcba4eca5146298af0a48`；公证 `Accepted`（id `3f8d69d9-17f1-4acc-9e23-184f36893953`）、`stapler validate` 通过、`spctl` 为 `accepted` / `source=Notarized Developer ID`；线上 `Content-Length` 与本地一致、appcast 内 `sparkle:edSignature` 用公钥 `openssl pkeyutl -verify` 通过。**该包不含其后 4 个提交**（`47f60fb` Windows 自动更新安装后壳层不退出、`b55f966` Windows 僵尸进程与协助器、`2d75aaa` 更新弹窗紧凑化、`ed747a9` 下载页 URL / 会话持久化）——其中 `2d75aaa`、`ed747a9` 含 `web/` 与 `server/` 改动，**要对 macOS 客户生效需重新出包**。

### 2.2 Windows 打包（绿色版 + 安装版）

```powershell
# 一条命令发布：绿色包 → 安装包 → manifest.json（platforms.windows）→ 上传 OSS
powershell -ExecutionPolicy Bypass -File tools\release\publish_windows.ps1
powershell -ExecutionPolicy Bypass -File tools\release\publish_windows.ps1 -Notes "本次更新内容" -DryRun

# 分步执行
# 绿色版
powershell -ExecutionPolicy Bypass -File tools\package\package_windows.ps1
# 安装版（Inno Setup 6；SourceRoot 指向含两个 exe 与 fonts\ 的暂存目录）
ISCC.exe /DSourceRoot=<暂存目录> packaging\windows\DevCard.iss
# 更新清单：由 publish_windows.ps1 调 tools/release/manifest_v2.py 生成并签名
# （dist\manifest.json；tools\package\make_update_json.ps1 是 v1 遗留脚本，发布流程不再使用）
```

- 绿色版产物：`dist\DevCard-<版本>-win64-portable.zip`，包内 `DevCard.exe`、`devcard-server.exe`、`fonts\`、`VERSION.txt`。
- 自检：两个 exe 的 `ProductVersion` 必须等于 `VERSION.txt`（改版本后未重新 configure 会在这里失败）、字体齐全且非空、包内不得出现 `*.db`。
- 绿色版数据目录为 exe 旁 `data\`，首次启动创建；升级时手动替换程序目录并保留 `data\`。
- 安装版产物：`dist\DevCard-<版本>-setup.exe`；发布脚本用 `manifest_v2.py` 生成并签名 `dist\manifest.json`（Windows 段 `artifacts.setup` / `artifacts.portable` 记录 OSS 地址、大小与 SHA256）。上传后需同步一份到数据目录，本机服务才会读到。
- CI 的 Windows job 只构建、测试并产出绿色包与安装包；`manifest.json` 由 `release-windows` 工作流生成并上传。

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
├── tools/mac/                # macOS 构建入口（build.sh：cmake configure + build；test.sh）
├── tools/package/            # 打包工具：package_macos.sh（macOS DMG：构建 + 签名 + 公证 + staple）、make_icons.sh（三端图标统一生成）、package_windows.ps1（Windows 绿色包）
├── tools/release/            # 发布入口（publish_macos.sh / publish_windows.ps1）+ 共享清单生成 manifest_v2.py + 门禁 guard.py + appcast 生成 + selftest + verify_build_stamp.ps1（Windows 构建号注入验证）
├── tests/                    # ctest 5 条（core / web_assets / no_opposans / embed_web / api_contract）+ fixture
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
| [docs/改造技术方案.md](docs/改造技术方案.md) | 去 Qt 的目标架构、DAO 与迁移、UI/图标重设计、构建打包、分阶段路径、风险与工作量、Win7/8 可行性结论与决策清单 | 部分已实现（M0–M5、M6 检查 + 告知与自更新阶段一；**Windows 阶段二已端到端实测通过**；**macOS 自更新改由 Sparkle 承担，已端到端实测通过**） |
| [docs/M4-卡片打印样张-17条-2页.pdf](docs/M4-卡片打印样张-17条-2页.pdf) | 17 条实测数据导出的完整 2 页 A4 卡片样张（新架构打印链路产物） | 实测样张 |

改造决策：D1–D13 **全部已定** —— 不支持 Win7/8（旧线已从仓库历史删除，存量客户不再获得更新，也无法从本仓库重建旧版）；打印双通道（导出 PDF + 直接打印，两通道实物叠图均 ≤ 1 mm 为验收门槛；libharu 已随旧线删除，不再是待删项）；原生 JS/CSS + 两个通用件（实际未拆，全在 `app.js`）；单进程；导入保持现状（同一文件导入两次即翻倍）；macOS 15+（三方库按 15.0 构建，13/14 不支持）；SQL 主要在 DAO（`settings`/`audit`/`backup`/`heartbeat` 仍有裸 SQL，未完全收口）+ 方言 helper；网络版最小可用；Argon2id + 强制改密 + 命令行重置；深青视觉；macOS 只出 DMG（**Developer ID 签名 + Apple 公证 + staple，2026-11 起，取代 ad-hoc**）；Windows 出安装版 + 绿色版（安装器 `packaging/windows/DevCard.iss` 在 Windows 线 `win/mac-build-scripts`，本分支没有该目录；已实测静默安装；绿色版打包脚本 `tools/package/package_windows.ps1` 已入库并实跑）；**macOS 自更新用 Sparkle 2.10.1（1.2.2 起菜单栏「检查更新…」与 Web 顶栏「检查更新」按钮两个入口，前者走 Sparkle 原生菜单、后者经原生桥转交 Sparkle）**；字体改用思源黑体 / Noto Sans SC 子集。**旧版数据不再支持导入**（迁移路径与导入器已删除）。详见 [改造技术方案 §14](docs/改造技术方案.md)。

---

## 5. 已知限制

以下均为**现状**，不是目标形态：

- macOS（Apple Silicon，macOS 15+）与 Windows 可运行；macOS 版自 2026-11-17 起用 Developer ID Application 签名并经 Apple 公证 + staple，首次打开无需手动放行；此前基于 ad-hoc 签名的旧包仍需右键「打开」放行。
- macOS 图标已随包（`shell/mac/DevCard.icns`，DC 字标）；Windows `.ico`（`shell/win/DevCard.ico`）、web `favicon.ico`、浏览器 shortcut icon（`favicon-16/32/48x*.png`）、Apple Touch（`apple-touch-icon.png`）、PWA 图标（`icons/pwa-*.png` + `icons/pwa-maskable-*.png`）与分享预览（`og-image.png`）**全部由 `tools/package/make_icons.sh` 从同一份母版统一生成**。母版已更新为 SF Pro Black 并转曲，三端几何与色彩一致。
- Windows 安装器 `packaging/windows/DevCard.iss`（每用户安装、`PrivilegesRequired=lowest`、不注册服务、卸载只删 `{app}` 并在结束时提示数据目录）**已编译并实测 `/VERYSILENT` 安装/卸载**；向导页未逐页点，SmartScreen 与 WebView2 Runtime 缺失场景仍待验。绿色版打包脚本为 `tools/package/package_windows.ps1`（产物 `DevCard-<版本>-portable.zip`，打包前自检版本与内容）。
- **卸载不删数据目录**：安装版卸载只移除程序文件与快捷方式，数据目录保留，结束页提示路径。
- CI 已接 GitHub Actions（`.github/workflows/ci.yml`）：Windows job 取 WebView2 SDK 后构建、跑 5 条 `ctest`（core / web_assets / no_opposans / embed_web / api_contract），并打绿色包上传 artifact；macOS job 构建并跑同一组测试。许可证为 **AGPL-3.0**（全文见 [LICENSE](LICENSE)），如需商业授权请另行联系作者。
- 源码注释提到的 Python 原版不在本仓库，历史行为未逐条比对。

**当前已知边界（照实写）**：

- **M0–M4.2 完成、M5 第一段完成（macOS）、M5 第二段 Windows 壳已编译并静默安装、M6 检查 + 告知与自更新阶段一完成、Windows 自更新阶段二与 macOS Sparkle 自更新均端到端实测通过**；**1.2.1 与 1.2.2 已于 2026-11-17 先后发布（tag `v1.2.1` / `v1.2.2`）**，macOS 公证包、Windows 安装包/绿色包与 appcast、更新清单（1.2.1/1.2.2 为 `update.json`，现为 `manifest.json`）均已上线。M4 的 G2「两条通道实物叠图 ≤ 1 mm」尚未验收（**libharu 已随旧线整体删除**，仓库里没有回退实现，G2 只剩验收意义）。旧 Qt 线已从仓库历史删除（历史已重写、`v1.1.3` tag 与 `backup-m7-*` 分支已删），旧 commit hash 不可引用。
- **更新能力**：`/api/version` 读数据目录 `manifest.json`（v2 统一清单，Ed25519 验签后取本平台段）；远端检查在页面侧 `fetch`（`check{enabled,endpoint,interval_hours}` 由服务端内嵌默认值下发，清单中已无 `check_*` 字段），失败静默。自更新已拍板为「下载 + 校验 + 替换 + 重启」三阶段：**阶段一（下载到数据目录 `updates/` + SHA256 与字节数双校验）已完成，并用真实整包（48,710,744 B）跑通全链路**；**阶段二（Windows 协助器 + Inno Setup 静默升级）已在 Windows 端到端实测通过**（见 §5「已知限制」Windows 侧段落）；**macOS 原定的「本期不做自动替换」已被 Sparkle 2.10.1 方案取代，检查 → 下载 → EdDSA 验签 → 替换 → 重启已端到端实测通过**。信任锚：清单统一走 Ed25519 验签（公钥随客户端编译）；**Windows 暂不购买 Authenticode 代码签名证书**（2026-11-17 决定，发布页已加 SmartScreen 提示）；macOS 另有 Developer ID 签名 + Apple 公证 + appcast 内 EdDSA 签名。
- **实物打印未验**：本机没有打印机，只验证了「同一份 2 页 A4 PDF 能构造打印任务」，打印面板确认与出纸需要目标打印机。
- **Windows 侧自更新阶段二已端到端实测**：协助器（`devcard-server.exe --apply-update`）拉起后，服务器进程退出，Inno Setup 以 `/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /CLOSEAPPLICATIONS /LOG=` 静默替换，完成后读取 PE 版本写 `install-result.json`，最后拉起新版本。**1.2.0 → 1.2.1 实测闭环**：`install-result.json.exit_code=0`、`version_before=1.2.0`、`version_after=1.2.1`、磁盘 PE 号从 1.2.0 变成 1.2.1。**2026-11-17 连修的四处缺陷（均已实测修复）**：① 协助器内部两处 `write_apply_state` 传 `result.parent_path()`（即 `data_dir/updates/`），函数又自动拼 `data_dir/updates`，状态被写到 `updates/updates/`，重启后读不到 Done——改为从 result 反推 `data_dir = result.parent_path().parent_path()`；② 安装接口只设 `stop_requested=true` 但壳模式没人轮询该标志，协助器 `WaitForSingleObject` 永久卡在 `installing`——接口改为同时 `server.stop()`，壳层加 200 ms 轮询线程检测后 `PostMessageW(WM_CLOSE)`，`WM_CLOSE` 见 `server_exit_requested` 跳过确认框；③ 协助器复制的是壳层 `DevCard.exe`（它忽略 `--apply-update`，启动后变成普通服务器）——改为复制 `devcard-server.exe`；④ `CreateProcessW` 的 `DETACHED_PROCESS` / `CREATE_NO_WINDOW` 实测会让协助器内部子进程无限等待（连 `write_apply_state(Verifying)` 都到不了）——已去掉这两个标志。另修壳层启动失败（端口占用 / 实例冲突）时 `MessageBox` 阻塞造成的僵尸进程：`Fatal()` 在 MessageBox 返回后立即 `ExitProcess(1)`，并加 `MB_TOPMOST|MB_SETFOREGROUND`。仍未做：安装向导页逐页点、实物打印与导出桥端到端、SmartScreen 实际拦截文案、WebView2 Runtime 缺失场景、交互回归、emoji 路径，清单见 [改造技术方案 §10.9](docs/改造技术方案.md)。
- **旧版数据不再支持导入**：启动时若数据目录里是旧版 `devicedb.db`（只有 `tb_device`/`tb_user`、无 `schema_migration`），新版本会直接报 `legacy_db` 并拒绝在原库上建表；迁移路径、旧库导入器与 `--import-legacy` 已整体删除，不提供自动或手工迁移入口。

## 许可证

本项目以 **GNU Affero General Public License v3.0** 发布，官方全文见 [LICENSE](LICENSE)。

Copyright (C) 2026 linyw <458499311@qq.com>

如需商业授权，请另行联系作者。
