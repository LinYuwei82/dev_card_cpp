## DevCard 1.3.10

### 本次更新

- **新增「关于」入口（双平台）**：macOS 应用菜单首位「关于 DevCard」、Windows 菜单栏「帮助 → 关于 DevCard」，可查看当前版本号与构建号，并直达更新记录、用户手册与项目发布页。
- 其余功能与数据行为与 1.3.9 完全一致，台账数据无任何格式变化。

### 升级说明

- 1.3.8 / 1.3.9 用户可直接点「检查更新 → 下载更新包 → 安装并重启」升级，数据自动保留。

### 构建与分发

- 本次双平台：构建号 `11`。

### 下载

| 平台 | 文件 | 说明 |
| --- | --- | --- |
| macOS（Apple Silicon） | `devcard-1.3.10-macos-arm64.dmg` | 拖拽到「应用程序」即可 |
| Windows | `devcard-1.3.10-win64-setup.exe` | 安装版 |
| Windows | `devcard-1.3.10-win64-portable.zip` | 免安装绿色版 |

### 校验

随附 `SHA256SUMS.txt`，下载后可自行校验：

```
shasum -a 256 -c SHA256SUMS.txt
```

更新清单使用 Ed25519 签名（`manifest.json`），客户端验签失败会自动放弃本次更新。
