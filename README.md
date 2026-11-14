# DevCard · 设备管理卡制作系统

物业设备台账与设备管理卡打印工具：维护设备名称 / 位置 / 控制范围 / 维护电话，按条件筛选后把结果排成 A4 版面（每页 3 列 × 4 行，共 12 张卡片）导出 PDF 打印。

> **当前状态（重要）**
> 仓库里有两条线并行：
> 1. **旧 Qt 6 / C++17 线（v1.1.3，冻结）**：Windows + macOS（Apple Silicon，最低 macOS 15）。v1.1.3 的 macOS DMG 已挂到 Gitee / GitHub Release；Windows 绿色版 zip 尚未产出（打包脚本已就绪但未在 Windows 实跑），旧线自身没有 `.iss`（安装器属于新架构 Windows 线，见下）。**旧线仍可编译、可发版，不在本轮改动范围内**（`src/`、`dev_card.pro`、`build.bat` 一行不动）。
> 2. **新架构线（分支 `macmini/new-arch`，C++20 + SQLite + 系统 WebView）**：**当前状态：M0–M4.2 完成**（M4 的 G2 实物叠图与打印面板仍待验），**M5 第一段已落地**（版本检查接口与「有新版本」提示、macOS 一条命令出 DMG 并实测）；**M5 第二段：Windows 壳已编译，`/VERYSILENT` 安装 + 卸载实测通过**（向导页、实物打印、SmartScreen 等仍待补）。**M6 的「检查 + 告知」已交付**（含 CSP `connect-src` 根因修复与断言）；自更新已拍板「下载 + 校验 + 替换 + 重启」，**阶段一（下载 + SHA256/字节数校验）已完成（`a4cda3a` + `6f425fd`），阶段二、三未动**，见 [改造技术方案 §10.4.2](docs/改造技术方案.md)。
>
> 新架构开发期版本为 `0.x`（仅内部）；正式对外版本为 **1.2.0**，唯一来源是仓库根目录的 `VERSION.txt`（server / Windows 版本资源 / macOS bundle 都从它读取）。
> 下文凡标注「目标形态」的内容都是计划；判断旧线现状看 [现状技术方案](docs/现状技术方案.md)。

## 已发布产物（v1.1.3）

| 平台 | 产物 | 状态 |
|---|---|---|
| macOS | `DevCard-1.1.3-macos-arm64.dmg` | 已发布到 Gitee / GitHub Release；67 MiB；[SHA256](#已发布产物校验) |
| Windows | `DevCard-1.1.3-portable.zip` | 脚本就绪，尚未在 Windows 构建/打包 |
| Windows | 安装版 `setup.exe` | v1.1.3 旧线无 `.iss`；新架构 Windows 线已有 `packaging/windows/DevCard.iss`，已编译并实测 `/VERYSILENT` 安装与卸载（向导页未逐页点、`web\*` 形态待补） |

- Release：<https://github.com/LinYuwei82/dev_card_cpp/releases/tag/v1.1.3>、<https://gitee.com/lin-yuwei168/dev_card_cpp/releases/tag/v1.1.3>
- macOS 版为 ad-hoc 签名、未公证；首次打开需在 Gatekeeper 中手动放行，详见 [Release 正文](docs/release-v1.1.3.md)。
- 最低 macOS 15，对应 D7 已定口径：Homebrew 的 libharu/libzip/libxlsxwriter 均按 15.0 构建；13/14 不在支持范围（此前未发布过 macOS 版，存量客户均为 Windows）。
- tag `v1.1.3` 当前指向 `bf3e2b4`；曾从 `8b61937` 强制移动一次，原因见改造方案 §0.1。

<a id="已发布产物校验"></a>
DMG SHA256：

```text
a4aac2045131bc43aa68c78d2f8856b095f4493a52a06515c175651f28446946
```

## 1. 项目定位

| 项 | 说明 |
|---|---|
| 使用者 | 物业设备管理员（单机、单人日常使用） |
| 核心流程 | 台账维护 → 按名称/位置筛选或按字段查询 → 生成卡片 PDF → 打印塑封上墙 |
| 数据规模 | 数十到数千条设备记录；单文件 SQLite |
| 部署形态 | Windows 绿色目录：程序 + `resources/` + `data/devicedb.db`；macOS：`.app` / DMG，数据在 `~/Library/Application Support/Anye/DevCard/` |
| 平台 | v1.1.3 实际：Windows 10 1809+ / 11、macOS 15+（Apple Silicon）；改造目标：macOS 15+（D7）、Windows 10 1809+ |

功能范围（现状）：

- 设备台账增、删、改、清空、合并重复
- 按名称、位置两个下拉框组合筛选；按设备名称/位置/控制范围/维护电话模糊查询
- Excel（`.xlsx`）导入导出
- 按当前筛选或查询结果生成设备管理卡 PDF（A4，3×4 版式）
- 用户维护（新增、改密、删除）

---

## 2. 当前版本：构建与运行（Qt 6，Windows / macOS）

### 2.1 前置条件

| 依赖 | 版本/位置 | 说明 |
|---|---|---|
| Qt | 6.x（Windows：MinGW 64-bit；macOS：Homebrew Qt 6.11.1） | 需 `core gui widgets sql xml` |
| 编译器 | Windows：MinGW 13.x；macOS：Xcode Command Line Tools（arm64） | `mingw32-make` / `make` 在 `PATH` 中 |
| libharu / libzip / libxlsxwriter | Windows：`C:/msys64/mingw64/{include,lib}`；macOS：`/opt/homebrew/{include,lib}` | 头文件 `hpdf.h`、`zip.h`、`xlsxwriter.h`；运行期需要对应动态库 |
| SQLite | 仓库内 `3rdparty/sqlite-amalgamation-3460100` | 直接编译源码，无需外部安装 |

### 2.2 一键构建（推荐）

在 Windows 上双击或命令行执行：

```bat
build.bat
```

脚本会：检查 qmake 与第三方头文件 → 备份旧的 `bin/devicedb.db`（若有）→ 清理 `build/` 与 `bin/` → `qmake dev_card.pro` → 编译 → `windeployqt` → 复制 `resources/` 到 `bin/`。

> 数据库已改存到用户数据目录（Windows：`%LOCALAPPDATA%\Anye\DevCard\devicedb.db`），构建不再触碰运行期数据。旧版留在 `bin\devicedb.db` 的文件会在构建前备份到 `%LOCALAPPDATA%\Anye\DevCard\legacy\`，程序首次启动时自动迁移。

绿色版打包（Windows，先完成上一步）：

```bat
powershell -ExecutionPolicy Bypass -File tools\package\make_portable_win.ps1
```

脚本从 `dev_card.pro` 读取版本，输出 `dist\DevCard-<版本>-portable.zip`，自动排除数据库/示例 xlsx/日志，并打印实际大小与 SHA256。

### 2.3 手动 qmake

```bat
qmake dev_card.pro -spec win32-g++ "CONFIG+=release"
mingw32-make
```

产物：`bin/DevCard.exe`。

### 2.4 CMake（备选，未在本仓库验证）

```bat
cmake -S . -B build-cmake -G Ninja -DTHIRDPARTY_DIR=<含 include/ 与 lib/ 的目录>
cmake --build build-cmake
```

需要自行提供 `hpdf`、`zip`、`xlsxwriter` 的头文件与库；该路径未经过完整构建验证。

### 2.5 运行

- 直接运行 `bin/DevCard.exe`（或构建产物目录中的可执行文件）。
- 程序从**可执行文件同目录**读取 `resources/`（字体、Logo、图标）；`build.bat` 会自动把它复制进 `bin/`。
- 数据库存放在用户数据目录，不再放在程序目录：
  - Windows：`%LOCALAPPDATA%\Anye\DevCard\devicedb.db`
  - macOS：`~/Library/Application Support/Anye/DevCard/devicedb.db`
- 首次启动时，如果程序目录或 `…\DevCard\legacy\` 下存在旧的 `devicedb.db`，会先复制并校验，成功后才切换为正式库；**原文件保留作为备份**。迁移失败会弹窗说明并退出，不会静默创建空库。
- 现状内置两个默认账号，密码为弱密码且明文存储（具体值不在文档中重复），属待改造项。
- macOS 版已可构建与运行（Apple Silicon，最低 macOS 15）；Intel Mac 未验证，Windows 构建链未受影响。

### 2.6 macOS 构建与 DMG（v1.1.3 实际做法）

```bash
brew install qt libharu libzip libxlsxwriter
mkdir -p build/obj build/moc build/rcc build/ui
mkdir -p /tmp/devcard_mac_build && cd /tmp/devcard_mac_build
/opt/homebrew/opt/qt/bin/qmake /path/to/dev_card_cpp/dev_card.pro
make -j8
```

`.pro` 的 `macx` 分支会把 `packaging/mac/Info.plist`、`DevCard.icns` 和 `resources/{fonts,icons}` 放进 `bin/DevCard.app`。随后用 `macdeployqt bin/DevCard.app -always-overwrite` 收依赖、删掉未使用且会引用缺失 QtPdf/QtSvg/VirtualKeyboard 的插件（`bin/DevCard.app/Contents/PlugIns/{imageformats,iconengines,platforminputcontexts,sqldrivers}`），再 `codesign --force --deep --sign - bin/DevCard.app`。

> 以上是 **v1.1.3 旧 Qt 线的历史做法**，只用于复现旧包。DMG 不再手工敲 `hdiutil`——新架构改用 §2.8 的 `tools/package/make_dmg.sh` 一条命令完成构建、签名与打包；旧线的完整步骤留档在 [改造技术方案 §10.4](docs/改造技术方案.md)。

- 资源定位由 `src/app_paths.h` 的 `appResourcePath()` 完成：Windows 取 `<exe>/resources`，macOS 取 `Contents/Resources/resources`。
- 当前 Homebrew 三方库按 macOS 15.0 构建，所以 DMG 的 `LSMinimumSystemVersion` 写的是 15.0，与 D7 已定的 macOS 15 一致。
- DMG 内不含 `devicedb.db`、`tb_device.xlsx`；`dist/` 已 gitignore。

### 2.7 新架构（改造中）构建与运行

新架构与旧 Qt 线共用仓库，但构建入口独立；不打开开关时 `build.bat` / qmake 的行为完全不变。

```bash
# 构建（macOS / Linux / Windows 的 CMake 工具链）
cmake -S . -B build-new -DDEVCARD_NEW_ARCH=ON
cmake --build build-new -j8
ctest --test-dir build-new --output-on-failure
```

- **单机版（桌面壳）**：macOS 构建后运行 `build-new/shell/DevCard.app`（或 `open build-new/shell/DevCard.app`）。壳内嵌启动本地 HTTP 服务并加载页面，界面表现为普通桌面窗口。
- **网络版（浏览器）**：运行 `build-new/server/devcard-server --port 18081`，浏览器访问 `http://127.0.0.1:18081/`。目前只有本地监听与浏览器访问，`--server` 独立的局域网服务模式属 M7，尚未实现。
- **数据目录**：macOS `~/Library/Application Support/Anye/DevCard/`（代码实值，见 `server/src/platform_paths.cpp`）；Windows 安装版 `%LOCALAPPDATA%\DevCard\data`、便携版 exe 旁 `data\`（显式覆盖优先，形态默认排最后，候选都不可写时报 `data_dir_unwritable`，不回退临时目录）。库文件 `devicedb.db`，自定义图标在 `assets/` 下（不进程序包、不进仓库）。Windows 早期 `Anye/DevCard` 下的库只提示、不自动使用。
- **版本**：只改根目录 `VERSION.txt`（当前 `0.1.0` 开发期；对外发布时改为 `1.2.0`），server 版本串、Windows 版本资源、macOS bundle 版本自动跟随。
- **验证打印**：界面上「卡片打印」→「导出 PDF」/「直接打印」；自动化导出可用环境变量 `DEVCARD_PRINT_SELFTEST_PDF=<输出路径>` 启动壳程序，导出后自动退出。
- **版本检查（M5）**：`GET /api/version` 读取数据目录下的 `update.json`（`{"latest":"1.2.0","download_url":"…","notes":"…"}`，由发布方投放），顶栏显示「有新版本 vX」，点击打开下载页；**不自动下载、不自动替换**。清单缺失或非法时只不显示提示，不影响台账。详见 [改造技术方案 §10.4.1](docs/改造技术方案.md)。
- **远端检查（M6，已交付）**：页面侧 `fetch` 走 `/api/version` 下发的 `check{enabled,endpoint,interval_hours}`（同来自 `update.json`），超时 / DNS 失败 / 非 JSON / 版本号非法四类失败与「没有新版本」界面不可区分。`/` 的 CSP 已显式放行 `connect-src 'self' https: http:`——缺它时 WebKit 只报 `TypeError: Load failed`，会被误判成网络故障。自更新阶段一已完成：更新弹窗里的「下载更新包」把包下载到数据目录 `updates/` 并校验 SHA256 + 字节数，**只下载、不替换**；**阶段二、三未动**。边界：阶段一只用假 curl + 本地清单验过，**真实 GitHub 下载尚未验证**；域名白名单是按 curl 的观察定的，Windows 走系统代理时目标域名不出现在报文里，这条待 Windows 侧回填。
- **账号与密码**：首次启动生成 `root` 与 `安业物业` 两个随机密码账号，写入数据目录 `初始账号密码.txt`（0600），全部改密后自动删除；首次登录与管理员重置不要求旧密码（服务端按 `must_change_password` 判定），自助改密仍验旧密码；密码框支持明文/掩码切换。
- **公司名**：录入上限 16 个字符，卡面仍按 13 字宽截断；界面实时预览「将打印为：……」，超出时明确告知会截断。
- 新增依赖全部内嵌（cpp-httplib、miniz、sqlite amalgamation），不需要 Qt；Windows 侧新架构壳**已编译，`/VERYSILENT` 安装与卸载实测通过**，交互与实物打印仍待补（§10.9）。
- **Windows**：先跑 `powershell -ExecutionPolicy Bypass -File shell/win/fetch-webview2.ps1` 取 WebView2 SDK（固定版本 + SHA256，不入库），再 `cmake -S . -B build-win -DDEVCARD_NEW_ARCH=ON` + 构建；要求 VS2022 Build Tools 与 WebView2 Runtime。安装/卸载已按 `/VERYSILENT` 跑通；向导页、SmartScreen、WebView2 Runtime 缺失场景、导出桥与打印实物仍按 [§10.9](docs/改造技术方案.md) 逐条验证后再宣称可用。

### 2.8 新架构 macOS 打包（一条命令）

```bash
tools/package/make_dmg.sh                # 构建 + 依赖自检 + ad-hoc 签名 + DMG
tools/package/make_dmg.sh --skip-build   # 用已有 build-new 产物重打包
```

- 版本读 `VERSION.txt`，并与 `Info.plist` 的 `CFBundleShortVersionString` 核对；不一致直接失败。
- **依赖自检按 `otool -L` 实测清单**，不写死文件列表：系统库只核对 `/usr/lib/`、`/System/Library/` 前缀（共享缓存里的库磁盘上没有文件），包外依赖必须已存在且收进 `.app`，否则失败。
- 签名按 D13 用 ad-hoc（`codesign --sign -`），`codesign --verify --deep --strict` 通过；不公证。
- **字体随包自检**：`Contents/Resources/fonts/` 必须有两个子集 TTF，且与仓库 `web/fonts/` 逐字节一致（`cmp`），否则打包失败——缺字体就会回落到系统字体，两个平台印出的卡不一致。
- 产物：`dist/DevCard-<版本>-macos-<arch>.dmg`，脚本最后打印字节数、可读体积与 SHA256；DMG 内含应用与「应用程序」快捷方式。
- 本机实测（0.1.0）：`dist/DevCard-0.1.0-macos-arm64.dmg`，**4,593,677 B（4.4M）**，SHA256 `531f9fc42593e77e31c0362e907da04ec5c6527027e8ddff23381191cdc5ec23`。构成为主程序 2,967,552 B + `Resources/fonts` 下两个子集字体 4,560,472 B；上一版不含字体时为 1,651,241 B（`24bdae6c…cd13e`）。

---

## 3. 目录结构

### 3.1 当前仓库（实际）

```text
dev_card_cpp/
├── dev_card.pro              # 旧 Qt 线 qmake 工程（冻结）
├── CMakeLists.txt            # 新架构构建入口（-DDEVCARD_NEW_ARCH=ON）；同时保留旧线 CMake 备选
├── build.bat                 # 旧线 Windows 一键构建 + windeployqt
├── VERSION.txt               # 新架构版本唯一来源
├── src/                      # 旧 Qt 线源码（冻结，不再改动）
├── core/                     # 新架构：业务 / DAO / 迁移 / xlsx / 设置 / 图片元数据
├── server/                   # 新架构：HTTP 路由与入口 devcard-server
├── shell/                    # 新架构：macOS 壳（WKWebView + 打印桥，已实测）/ Windows 壳（WebView2，已编译、静默安装实测）
├── web/                      # 新架构：原生 JS/CSS 前端（构建期内嵌进二进制）；web/fonts/ 放子集 TTF（随包进 Resources）
├── tools/fonts/              # 字体子集脚本（make_subset.sh：变量字体定字重 + pyftsubset，可 --verify）
├── tests/                    # 新架构：ctest 4 条（core / web_assets / no_opposans / embed_web）+ fixture
├── resources/                # 旧线运行期资源：字体、图标、qrc（示例 xlsx 不入库）
├── packaging/mac/            # macOS Info.plist 与 DevCard.icns
├── tools/package/            # macOS DMG 脚本（make_dmg.sh）/ Windows 绿色版脚本（PowerShell）
├── 3rdparty/ third_party/    # SQLite amalgamation / 内嵌第三方依赖；3rdparty/fonts/noto-sans-sc/ 存原始 Noto Sans SC 变量字体与 OFL.txt
├── docs/                     # 技术文档、样张、Release 正文
├── dist/                     # 本地打包产物（已 gitignore）
└── bin/ build/ build-new/    # 构建产物（已 gitignore）
```

### 3.2 新架构目标结构（已实现部分）

`core`（纯 C++20 业务 / DAO / 迁移）+ `server`（HTTP 与入口）+ `shell`（macOS 壳已实测；Windows 壳已编译、静默安装实测）+ `web`（原生 JS 前端，构建期内嵌）+ `tests`，详见 [改造技术方案 §4.1](docs/改造技术方案.md)。

---

## 4. 文档索引

| 文档 | 内容 | 状态 |
|---|---|---|
| [docs/现状技术方案.md](docs/现状技术方案.md) | 当前 Qt 版的实际架构、模块职责、数据流、数据库、构建部署方式，以及通读发现的问题清单 | 描述现状 |
| [docs/改造技术方案.md](docs/改造技术方案.md) | 去 Qt 的目标架构、DAO 与迁移、UI/图标重设计、构建打包、分阶段路径、风险与工作量、Win7/8 可行性结论与决策清单 | 部分已实现（M0–M5、M6 检查 + 告知与自更新阶段一；阶段二、三未动） |
| [docs/M4-卡片打印样张-17条-2页.pdf](docs/M4-卡片打印样张-17条-2页.pdf) | 17 条实测数据导出的完整 2 页 A4 卡片样张（新架构打印链路产物） | 实测样张 |

改造决策：D1–D13 **全部已定** —— 不支持 Win7/8（存量客户保留旧版冻结）；打印双通道（导出 PDF + 直接打印，两通道实物叠图均 ≤ 1 mm 才删 libharu）；原生 JS/CSS + 两个通用件；单进程；导入保持现状（同一文件导入两次即翻倍）；macOS 15+（三方库按 15.0 构建，13/14 不支持）；DAO 集中 SQL + 方言 helper；网络版最小可用；Argon2id + 强制改密 + 命令行重置；深青视觉；macOS 只出 DMG；Windows 出安装版 + 绿色版（安装器 `packaging/windows/DevCard.iss` 已实测静默安装，绿色版脚本仍是旧 Qt 线口径）；macOS 暂不公证，继续 ad-hoc 签名；字体改用思源黑体 / Noto Sans SC 子集。详见 [改造技术方案 §14](docs/改造技术方案.md)。

---

## 5. 已知限制

以下均为**现状**，不是目标形态：

- Windows 与 macOS（Apple Silicon，macOS 15+）可运行；界面用固定像素坐标，缩放/字体缺失时排版会变。macOS 版未公证，首次打开需手动放行。
- 密码明文存储，默认账号每次启动自动补回。
- v1.1.3 的 Windows 绿色版 zip 尚未产出；新架构 Windows 线已有 `packaging/windows/DevCard.iss`（每用户安装、`PrivilegesRequired=lowest`、不注册服务、卸载只删 `{app}` 并在结束时提示数据目录），**已编译并实测 `/VERYSILENT` 安装/卸载**；向导页未逐页点、`web\*` 挂了 `skipifsourcedoesntexist` 导致「带 web 资源」形态未验，补齐前不写「安装包已完整验收」。绿色版脚本 `make_portable_win.ps1` 仍是旧 Qt 线口径（入口仍指 `bin\DevCard.exe`），新架构 Windows 产物尚未接进打包。macOS 已按 D7 定为 15+，按 D13 不公证（ad-hoc 签名，首次需 Gatekeeper 放行）。
- **卸载不删数据目录**：安装版卸载只移除程序文件与快捷方式，数据目录保留，结束页提示路径。
- 无 CI、无许可证声明。（新架构 `tests/` 已有 4 条 `ctest`：core / web_assets / no_opposans / embed_web；CI 仍未接。）
- 源码注释提到的 Python 原版不在本仓库，历史行为未逐条比对。

**新架构当前已知边界（照实写）**：

- **M0–M4.2 完成、M5 第一段完成（macOS）、M5 第二段 Windows 壳已编译并静默安装、M6 检查 + 告知与自更新阶段一完成**；M4 的 G2「两条通道实物叠图 ≤ 1 mm」尚未验收，因此 **libharu 未删**，旧 Qt 线仍是可用的发版路径。
- **更新能力**：`/api/version` 读本地 `update.json`；远端检查在页面侧 `fetch`（`check_*` 三项配置），失败静默。自更新已拍板为「下载 + 校验 + 替换 + 重启」三阶段，**阶段一（下载到数据目录 `updates/` + SHA256 与字节数双校验）已完成（`a4cda3a`，闸门与状态码表 `6f425fd`），阶段二、三未动**。阶段一只用假 curl + 本地清单验过，**真实 GitHub 下载未验**。无 Authenticode / 公证，信任锚只有「固定 HTTPS 域名 + 清单版本与哈希」，挡不住清单连同包一起被伪造，因此不写「安全校验通过」。
- **实物打印未验**：本机没有打印机，只验证了「同一份 2 页 A4 PDF 能构造打印任务」，打印面板确认与出纸需要目标打印机。
- **Windows 侧只走到「静默安装跑通」**：Windows 壳（WebView2）已编译，`/VERYSILENT` 安装（无 UAC、落 `%LOCALAPPDATA%\Programs\DevCard`、`health=200`）与卸载（数据目录逐文件 SHA256 前后一致）已实测。仍未做：安装向导页逐页点、实物打印与导出桥端到端、SmartScreen 实际拦截文案、WebView2 Runtime 缺失场景、交互回归、中文路径在 Windows 上的落盘验证（自动化用例已加，需在 Windows 上跑 `ctest` 才算数），清单见 [改造技术方案 §10.9](docs/改造技术方案.md)。
- **旧库迁移夹具无设备行**：用于 M3 验收的脱敏旧库样本里没有设备记录，设备行迁移只做了逻辑与单测，未做真实数据抽样比对。

v1.1.3 已修复：数据库迁出程序目录（构建不再删库、只读安装目录可写用户数据）、`build.bat` 构建后自动复制 `resources/` 到 `bin/`。详见 [v1.1.3 变更说明](docs/变更说明-v1.1.3.md)。
