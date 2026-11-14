# DevCard · 设备管理卡制作系统

物业设备台账与设备管理卡打印工具：维护设备名称 / 位置 / 控制范围 / 维护电话，按条件筛选后把结果排成 A4 版面（每页 3 列 × 4 行，共 12 张卡片）导出 PDF 打印。

> **当前状态（重要）**
> 本仓库现在仍是 **Qt 6 / C++17 版 v1.1.3，仅面向 Windows**。v1.1.3 仍使用 OPPOSans；开源字体替换属于改造项（D12），尚未实施。
> 去 Qt、改 B/S 内核（单机版 + 网络版一体化）的改造**只完成了方案与决策，代码尚未开始迁移**。
> 下文凡标注「目标形态」的内容都是计划，不代表仓库已有该能力；判断现状请看 [现状技术方案](docs/现状技术方案.md)。

---

## 1. 项目定位

| 项 | 说明 |
|---|---|
| 使用者 | 物业设备管理员（单机、单人日常使用） |
| 核心流程 | 台账维护 → 按名称/位置筛选或按字段查询 → 生成卡片 PDF → 打印塑封上墙 |
| 数据规模 | 数十到数千条设备记录；单文件 SQLite |
| 部署形态 | 免安装绿色目录：程序 + `resources/` + `devicedb.db` 放在同一目录 |
| 平台 | 现状：Windows；改造目标：macOS 13+（待定 D7）与 Windows 10 1809+ / 11 |

功能范围（现状）：

- 设备台账增、删、改、清空、合并重复
- 按名称、位置两个下拉框组合筛选；按设备名称/位置/控制范围/维护电话模糊查询
- Excel（`.xlsx`）导入导出
- 按当前筛选或查询结果生成设备管理卡 PDF（A4，3×4 版式）
- 用户维护（新增、改密、删除）

---

## 2. 当前版本：构建与运行（Qt 6，Windows）

### 2.1 前置条件

| 依赖 | 版本/位置 | 说明 |
|---|---|---|
| Qt | 6.x（MinGW 64-bit） | 需 `core gui widgets sql xml` |
| MinGW | 13.x（随 Qt 安装） | `mingw32-make` 在 `PATH` 中 |
| libharu / libzip / libxlsxwriter | `C:/msys64/mingw64/{include,lib}` | 头文件 `hpdf.h`、`zip.h`、`xlsxwriter.h`；运行期需要对应 DLL |
| SQLite | 仓库内 `3rdparty/sqlite-amalgamation-3460100` | 直接编译源码，无需外部安装 |

### 2.2 一键构建（推荐）

在 Windows 上双击或命令行执行：

```bat
build.bat
```

脚本会：检查 qmake 与第三方头文件 → 备份旧的 `bin/devicedb.db`（若有）→ 清理 `build/` 与 `bin/` → `qmake dev_card.pro` → 编译 → `windeployqt` → 复制 `resources/` 到 `bin/`。

> 数据库已改存到用户数据目录（Windows：`%LOCALAPPDATA%\Anye\DevCard\devicedb.db`），构建不再触碰运行期数据。旧版留在 `bin\devicedb.db` 的文件会在构建前备份到 `%LOCALAPPDATA%\Anye\DevCard\legacy\`，程序首次启动时自动迁移。

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
- 现状内置账号：`root / 123456`、`安业物业 / 123456`（明文存储，属待改造项）。
- 现状仅面向 Windows；macOS 可构建属改造目标，尚未实现。

---

## 3. 目录结构

### 3.1 当前仓库（实际）

```text
dev_card_cpp/
├── dev_card.pro              # qmake 工程（当前主构建入口）
├── CMakeLists.txt            # CMake 备选构建
├── build.bat                 # Windows 一键构建 + windeployqt
├── fix_env.bat / .reg        # 开发机 PowerShell 执行策略修复（未入库）
├── DevCard_resource.rc       # Windows 版本信息资源（当前未被构建引用）
├── src/
│   ├── main.cpp              # 入口
│   ├── app_info.h            # 全局变量与版本/版权常量
│   ├── db/                   # SQLite 访问与初始化
│   ├── excel/                # xlsx 导入导出
│   ├── printer/              # 设备管理卡 PDF（libharu）
│   └── ui/                   # 窗口与手写的表单布局类
├── resources/                # 运行期资源：字体、图标、qrc、示例 xlsx
├── 3rdparty/                 # SQLite amalgamation
├── docs/                     # 技术文档
└── bin/ build/ Makefile*     # 构建产物（已 gitignore）
```

### 3.2 目标结构（改造方案，尚未实现）

`core`（纯 C++20 业务 / DAO / 迁移）+ `server`（HTTP 与入口）+ `shell`（macOS / Windows 壳）+ `web`（原生 JS 前端，内嵌进二进制）+ `tests` + `third_party`，详见 [改造技术方案 §4.1](docs/改造技术方案.md)。

---

## 4. 文档索引

| 文档 | 内容 | 状态 |
|---|---|---|
| [docs/现状技术方案.md](docs/现状技术方案.md) | 当前 Qt 版的实际架构、模块职责、数据流、数据库、构建部署方式，以及通读发现的问题清单 | 描述现状 |
| [docs/改造技术方案.md](docs/改造技术方案.md) | 去 Qt 的目标架构、DAO 与迁移、UI/图标重设计、构建打包、分阶段路径、风险与工作量、Win7/8 可行性结论与决策清单 | 方案，未实施 |

改造决策：D1–D12 **已定**，D13（macOS 是否公证）待定 —— 不支持 Win7/8（存量客户保留旧版冻结）；打印双通道（导出 PDF + 直接打印，两通道实物叠图均 ≤ 1 mm 才删 libharu）；原生 JS/CSS + 两个通用件；单进程；导入保持现状（同一文件导入两次即翻倍）；macOS 13+；DAO 集中 SQL + 方言 helper；网络版最小可用；Argon2id + 强制改密 + 命令行重置；深青视觉；macOS 只出 DMG，Windows 出安装版 + 绿色版（数据目录按形态分离）；字体改用思源黑体 / Noto Sans SC 子集。详见 [改造技术方案 §14](docs/改造技术方案.md)。

---

## 5. 已知限制

以下均为**现状**，不是目标形态：

- 仅 Windows 可运行；界面用固定像素坐标，缩放/字体缺失时排版会变。
- 口令明文存储，默认账号每次启动自动补回。
- 无测试、无 CI、无许可证声明。
- 源码注释提到的 Python 原版不在本仓库，历史行为未逐条比对。

v1.1.3 已修复：数据库迁出程序目录（构建不再删库、只读安装目录可写用户数据）、`build.bat` 构建后自动复制 `resources/` 到 `bin/`。详见 [v1.1.3 变更说明](docs/变更说明-v1.1.3.md)。
