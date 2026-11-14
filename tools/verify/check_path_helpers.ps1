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
foreach ($candidate in @("python3", "python")) {
    $found = Get-Command $candidate -ErrorAction SilentlyContinue
    if ($found) { $python = @($found.Source); break }
}
if (-not $python) {
    $py = Get-Command "py" -ErrorAction SilentlyContinue
    if ($py) { $python = @($py.Source, "-3") }
}
if (-not $python) {
    Write-Error "需要 Python 3（python3 / python / py -3）"
    exit 3
}

$arguments = @($script)
if ($Root) { $arguments += @("--root", $Root) }
& $python[0] @($python[1..($python.Length - 1)] + $arguments)
exit $LASTEXITCODE
