## DevCard 1.3.4

### 本次更新

- **Windows 安装包支持 WebView2 可选安装**：仅在系统未安装 WebView2 Runtime 时显示该任务，默认勾选，从官方地址下载并以静默方式安装；下载或安装失败不阻断主程序安装。
- **单实例启动**：重复启动会激活已有窗口，不再打开多个实例。
- **台账打印支持勾选**：勾选记录后仅打印选中项，未勾选时保持原行为。
- **Windows 强制升级握手**：新增 `checkMinSupportedVersion` 校验，低于最低支持版本的客户端在启动握手阶段收到明确提示。
- **静默卸载修复**：改用 `SuppressibleMsgBox`，消除卸载后残留 `_unins.tmp` 的问题。
- **内部重构**：前端按域拆分为 ES Module，`app.js` 仅保留入口装配；设置、审计、备份、用户与实例心跳的 SQL 收口到 DAO，降低单文件维护成本。

### 构建与分发

- macOS 安装包使用 **Developer ID 签名 + Apple 公证 + stapler 票据**。
- Windows 安装包与绿色版同步更新，构建号 `5`。

### 下载

| 平台 | 文件 | 说明 |
| --- | --- | --- |
| macOS（Apple Silicon） | `devcard-1.3.4-macos-arm64.dmg` | 拖拽到「应用程序」即可 |
| Windows | `devcard-1.3.4-win64-setup.exe` | 安装版 |
| Windows | `devcard-1.3.4-win64-portable.zip` | 免安装绿色版 |

### 校验

随附 `SHA256SUMS.txt`，下载后可自行校验：

```bash
# macOS / Linux
shasum -a 256 -c SHA256SUMS.txt

# Windows
certutil -hashfile devcard-1.3.4-win64-setup.exe SHA256
```

### 升级说明

应用内「检查更新」会自动检测并升级；也可以从下载页手动安装覆盖。构建号 `5`，macOS 与 Windows 同号。
