# check_path_helpers.ps1 —— 路径 helper 唯一性与调用点盘点（Windows 入口）
#
# 用法：    powershell -ExecutionPolicy Bypass -File tools\verify\check_path_helpers.ps1
#           [-Root PATH]
# 前置条件：需要 Python 3（python3 / python / py -3 任一可用）。只读源码。
# 退出码：  0 通过 / 1 某个 helper 有 2 份及以上定义 / 3 缺 Python
# 详细判据与「故意造坏输入」示例见 check_path_helpers.py 顶部注释。
param([string]$Root = "")

$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$script = Join-Path $here "check_path_helpers.py"

$python = $null
# Windows 的 WindowsApps 里有 python3.exe 的应用执行别名，Get-Command 能找到它，
# 但运行起来直接 9009。所以候选必须真跑一次 --version 才算数。
foreach ($candidate in @(@("python"), @("python3"), @("py", "-3"))) {
    $found = Get-Command $candidate[0] -ErrorAction SilentlyContinue
    if (-not $found) { continue }
    $pre = @()
    if ($candidate.Length -gt 1) { $pre = @($candidate[1..($candidate.Length - 1)]) }
    & $found.Source @($pre + @("--version")) *> $null
    if ($LASTEXITCODE -eq 0) { $python = @($found.Source) + $pre; break }
}
if (-not $python) {
    Write-Error "需要 Python 3（python3 / python / py -3）"
    exit 3
}

$arguments = @($script)
if ($Root) { $arguments += @("--root", $Root) }
# $python 只有一个元素时（最常见）$python[1..0] 会取到 $null，命令直接 9009。
$prefix = @()
if ($python.Length -gt 1) { $prefix = @($python[1..($python.Length - 1)]) }
& $python[0] @($prefix + $arguments)
exit $LASTEXITCODE
