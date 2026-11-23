## DevCard 1.3.5

### 本次更新

- **修复 Windows 端「检查更新」后无法下载/安装新版本**：1.3.3 与 1.3.4 的 Windows 壳用自实现的 COM 宿主对象向页面暴露更新接口，缺少类型信息，页面取成员后被当成普通值调用，报 `unable to call method on non-function`。表现为：能提示"发现新版本"，点「下载更新包」必定失败，也无法安装。
- 更新接口改为页面注入的 `window.__devcardUpdater` 通道（`postMessage` + 完成事件），下载/安装在后台线程完成后的结果会正确回到界面。
- 下载完成码与前端的判定对齐（`DOWNLOADED`），下载成功后「安装并重启」可正常使用。

### 升级说明（重要）

**1.3.3 / 1.3.4 的 Windows 客户端无法自动升级到 1.3.5**——自动更新链路本身就是这两版坏掉的部分。请手动下载 1.3.5 安装包覆盖安装一次；装好之后，后续版本的自动更新恢复正常。

### 构建与分发

- 本次仅 Windows：构建号 `6`。
- macOS 保持 1.3.4（构建号 `5`），无变化。

### 下载

| 平台 | 文件 | 说明 |
| --- | --- | --- |
| Windows | `devcard-1.3.5-win64-setup.exe` | 安装版 |
| Windows | `devcard-1.3.5-win64-portable.zip` | 免安装绿色版 |

### 校验

随附 `SHA256SUMS.txt`，下载后可自行校验：

```bash
# Windows
certutil -hashfile devcard-1.3.5-win64-setup.exe SHA256
```
