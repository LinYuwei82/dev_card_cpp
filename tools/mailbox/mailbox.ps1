# dev_card_cpp 双机会话信箱（Windows / PowerShell 5.1）。
# 协议见信箱根目录的 PROTOCOL.md（v2），本脚本不擅自扩展协议；接口与 mailbox.sh 一致。
#
# 用法：
#   .\mailbox.ps1 send --subject <标题> --body <文件|-> [--re <对方文件名>]
#   .\mailbox.ps1 poll
#   .\mailbox.ps1 peek
#   .\mailbox.ps1 ack --file <文件名>
#   .\mailbox.ps1 heartbeat
#   .\mailbox.ps1 status
#
# 环境变量：
#   DEVCARD_MAILBOX_DIR  信箱目录，默认 Z:\devcard-mailbox（Z: 用 net use 映射到共享盘）
#   DEVCARD_SIDE         本机身份 mac|win，默认 win
#
# 节奏约定：两边每 3 分钟 poll 一次（由外部定时任务执行，脚本本身不睡眠），
# 离线判据 45 分钟。轮询间隔不等于回复延迟，只有心跳能证明对方在。
#
# 本文件必须保存为 UTF-8 带 BOM：PowerShell 5.1 读无 BOM 的 UTF-8 会按 GBK 解析，
# 中文注释与字符串会变乱码，甚至直接语法错。
#
# 这里故意不写 param()：PowerShell 会把 --subject 当成参数名去绑定，
# 而协议要求两个平台用同样的 -- 前缀，所以自己解析 $args。

$ErrorActionPreference = 'Stop'

$MailboxDir = if ($env:DEVCARD_MAILBOX_DIR) { $env:DEVCARD_MAILBOX_DIR } else { 'Z:\devcard-mailbox' }
$Inbox      = Join-Path $MailboxDir 'inbox'
$Reading    = Join-Path $MailboxDir 'reading'
$Archive    = Join-Path $MailboxDir 'archive'
$Presence   = Join-Path $MailboxDir 'presence'
$Side       = if ($env:DEVCARD_SIDE) { $env:DEVCARD_SIDE } else { 'win' }
$Peer       = if ($Side -eq 'mac') { 'win' } elseif ($Side -eq 'win') { 'mac' } else { '' }
$OfflineSeconds = 45 * 60
$MaxBodyBytes   = 8192
$Utf8NoBom = New-Object System.Text.UTF8Encoding($false)

if (-not $Peer) {
  Write-Error "DEVCARD_SIDE 只能是 mac 或 win（当前：$Side）"
  exit 2
}

function Get-UtcStamp { (Get-Date).ToUniversalTime().ToString('yyyyMMddTHHmmssZ', [Globalization.CultureInfo]::InvariantCulture) }
function Get-UtcNow   { (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ', [Globalization.CultureInfo]::InvariantCulture) }

function Stop-MailboxUnavailable([string]$Reason) {
  [Console]::Error.WriteLine("信箱不可用：$Reason")
  exit 1
}

# 目录不在或不可写，一律当「信箱不可用」——绝不退化成「没有新消息」。
function Assert-Mailbox {
  if (-not (Test-Path -LiteralPath $MailboxDir -PathType Container)) {
    Stop-MailboxUnavailable "目录不存在 $MailboxDir（共享盘没映射？先 net use Z: \\192.168.50.180\share）"
  }
  foreach ($dir in @($Inbox, $Reading, $Archive, $Presence)) {
    if (-not (Test-Path -LiteralPath $dir -PathType Container)) { Stop-MailboxUnavailable "缺少 $dir" }
  }
  # SMB 上 ACL 未必如实反映，直接试写一个探针文件比看属性可靠。
  $probe = Join-Path $Inbox ('.tmp-write-probe-' + $PID)
  try {
    [System.IO.File]::WriteAllText($probe, 'x', $Utf8NoBom)
    Remove-Item -LiteralPath $probe -Force -ErrorAction SilentlyContinue
  } catch {
    Stop-MailboxUnavailable "$Inbox 不可写（挂载只读或权限不足）"
  }
}

function Write-Heartbeat {
  $tmp = Join-Path $Presence ('.tmp-' + $Side + '.txt')
  try {
    [System.IO.File]::WriteAllText($tmp, (Get-UtcNow) + "`n", $Utf8NoBom)
    Move-Item -LiteralPath $tmp -Destination (Join-Path $Presence ($Side + '.txt')) -Force
  } catch {
    Remove-Item -LiteralPath $tmp -Force -ErrorAction SilentlyContinue
    [Console]::Error.WriteLine("警告：心跳写入失败（$(Join-Path $Presence ($Side + '.txt'))），不影响消息读写")
  }
}

function Get-NextSeq {
  $max = 0
  foreach ($dir in @($Inbox, $Reading, $Archive)) {
    Get-ChildItem -LiteralPath $dir -File -ErrorAction SilentlyContinue | ForEach-Object {
      if ($_.Name -match ('-' + [regex]::Escape($Side) + '-(\d{3})\.md$')) {
        $n = [int]$Matches[1]
        if ($n -gt $max) { $max = $n }
      }
    }
  }
  return ('{0:D3}' -f ($max + 1))
}

# 移动失败重试一次；仍失败就报错并保留源文件，绝不丢内容。
function Move-WithRetry([string]$Source, [string]$Destination) {
  try {
    Move-Item -LiteralPath $Source -Destination $Destination -ErrorAction Stop
    return $true
  } catch {
    Start-Sleep -Seconds 1
    try {
      Move-Item -LiteralPath $Source -Destination $Destination -ErrorAction Stop
      return $true
    } catch {
      [Console]::Error.WriteLine("移动失败：$Source → $Destination（源文件已保留）")
      return $false
    }
  }
}

function Get-PeerMessages {
  @(Get-ChildItem -LiteralPath $Inbox -File -ErrorAction SilentlyContinue |
      Where-Object { $_.Name -notlike '.tmp-*' -and $_.Name -notlike ('*-' + $Side + '-*') } |
      Sort-Object Name)
}

function Invoke-Send {
  $subject = ''; $body = ''; $re = '-'
  $i = 0
  while ($i -lt $args.Count) {
    switch ($args[$i]) {
      '--subject' { $i++; $subject = [string]$args[$i] }
      '--body'    { $i++; $body = [string]$args[$i] }
      '--re'      { $i++; $re = [string]$args[$i] }
      default {
        if ($args[$i]) { [Console]::Error.WriteLine("未知参数：$($args[$i])"); exit 2 }
      }
    }
    $i++
  }
  if (-not $subject) { [Console]::Error.WriteLine('缺少 --subject'); exit 2 }
  if (-not $body)    { [Console]::Error.WriteLine('缺少 --body（用 - 表示标准输入）'); exit 2 }

  Assert-Mailbox

  if ($body -eq '-') {
    $text = [Console]::In.ReadToEnd()
  } else {
    if (-not (Test-Path -LiteralPath $body -PathType Leaf)) {
      [Console]::Error.WriteLine("正文文件不存在：$body"); exit 2
    }
    $text = [System.IO.File]::ReadAllText($body, $Utf8NoBom)
  }
  $bytes = $Utf8NoBom.GetByteCount($text)
  if ($bytes -gt $MaxBodyBytes) {
    [Console]::Error.WriteLine("正文字节数 $bytes 超过上限 $MaxBodyBytes（长内容给路径与 commit）")
    exit 2
  }

  $name = (Get-UtcStamp) + '-' + $Side + '-' + (Get-NextSeq) + '.md'
  $tmp  = Join-Path $Inbox ('.tmp-' + $name)
  $content = 're: ' + $re + "`n" + 'from: ' + $Side + "`n" + 'subject: ' + $subject + "`n---`n`n" + $text.TrimEnd("`r", "`n") + "`n"
  try {
    [System.IO.File]::WriteAllText($tmp, $content, $Utf8NoBom)
  } catch {
    Stop-MailboxUnavailable "写入 $tmp 失败：$($_.Exception.Message)"
  }
  if (-not (Move-WithRetry $tmp (Join-Path $Inbox $name))) { exit 1 }
  Write-Output "已发送：$name"
}

function Invoke-Poll {
  Assert-Mailbox
  Write-Heartbeat
  $items = Get-PeerMessages
  $count = 0
  foreach ($item in $items) {
    $count++
    Write-Output ('===== ' + $item.Name + ' =====')
    Write-Output ([System.IO.File]::ReadAllText($item.FullName, $Utf8NoBom))
    # 读过但没回：移到 reading/，发送方据此能区分「没读」和「读了没回」。
    if (-not (Move-WithRetry $item.FullName (Join-Path $Reading $item.Name))) {
      [Console]::Error.WriteLine('（移动失败，消息仍在 inbox，下次 poll 会再读到）')
    }
  }
  if ($count -eq 0) { Write-Output '没有待读消息（inbox 为空，或只有自己发的）' }
  else { Write-Output "共 $count 条，已移入 reading/（回了之后用 ack 归档）" }
}

function Invoke-Peek {
  Assert-Mailbox
  Write-Heartbeat
  $items = Get-PeerMessages
  foreach ($item in $items) {
    Write-Output $item.Name
    $head = [System.IO.File]::ReadAllLines($item.FullName, $Utf8NoBom) | Select-Object -First 3
    foreach ($line in $head) {
      if ($line -like 'from: *')    { Write-Output ('  来自: ' + $line.Substring(6)) }
      if ($line -like 'subject: *') { Write-Output ('  标题: ' + $line.Substring(9)) }
    }
  }
  Write-Output "未读 $($items.Count) 条（peek 不移动文件）"
}

# reading/ → archive/：回复发出后调用，只移动不改内容。
function Invoke-Ack {
  $file = ''
  $i = 0
  while ($i -lt $args.Count) {
    switch ($args[$i]) {
      '--file' { $i++; $file = [string]$args[$i] }
      default {
        if ($args[$i]) { [Console]::Error.WriteLine("未知参数：$($args[$i])"); exit 2 }
      }
    }
    $i++
  }
  if (-not $file) { [Console]::Error.WriteLine('缺少 --file <文件名>'); exit 2 }
  Assert-Mailbox
  $source = Join-Path $Reading $file
  if (-not (Test-Path -LiteralPath $source -PathType Leaf)) {
    [Console]::Error.WriteLine("reading/ 里没有 $file（先 poll 读过才能 ack）")
    exit 2
  }
  if (-not (Move-WithRetry $source (Join-Path $Archive $file))) { exit 1 }
  Write-Output "已归档：$file"
}

function Invoke-Status {
  Assert-Mailbox
  Write-Heartbeat
  Write-Output "信箱：$MailboxDir（可写）"
  Write-Output "本机：$Side"

  $now = (Get-Date).ToUniversalTime()
  $selfFile = Join-Path $Presence ($Side + '.txt')
  if (Test-Path -LiteralPath $selfFile) {
    $selfStamp = ([System.IO.File]::ReadAllText($selfFile, $Utf8NoBom)).Trim()
    $selfTime = [datetime]::MinValue
    $selfOk = [datetime]::TryParseExact(
      $selfStamp, 'yyyy-MM-ddTHH:mm:ssZ',
      [Globalization.CultureInfo]::InvariantCulture,
      [Globalization.DateTimeStyles]::AssumeUniversal, [ref]$selfTime)
    if ($selfOk) {
      $selfMinutes = [int](($now - $selfTime.ToUniversalTime()).TotalSeconds / 60)
      Write-Output "本机心跳：$selfStamp（$selfMinutes 分钟前）"
    } else {
      Write-Output "本机心跳：$selfStamp（无法解析）"
    }
  }

  $peerFile = Join-Path $Presence ($Peer + '.txt')
  if (-not (Test-Path -LiteralPath $peerFile)) {
    Write-Output "对方（$Peer）离线：没有心跳文件 $peerFile"
  } else {
    $peerStamp = ([System.IO.File]::ReadAllText($peerFile, $Utf8NoBom)).Trim()
    $peerTime = [datetime]::MinValue
    $parsed = [datetime]::TryParseExact(
      $peerStamp, 'yyyy-MM-ddTHH:mm:ssZ',
      [Globalization.CultureInfo]::InvariantCulture,
      [Globalization.DateTimeStyles]::AssumeUniversal, [ref]$peerTime)
    if (-not $parsed) {
      Write-Output "对方（$Peer）心跳无法解析：$peerStamp"
    } else {
      $age = ($now - $peerTime.ToUniversalTime()).TotalSeconds
      $minutes = [int]($age / 60)
      # 不只给在线/离线：差多少分钟要能一眼看出来。
      if ($age -gt $OfflineSeconds) {
        Write-Output "对方（$Peer）离线：上次心跳 $minutes 分钟前（$peerStamp）"
      } else {
        Write-Output "对方（$Peer）在线：上次心跳 $minutes 分钟前（$peerStamp）"
      }
    }
  }

  $readingCount = @(Get-ChildItem -LiteralPath $Reading -File -ErrorAction SilentlyContinue |
      Where-Object { $_.Name -notlike '.tmp-*' }).Count
  Write-Output "未读消息：$((Get-PeerMessages).Count) 条，已读待回：$readingCount 条"
}

$command = if ($args.Count -gt 0) { [string]$args[0] } else { '' }
$rest = if ($args.Count -gt 1) { @($args[1..($args.Count - 1)]) } else { @() }

switch ($command) {
  'send'      { Invoke-Send @rest }
  'poll'      { Invoke-Poll }
  'peek'      { Invoke-Peek }
  'ack'       { Invoke-Ack @rest }
  'heartbeat' { Assert-Mailbox; Write-Heartbeat; Write-Output "心跳已更新：$(Join-Path $Presence ($Side + '.txt'))" }
  'status'    { Invoke-Status }
  ''          { Get-Help $PSCommandPath -Detailed }
  default     { [Console]::Error.WriteLine("未知命令：$command"); exit 2 }
}
