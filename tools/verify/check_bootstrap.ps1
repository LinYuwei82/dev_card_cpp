# check_bootstrap.ps1 —— 中文数据目录下 /api/bootstrap 实跑（Windows 入口）
#
# 用法：    powershell -ExecutionPolicy Bypass -File tools\verify\check_bootstrap.ps1
#           [-Server PATH] [-DataDir PATH] [-BuildDir PATH]
# 前置条件：已在 Windows 实机构建 devcard-server.exe（默认 build-new\server\
#           devcard-server.exe）；需要 Python 3。
#           **这条必须在 Windows 实机执行**：macOS 上跑绿只能证明 POSIX 分支，
#           不能替代 ACP 默认代码页下的结论。
# 退出码：  0 通过 / 1 失败 / 3 环境不具备（未构建、服务起不来）/ 4 缺 Python
# 详细判据与「故意造坏输入」示例见 check_bootstrap.py 顶部注释。
param([string]$Server = "", [string]$DataDir = "", [string]$BuildDir = "")

$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$script = Join-Path $here "check_bootstrap.py"

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
    exit 4
}

$arguments = @($script)
if ($Server) { $arguments += @("--server", $Server) }
if ($DataDir) { $arguments += @("--data-dir", $DataDir) }
if ($BuildDir) { $arguments += @("--build-dir", $BuildDir) }
& $python[0] @($python[1..($python.Length - 1)] + $arguments)
exit $LASTEXITCODE
