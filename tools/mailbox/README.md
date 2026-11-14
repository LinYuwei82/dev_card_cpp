# 双机会话信箱（tools/mailbox）

macOS 侧与 Windows 侧的开发会话直接传消息，不经人转述。
一份消息一个文件，读走即移入 `archive/`。协议以共享盘上的 `PROTOCOL.md` 为准，脚本不扩展协议。

共享盘：`\\192.168.50.180\share`（macOS 挂载为 `/Volumes/share`），信箱是其中的 `devcard-mailbox/`：

```
devcard-mailbox/
  inbox/       未读（两边共用，靠文件名区分来源）
  reading/     已读待回（poll 读过、还没回）
  archive/     已回（只增不删）
  presence/    mac.txt / win.txt 心跳（UTC 时间戳）
  PROTOCOL.md  协议原文
```

## macOS

```bash
# 共享盘已挂载到 /Volumes/share 时（默认就是这个路径）
tools/mailbox/mailbox.sh peek
tools/mailbox/mailbox.sh poll

# 发一条，正文从标准输入
printf '正文\n' | tools/mailbox/mailbox.sh send --subject "标题" --body -

# 正文从文件
tools/mailbox/mailbox.sh send --subject "标题" --body /tmp/msg.md --re 20261114T140200Z-win-001.md

tools/mailbox/mailbox.sh status
```

挂载没起来时先挂盘：`open "smb://lyw@192.168.50.180/share"`（会用钥匙串里的凭据），确认 `/Volumes/share` 出现。

## Windows（PowerShell 5.1）

```powershell
# 1) 映射盘符（当前登录会话有效；重启后要重做，或加 /persistent:yes）
net use Z: \\192.168.50.180\share

# 2) 信箱在 Z:\devcard-mailbox
cd <repo>\tools\mailbox
.\mailbox.ps1 peek
.\mailbox.ps1 poll

# 发一条（正文从标准输入；PowerShell 里用管道）
'正文' | .\mailbox.ps1 send --subject "标题" --body -
.\mailbox.ps1 send --subject "标题" --body C:\tmp\msg.md --re 20261114T140200Z-mac-001.md

.\mailbox.ps1 status
```

- 脚本自己解析 `--subject/--body/--re`，所以写法与 macOS 完全一致（不写成 `-Subject`）。
- 默认信箱目录是 `Z:\devcard-mailbox`；盘符不同就设 `$env:DEVCARD_MAILBOX_DIR = 'X:\devcard-mailbox'`。
- 本机身份默认 `win`；要模拟另一端可设 `$env:DEVCARD_SIDE = 'mac'`。
- `mailbox.ps1` **本身必须保存为 UTF-8 带 BOM**：PowerShell 5.1 读无 BOM 的 UTF-8 会按 GBK 解析，中文会变乱码甚至语法错。改动脚本后请确认 BOM 还在。
- 消息文件统一 UTF-8 无 BOM + LF；用记事本另存会加 BOM，不要用记事本改 `inbox/` 里的文件。

## 命令

| 命令 | 作用 |
|---|---|
| `send --subject <s> --body <文件\|-> [--re <对方文件名>]` | 写 `.tmp-` 再改名，序号按本侧历史自增 |
| `peek` | 只列未读（文件名、来源、标题），不移动 |
| `poll` | 按文件名顺序打印全文，读完移到 `reading/`（已读待回） |
| `ack --file <名>` | 回复发出后把 `reading/` 里的消息移到 `archive/` |
| `heartbeat` | 刷新 `presence/<side>.txt` |
| `status` | 信箱是否可用、双方心跳、未读条数 |

## 三条失败行为（不许静默）

1. **信箱目录不存在或不可写**：退出码 1，输出「信箱不可用：…」。**不会**输出「没有新消息」——共享盘掉了和对方没说话是两件事。
2. **对方心跳超过 45 分钟**：`status` 显示「对方（win/mac）离线：上次心跳 X 分钟前」。没有心跳文件也按离线处理。
   轮询间隔约定 3 分钟，但**轮询间隔不等于回复延迟**——agent 只在自己被唤醒时处理消息；45 分钟是 15 倍间隔，避免把「没被唤醒」误判成「下线」。
3. **移动失败**：重试一次，仍失败则报错并保留原文件（消息留在 `inbox/`，下次 `poll` 会再读到）。

## 常见失败

| 现象 | 原因 | 处理 |
|---|---|---|
| `信箱不可用：目录不存在 …` | 共享盘没挂/没映射 | macOS：`open "smb://lyw@192.168.50.180/share"`；Windows：`net use Z: \\192.168.50.180\share` |
| `信箱不可用：… 不可写` | 挂载只读、或 NAS 账号权限不足 | 重挂；确认用的是 `lyw` 账号 |
| `status` 显示对方离线但对方在干活 | 对方没跑 `poll/peek/status`（只有这些命令写心跳） | 让对方跑一次 `status` |
| 消息发出去了对方看不到 | 对方 `DEVCARD_SIDE` 设错，把自己的消息跳过了 | 检查环境变量 |
| 中文乱码 | 用编辑器另存成了 GBK 或带 BOM | 只通过脚本写，正文保持 UTF-8 |

## 边界

不放密码、token、私钥、客户数据——这是同网段可读的明文 SMB。单条正文上限 8 KB，长内容给仓库路径与 commit。
