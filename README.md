# DevCard · 设备管理卡制作系统

物业设备台账与设备管理卡打印工具：维护设备名称 / 位置 / 控制范围 / 维护电话，按条件筛选后把结果排成 A4 版面（每页 3 列 × 4 行，共 12 张卡片）导出 PDF 打印。

> **当前状态**
> 仓库只保留新架构线（C++20 + SQLite + 系统 WebView），旧 Qt 线已从仓库历史删除（不是仅工作区删除），旧 commit hash 与 `v1.1.3` tag 一律不可引用。**当前版本 `1.3.2`**（唯一来源：仓库根 `VERSION.txt`），发布号为 annotated tag `v<semver>+<seq>`（已发布 `v1.3.0`、`v1.3.1+1`、`v1.3.1+2`），**构建号 = tag 序号 `seq`**（从 1 起，无固定基数）。
>
> 功能线：M0–M4.2 已落地（M4 的 G2 实物叠图与打印面板仍待验）；M5 版本检查、「有新版本」提示与 macOS 一条命令出 DMG 已落地；M6 检查 + 告知已交付；自更新三端可用——Windows 走「下载 + 校验 + 替换 + 重启」，macOS 走 Sparkle 2.10.1（检查 → 下载 → EdDSA 验签 → 替换 → 重启），均已端到端实测。Windows 安装包 `/VERYSILENT` 安装 + 卸载实测通过；向导页、SmartScreen、WebView2 Runtime 缺失场景与实物打印仍待补（见 [改造技术方案 §10.9](docs/改造技术方案.md)）。
>
> 旧版数据不再支持导入：迁移路径、旧库导入器与 `--import-legacy` 已整体删除（详见 [改造技术方案 §0.1](docs/改造技术方案.md)）。

## 1. 项目定位

| 项 | 说明 |
|---|---|
| 使用者 | 物业设备管理员（单机、单人日常使用） |
| 核心流程 | 台账维护 → 按名称/位置筛选或按字段查询 → 生成卡片 PDF → 打印塑封上墙 |
| 数据规模 | 数十到数千条设备记录；单文件 SQLite |
| 部署形态 | Windows 安装版 / 绿色版；macOS `.app` / DMG，数据在 `~/Library/Application Support/Anye/DevCard/` |
| 平台 | macOS 15+（Apple Silicon）、Windows 10 1809+ |

功能范围（现状）：

- 设备台账增、删、改、清空、合并重复
- 按名称、位置两个下拉框组合筛选；按设备名称/位置/控制范围/维护电话模糊查询
- Excel（`.xlsx`）导入导出
- 按当前筛选或查询结果生成设备管理卡 PDF（A4，3×4 版式）
- 用户维护（新增、改密、删除）

---

## 2. 构建与运行

macOS：

```bash
cmake --preset macos-release                  # Ninja + Release → build/release
cmake --build --preset macos-release --parallel
ctest --test-dir build/release --output-on-failure
```

Windows（Ninja + MSVC）：先进入已跑过 `vcvars64.bat` 的「x64 Native Tools Command Prompt for VS 2022」（CI 用 `ilammy/msvc-dev-cmd` 注入环境），并先取 WebView2 SDK：

```powershell
powershell -ExecutionPolicy Bypass -File shell/win/fetch-webview2.ps1
cmake --preset windows-release                # → build/windows
cmake --build build/windows --parallel
ctest --test-dir build/windows --output-on-failure
```

Ninja 是单配置生成器，**产物没有 `Release/` 子目录**：

- macOS：`build/release/shell/DevCard.app`（可执行文件 `build/release/shell/DevCard.app/Contents/MacOS/DevCard`）、`build/release/server/devcard-server`
- Windows：`build/windows/shell/win/DevCard.exe`、`build/windows/server/devcard-server.exe`

开发迭代用 `macos-dev` / `windows-dev` preset，构建目录分别是 `build-new-dev` / `build-win-dev`（仅 dev 上下文；发布与打包一律用 `build/release` / `build/windows`）。

其他运行相关：

- **单机版（桌面壳）**：macOS 构建后 `open build/release/shell/DevCard.app`；Windows 双击 `build/windows/shell/win/DevCard.exe`。壳内嵌启动本地 HTTP 服务并加载页面。
- **网络版（浏览器）**：运行 `build/release/server/devcard-server --port 18081`（Windows 为 `build/windows/server/devcard-server.exe`），浏览器访问 `http://127.0.0.1:18081/`。`--server` 支持监听地址与来源 IP、HttpOnly 会话、多实例防护、登录限流、每用户数据目录；部署与防火墙见 [docs/部署文档.md](docs/部署文档.md)。
- **数据目录**：macOS `~/Library/Application Support/Anye/DevCard/`（见 `server/src/platform_paths.cpp`）；Windows 安装版 `%LOCALAPPDATA%\DevCard\data`、便携版 exe 旁 `data\`（显式覆盖优先，形态默认排最后，候选都不可写时报 `data_dir_unwritable`，不回退临时目录）。库文件 `devicedb.db`，自定义图标在 `assets/` 下（不进程序包、不进仓库）。旧版库（只有 `tb_device`/`tb_user`、无 `schema_migration`）启动直接报 `legacy_db`，不会在原库上建表。
- **版本**：只改根目录 `VERSION.txt`；server 版本串、Windows 版本资源、macOS bundle 版本自动跟随。改版本后必须重新 configure（PE 与 bundle 版本在 configure 阶段注入）。
- **验证打印**：界面「卡片打印」→「导出 PDF」/「直接打印」；自动化导出可用环境变量 `DEVCARD_PRINT_SELFTEST_PDF=<输出路径>` 启动壳程序，导出后自动退出。
- **版本检查**：`GET /api/version` 读取数据目录下的 `manifest.json`（Schema v2 统一清单，两平台同一份、Ed25519 签名，服务端用编译期内嵌公钥验签后只取 `platforms.<本平台>`），返回本平台 `latest`、`build` 与产物资产。清单缺失、验签失败或非法时不显示提示，不影响台账。远端检查由页面 `fetch` 走 `/api/version` 下发的 `check{enabled,endpoint,interval_hours}`（默认 `https://dl.xiaotu.online/devcard/updates/stable/manifest.json`、24 小时）。macOS 壳的「检查更新」经原生桥转交 Sparkle；Windows 壳与浏览器走清单通道。
- **构建号**：`cmake -DDEVCARD_BUILD=<seq>` 经 `cmake/devcard_version.cmake` 的 `devcard_add_build_stamp()` 注入所有生产 target 的编译期宏 `DEVCARD_BUILD`（整数）与 `DEVCARD_BUILD_STR`（字符串）；客户端 `compiled_build_number()` 读取，同版本重发时清单 `build > 本机构建号` 才提示更新。未注入时返回 0，回退到只比 version。`seq` 来自发布 tag（见 §2.3）。
- **账号与密码**：首次启动生成 `root` 与 `安业物业` 两个随机密码账号，写入数据目录 `初始账号密码.txt`（0600），全部改密后自动删除；首次登录与管理员重置不要求旧密码（按 `must_change_password` 判定），自助改密仍验旧密码。
- **接口契约测试**：`tests/test_api_contract.cpp` 随 `ctest` 运行——进程内起真实服务，锁死认证闸门顺序、乐观锁 409 响应形状、`/api/version` 字段集，并把前端 `app.js` 的全部 `/api/` 字面量与服务端路由契约表比对。
- **会话持久化**：登录会话落盘 `data_dir/sessions.json`（TTL 12 小时），进程重启后旧 cookie 仍有效；滑动 TTL 只改内存，`last_seen` 落盘值有滞后。
- **公司名**：录入上限 16 个字符，卡面仍按 13 字宽截断；界面实时预览「将打印为：……」。13 字与 16 字两张样张已实测等价：132,718 B / 2 页 / 12 + 5 / 跨页 0。
- 新增依赖全部内嵌（cpp-httplib、miniz、sqlite amalgamation），不需要 Qt。

### 2.1 macOS 打包（一条命令）

```bash
tools/package/package_macos.sh                # 构建 + 依赖自检 + Developer ID 签名 + 公证 + staple → DMG
tools/package/package_macos.sh --skip-build   # 用已有 build/release 产物重打包
tools/package/package_macos.sh --feed-url https://dl.xiaotu.online/devcard/updates/stable/appcast.xml

DEVCARD_BUILD=<seq> tools/release/publish_macos.sh   # 出包 → 签名 → appcast + manifest.json → 上传 OSS
tools/release/publish_macos.sh --notes "本次更新内容"
tools/release/publish_macos.sh --dry-run      # 只出包与清单，不上传
```

- 版本读 `VERSION.txt`，并与产物 `Info.plist` 的 `CFBundleShortVersionString` / `CFBundleVersion` 核对；不一致直接失败。
- 构建走 `cmake --preset macos-release`（`build/release`），发布/出包构建显式开启 `-DDEVCARD_RELEASE_BUILD=ON`，必须提供 `DEVCARD_BUILD`（纯数字且不是开发占位值）。
- **Sparkle 自检**：feed 默认 `https://dl.xiaotu.online/devcard/updates/stable/appcast.xml`，可用 `--feed-url` 或 `DEVCARD_SPARKLE_FEED_URL` 覆盖；打包时核对 `Info.plist` 的 `SUFeedURL` 与 `SUPublicEDKey`，缺失或不一致直接失败。
- **依赖自检按 `otool -L` 实测清单**，不写死文件列表：系统库只核对 `/usr/lib/`、`/System/Library/` 前缀，`@rpath` 依赖按 `Contents/Frameworks/` 核对，包外依赖必须已收进 `.app`。
- 签名用 Developer ID Application，逐层签、禁用 `--deep`，`codesign --verify --deep --strict` 通过；DMG 经 `notarytool submit --wait` 公证 + `stapler staple`，`spctl` 应为 `Notarized Developer ID`。公证凭证存 Keychain profile `devcard-notary`。
- **字体随包自检**：`Contents/Resources/fonts/` 必须有两个子集 TTF，且与仓库 `web/fonts/` 逐字节一致（`cmp`）。
- 产物：`dist/DevCard-<版本>-macos-<arch>.dmg`；脚本最后打印字节数、可读体积、DMG SHA256（仅传输校验）与 App 层身份哈希（可执行文件 + `Info.plist`）。
- 实测数值随构建与工具链变化，**只作当次记录，不作跨次基线**；每次出包以脚本末尾打印的字节数与 SHA256 为准。正式发布物还应记录公证 ID 与 `spctl` 结果。

### 2.2 Windows 打包（绿色版 + 安装版）

```powershell
# 一条命令发布：绿色包 → 安装包 → manifest.json（platforms.windows）→ 上传 OSS
powershell -ExecutionPolicy Bypass -File tools\release\publish_windows.ps1 -Build <seq>
powershell -ExecutionPolicy Bypass -File tools\release\publish_windows.ps1 -Notes "本次更新内容" -DryRun
```

在 macOS 上发布已构建好的 Windows 产物：

```bash
BUILD=<seq> tools/release/publish_windows.sh   # 自动找 dist/ 产物，构建号显式指定
DRY_RUN=1 tools/release/publish_windows.sh     # 只生成清单 + 过门禁，不上传
```

分步执行：

```powershell
# 绿色版（构建走 windows-release preset，产物在 build\windows）
powershell -ExecutionPolicy Bypass -File tools\package\package_windows.ps1 -Build <seq>
# 安装版（Inno Setup 6；SourceRoot 指向含两个 exe 与 fonts\ 的暂存目录）
ISCC.exe /DSourceRoot=<暂存目录> packaging\windows\DevCard.iss
# 更新清单：由 publish_windows.ps1 调 tools/release/manifest_v2.py 生成并签名
# （dist\manifest.json）
```

- 绿色版产物：`dist\DevCard-<版本>-win64-portable.zip`，包内 `DevCard.exe`、`devcard-server.exe`、`fonts\`、`VERSION.txt`；数据目录为 exe 旁 `data\`，升级时手动替换程序目录并保留 `data\`。
- 安装版产物：`dist\DevCard-<版本>-setup.exe`（发布脚本再按 v2 命名复制为 `devcard-<版本>-win64-setup.exe` 上传 OSS）。
- 自检：两个 exe 的 `ProductVersion` 必须等于 `VERSION.txt`、字体齐全且非空、包内不得出现 `*.db`；绿色包内 `DevCard.exe` 必须含 `DEVCARD_BUILD=<seq>` 标记。
- 发布脚本用 `manifest_v2.py` 生成并签名 `dist\manifest.json`（Windows 段 `artifacts.setup` / `artifacts.portable` 记录 OSS 地址、大小与 SHA256）；上传后需同步一份到数据目录，本机服务才会读到。
- CI 的 Windows job 只构建、测试并产出绿色包与安装包；正式发布由 `release-windows` 工作流完成（见 §2.3）。

### 2.3 发布号、门禁与 CI

- **版本号**：只改 `VERSION.txt`（`x.y.z`）。
- **发布号**：annotated tag `v<semver>+<seq>`，例如 `v1.3.2+3`；`semver` 必须等于 `VERSION.txt`，`seq` 从 1 起、只增不减，**构建号 `build = seq`**（无固定基数，旧的时间戳/大数构建号方案已废弃）。
- **分配并推送 tag**：`python3 tools/release/release_tag.py create --push`（默认取「已有最大序号 + 1」，也可 `--seq` 显式指定；tag 不可删除重打）。
- **CI 解析**：`release-windows.yml` 在 tag 触发时执行 `release_tag.py parse --ensure-latest`，把 `BUILD` 写入 `DEVCARD_BUILD`；`workflow_dispatch` 手动触发**必须填 `build`**。
- **发版前门禁**：`tools/release/check_version_bump.py --build <seq> --manifest-url https://dl.xiaotu.online/devcard/updates/stable/manifest.json`——版本不得回退，同版本重发时 build 必须严格大于线上所有平台的最大 build。
- **上传前门禁**：`guard.py preflight` 校验 v2 命名、版本、覆盖风险、内嵌地址、清单签名与构建号，并要求 DevCard 清单同时含 `platforms.macos + platforms.windows`；Windows 侧还会从绿色包 `DevCard.exe` 里提取 `DEVCARD_BUILD=<digits>` 与 `--build` 比对。同版本 build 判定逻辑与客户端共用 `tools/release/update_compare_vectors.json`。
- **上传后门禁**：`guard.py verify-live` 核对线上清单、大小、SHA256、Content-Type / Cache-Control。
- **发布脚本**：macOS `tools/release/publish_macos.sh`；Windows `tools/release/publish_windows.ps1`（Windows 本机一条龙）或 `tools/release/publish_windows.sh`（macOS 上只发布现成产物）。本地清单落在 `dist/manifest.json`、`dist/appcast.xml`，上传后即 OSS 上的同名对象。
- **OSS**：bucket `xiaotu-update`，前缀 `devcard/`，域名 `https://dl.xiaotu.online`；appcast 的 `sparkle:version` = `build`（即 tag 序号）。
- **CI**：`.github/workflows/ci.yml`——`release-guard`（guard 哈希、selftest、发布脚本布局检查）、`windows-build`（取 WebView2 SDK → MSVC 环境 → `windows-release` 构建/ctest → 绿色包 + 安装包）、`macos-build`（`macos-release` 构建/ctest）。`.github/workflows/release-windows.yml`——tag 或手动触发，解析构建号 → 版本/构建号递增校验 → `windows-release` 构建 → 发布。
- **测试**：C++ 测试在 `tests/`（`ctest` 5 条：core / web_assets / no_opposans / embed_web / api_contract）；发布工具测试为 `tools/release/test_release_tag.py`、`test_version_bump.py`、`test_guard.py`、`test_guard_manifest.py`（v2 清单字段/跨平台构建号/artifact 校验）、`test_release_layout.py`、`test_publish_release.py`、`test_shared_sync.py`（兄弟仓库存在时比对共享文件，单仓环境自动跳过）、`test_release_scripts.py`（shell 用例仅 macOS 执行）、`test_appcast.py`。

---

## 3. 目录结构

```text
dev_card_cpp/
├── CMakeLists.txt            # 唯一构建入口
├── CMakePresets.json         # macos-release / macos-dev / windows-release / windows-dev / linux-ci
├── VERSION.txt               # 版本唯一来源
├── core/                     # 业务 / DAO / 迁移 / xlsx / 设置 / 图片元数据
├── server/                   # HTTP 路由与入口 devcard-server
├── shell/                    # macOS 壳（WKWebView + 打印桥）/ Windows 壳（WebView2）
├── web/                      # 原生 JS/CSS 前端（构建期内嵌进二进制）；web/fonts/ 放子集 TTF
├── cmake/                    # embed_web / font_manifest / devcard_version / version.rc.in
├── tools/
│   ├── fonts/                # make_subset.sh（变量字体定字重 + pyftsubset）
│   ├── mac/                  # build.sh / test.sh（macOS 构建与测试入口）
│   ├── package/              # package_macos.sh（DMG）、make_icons.sh（三端图标）、package_windows.ps1（绿色包）
│   └── release/              # publish_* / release_tag.py / manifest_v2.py / guard.py / check_version_bump.py
│                             #   / generate_appcast.py / upload_oss.py / test_*.py / update_compare_vectors.json
├── packaging/windows/        # DevCard.iss（Inno Setup 安装包）
├── tests/                    # ctest 5 条 + fixture
├── 3rdparty/ third_party/    # SQLite amalgamation / 内嵌第三方依赖；3rdparty/fonts/ 存原始 Noto Sans SC
├── docs/                     # 技术文档、用户手册、样张
├── dist/                     # 本地打包产物与清单（已 gitignore）
└── build/                    # 构建产物（已 gitignore；release/ 与 windows/）
```

目标结构：`core`（纯 C++20 业务 / DAO / 迁移）+ `server`（HTTP 与入口）+ `shell`（平台壳）+ `web`（原生 JS 前端，构建期内嵌）+ `tests`，详见 [改造技术方案 §4.1](docs/改造技术方案.md)。

---

## 4. 文档索引

| 文档 | 内容 | 状态 |
|---|---|---|
| [docs/改造技术方案.md](docs/改造技术方案.md) | 去 Qt 的目标架构、DAO 与迁移、UI/图标重设计、构建打包、分阶段路径、风险与工作量、Win7/8 可行性结论与决策清单 | 历史归档 + 现行做法速查（构建/打包/发布以本文 §0 与 README §2 为准） |
| [docs/部署文档.md](docs/部署文档.md) | Windows 安装/绿色版、端口与防火墙、数据目录与备份、升级、错误码、限流、发布 checklist | 现行 |
| [docs/用户手册/用户手册.md](docs/用户手册/用户手册.md) | 面向使用者的操作说明与更新机制 | 现行 |
| [docs/M4-卡片打印样张-17条-2页.pdf](docs/M4-卡片打印样张-17条-2页.pdf) | 17 条实测数据导出的完整 2 页 A4 卡片样张 | 实测样张 |

改造决策 D1–D13 全部已定：不支持 Win7/8；打印双通道（导出 PDF + 直接打印）；原生 JS/CSS；单进程；导入允许重复；macOS 15+；SQL 主要在 DAO + 方言 helper；网络版最小可用；Argon2id + 强制改密 + 命令行重置；深青视觉；macOS 只出 DMG（Developer ID 签名 + Apple 公证 + staple）；Windows 出安装版 + 绿色版；macOS 自更新用 Sparkle 2.10.1；字体用 Noto Sans SC 子集。**旧版数据不再支持导入**。详见 [改造技术方案 §14](docs/改造技术方案.md)。

---

## 5. 已知限制

以下均为**现状**，不是目标形态：

- macOS（Apple Silicon，macOS 15+）与 Windows 可运行；macOS 版自 2026-11-17 起用 Developer ID Application 签名并经 Apple 公证 + staple，首次打开无需手动放行。
- macOS 图标 `shell/mac/DevCard.icns`、Windows `shell/win/DevCard.ico`、web favicon / Apple Touch / PWA / 分享预览图**全部由 `tools/package/make_icons.sh` 从同一份母版统一生成**。
- Windows 安装器 `packaging/windows/DevCard.iss`（每用户安装、`PrivilegesRequired=lowest`、不注册服务、卸载只删 `{app}` 并提示数据目录）**已编译并实测 `/VERYSILENT` 安装/卸载**；向导页未逐页点，SmartScreen 与 WebView2 Runtime 缺失场景仍待验。
- **卸载不删数据目录**：安装版卸载只移除程序文件与快捷方式，数据目录保留，结束页提示路径。
- **实物打印未验**：本机没有打印机，只验证了「同一份 2 页 A4 PDF 能构造打印任务」，打印面板确认与出纸需要目标打印机。
- Windows 自更新协助器（`devcard-server.exe --apply-update`）以 `/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /CLOSEAPPLICATIONS` 静默替换，完成后写 `install-result.json` 并拉起新版本；1.2.0 → 1.2.1 已实测闭环。仍未做：安装向导页逐页点、实物打印与导出桥端到端、SmartScreen 实际拦截文案、WebView2 Runtime 缺失场景、交互回归，清单见 [改造技术方案 §10.9](docs/改造技术方案.md)。
- **更新信任锚**：清单统一走 Ed25519 验签（公钥随客户端编译）；Windows 暂不购买 Authenticode 代码签名证书（2026-11-17 决定，发布页有 SmartScreen 提示）；macOS 另有 Developer ID 签名 + Apple 公证 + appcast 内 EdDSA 签名。
- macOS 自更新由 Sparkle 承担；appcast 由 `tools/release/generate_appcast.py` 生成，EdDSA 私钥存 `~/.config/devcard/sparkle-keys/`（不进仓库）。
- 源码注释提到的 Python 原版不在本仓库，历史行为未逐条比对。

## 许可证

本项目以 **GNU Affero General Public License v3.0** 发布，官方全文见 [LICENSE](LICENSE)。

Copyright (C) 2026 linyw <458499311@qq.com>

如需商业授权，请另行联系作者。
