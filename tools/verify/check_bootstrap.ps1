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
    exit 4
}

$arguments = @($script)
if ($Server) { $arguments += @("--server", $Server) }
if ($DataDir) { $arguments += @("--data-dir", $DataDir) }
if ($BuildDir) { $arguments += @("--build-dir", $BuildDir) }
# $python 只有一个元素时（最常见）$python[1..0] 会取到 $null，命令直接 9009。
$prefix = @()
if ($python.Length -gt 1) { $prefix = @($python[1..($python.Length - 1)]) }
& $python[0] @($prefix + $arguments)
exit $LASTEXITCODE
