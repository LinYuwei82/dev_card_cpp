# check_migrations.ps1 —— 迁移号与归属核对（Windows 入口）
#
# 用法：    powershell -ExecutionPolicy Bypass -File tools\verify\check_migrations.ps1
#           [-File PATH]
# 前置条件：需要 Python 3（python3 / python / py -3 任一可用）。只读源码，不构建。
# 退出码：  0 通过 / 1 失败 / 2 出现 v4+ 待与 Windows 确认 / 3 缺 Python
# 详细判据与「故意造坏输入」示例见 check_migrations.py 顶部注释。
param([string]$File = "")

$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$script = Join-Path $here "check_migrations.py"

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
if ($File) { $arguments += @("--file", $File) }
& $python[0] @($python[1..($python.Length - 1)] + $arguments)
exit $LASTEXITCODE
