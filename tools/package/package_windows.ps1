# DevCard Windows 绿色版打包：构建 → 暂存 → 版本/内容自检 → zip → 体积与 SHA256。
#
# 用法（在 PowerShell 里，仓库根目录或任意目录均可）：
#   powershell -ExecutionPolicy Bypass -File tools\package\package_windows.ps1
#   powershell -ExecutionPolicy Bypass -File tools\package\package_windows.ps1 -SkipBuild
#   ... -BuildDir D:\build\devcard -OutDir D:\dist
#
# 产物：<OutDir>\DevCard-<版本>-win64-portable.zip（默认 dist\）
# 包内结构：DevCard-<版本>-win64-portable\{DevCard.exe, devcard-server.exe, fonts\, VERSION.txt}
# 绿色版数据目录在 exe 旁 data\，由程序首次启动创建；打包时不含任何 data\ 或数据库文件。
#
# 版本真值只有仓库根目录 VERSION.txt：文件名、包内 VERSION.txt、两个 exe 的
# ProductVersion 三处必须一致；不一致直接失败，不产出一个「能解压但版本对不上」的包。

param(
    [switch]$SkipBuild,
    [string]$BuildDir = "",
    [string]$OutDir = "",
    [string]$Build = ""
)

$ErrorActionPreference = "Stop"

function Fail([string]$Message) {
    Write-Host ""
    Write-Host "[失败] $Message" -ForegroundColor Red
    exit 1
}

if ($env:OS -ne "Windows_NT") {
    Fail "package_windows.ps1 只能在 Windows 上运行"
}

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
if ([string]::IsNullOrWhiteSpace($BuildDir)) {
    $BuildDir = Join-Path $repoRoot "build\windows"
}
if ([string]::IsNullOrWhiteSpace($OutDir)) {
    $OutDir = Join-Path $repoRoot "dist"
}

$versionFile = Join-Path $repoRoot "VERSION.txt"
if (-not (Test-Path -LiteralPath $versionFile)) {
    Fail "找不到版本文件：$versionFile"
}
$version = (Get-Content -LiteralPath $versionFile -Raw).Trim()
if ([string]::IsNullOrWhiteSpace($version)) {
    Fail "VERSION.txt 为空"
}
Write-Host "版本：$version（来源 VERSION.txt）"

# ---------- 1. 构建 ----------
# 构建号：显式 -Build 优先；缺省时与另两仓一致，用 gen_build.py 自动生成，再通过
# -DDEVCARD_BUILD 注入编译期宏 DEVCARD_BUILD，并打开严格发布开关。
if ([string]::IsNullOrWhiteSpace($Build)) {
    $genBuild = Join-Path $repoRoot "tools\release\gen_build.py"
    if (-not (Test-Path -LiteralPath $genBuild)) { Fail "缺少 $genBuild，无法生成构建号" }
    # Windows 上 python3 常是 Store 占位程序，优先用 python
    $py = Get-Command python -ErrorAction SilentlyContinue
    if (-not $py) { $py = Get-Command python3 -ErrorAction SilentlyContinue }
    if (-not $py) { Fail "未找到 python，无法调用 gen_build.py" }
    $prevEap = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    $genOutput = & $py.Source $genBuild --product devcard --repo-root $repoRoot 2>&1
    $ErrorActionPreference = $prevEap
    $genText = ($genOutput | Out-String).Trim()
    if ($LASTEXITCODE -ne 0 -or $genText -notmatch '^\d+$') {
        Fail "gen_build.py 生成构建号失败（退出码 $LASTEXITCODE）：$genText"
    }
    $Build = $genText
    Write-Host "构建号：$Build（来源 gen_build.py）"
}

$buildStampArg = @("-DDEVCARD_BUILD=$Build", "-DDEVCARD_RELEASE_BUILD=ON")
if (-not $SkipBuild) {
    Write-Host "构建：$BuildDir（Release）"
    Write-Host "  cmake configure：$BuildDir -DDEVCARD_BUILD=$Build"
    & cmake -S $repoRoot -B $BuildDir -DCMAKE_BUILD_TYPE=Release @buildStampArg
    if ($LASTEXITCODE -ne 0) { Fail "cmake configure 失败（退出码 $LASTEXITCODE）" }
    & cmake --build $BuildDir --config Release
    if ($LASTEXITCODE -ne 0) { Fail "cmake --build 失败（退出码 $LASTEXITCODE）" }
} else {
    Write-Host "构建：跳过（-SkipBuild）"
    # SkipBuild 但带 -Build：先 re-configure 把构建号写入 CMakeCache，
    # 然后增量 rebuild——CMake 会检测到编译期宏 DEVCARD_BUILD 变化，
    # 重新编译 update_manifest.cpp（唯一引用点）；clean-first 代价太高，
    # 增量足够且更快。
    if (-not [string]::IsNullOrWhiteSpace($Build)) {
        Write-Host " （带 -Build 但 SkipBuild，re-configure + 增量 rebuild）"
        & cmake -S $repoRoot -B $BuildDir -DCMAKE_BUILD_TYPE=Release @buildStampArg
        if ($LASTEXITCODE -ne 0) { Fail "cmake re-configure 失败（退出码 $LASTEXITCODE）" }
        & cmake --build $BuildDir --config Release
        if ($LASTEXITCODE -ne 0) { Fail "cmake --build（增量）失败（退出码 $LASTEXITCODE）" }
    }
}

# 多配置生成器（VS）产物在 <target>\Release\ 下；单配置生成器（Ninja）直接在
# <target>\ 或构建根目录。按已知布局逐个查找，不递归扫 Debug 目录。
function Find-Exe([string]$Name, [string[]]$SubDirs) {
    $candidates = @()
    foreach ($sub in $SubDirs) {
        $candidates += (Join-Path $BuildDir "$sub\Release\$Name")
        $candidates += (Join-Path $BuildDir "$sub\$Name")
    }
    $candidates += (Join-Path $BuildDir "Release\$Name")
    $candidates += (Join-Path $BuildDir "$Name")
    foreach ($path in $candidates) {
        if (Test-Path -LiteralPath $path) { return (Resolve-Path -LiteralPath $path).Path }
    }
    Fail "找不到 $Name（已查：$($candidates -join ' / ')）。先不带 -SkipBuild 跑一次。"
}

$appExe = Find-Exe "DevCard.exe" @("shell\win")
$serverExe = Find-Exe "devcard-server.exe" @("server")

# 版本先校验再打包：改过 VERSION.txt 但没有重新 configure 时，
# PE 版本号仍是旧值，增量构建不会更新它——这种包发出去就是错的。
function Assert-ExeVersion([string]$Path) {
    $info = (Get-Item -LiteralPath $Path).VersionInfo
    if ($info.ProductVersion -ne $version) {
        Fail "$(Split-Path $Path -Leaf) 的 ProductVersion 是 $($info.ProductVersion)，与 VERSION.txt 的 $version 不一致；请重新执行 cmake configure 后再构建"
    }
}
Assert-ExeVersion $appExe
Assert-ExeVersion $serverExe
Write-Host "版本自检：两个 exe 的 ProductVersion 均为 $version"

# ---------- 2. 暂存 ----------
$stageName = "DevCard-$version-win64-portable"
$stage = Join-Path $OutDir $stageName
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
if (Test-Path -LiteralPath $stage) {
    Remove-Item -LiteralPath $stage -Recurse -Force
}
New-Item -ItemType Directory -Force -Path (Join-Path $stage "fonts") | Out-Null

Copy-Item -LiteralPath $appExe -Destination $stage
Copy-Item -LiteralPath $serverExe -Destination $stage
Copy-Item -LiteralPath $versionFile -Destination $stage

# 字体从仓库取，不从构建产物取：构建产物可能过期，而这里是打包判据本身。
$fonts = @("NotoSansSC-Regular.subset.ttf", "NotoSansSC-Bold.subset.ttf")
foreach ($font in $fonts) {
    $src = Join-Path $repoRoot "web\fonts\$font"
    if (-not (Test-Path -LiteralPath $src)) {
        Fail "仓库缺少子集字体：$src（先跑 tools/fonts/make_subset.sh）"
    }
    if ((Get-Item -LiteralPath $src).Length -le 0) {
        Fail "子集字体是空文件：$src"
    }
    Copy-Item -LiteralPath $src -Destination (Join-Path $stage "fonts\$font")
    $hash = (Get-FileHash -LiteralPath $src -Algorithm SHA256).Hash.ToLowerInvariant()
    Write-Host ("  字体 {0}  {1} bytes  {2}" -f $font, (Get-Item -LiteralPath $src).Length, $hash)
}

# ---------- 3. 包内容自检 ----------
$required = @(
    "DevCard.exe",
    "devcard-server.exe",
    "VERSION.txt",
    "fonts\NotoSansSC-Regular.subset.ttf",
    "fonts\NotoSansSC-Bold.subset.ttf"
)
foreach ($name in $required) {
    $path = Join-Path $stage $name
    if (-not (Test-Path -LiteralPath $path)) { Fail "包内缺少：$name" }
    if ((Get-Item -LiteralPath $path).Length -le 0) { Fail "包内文件为空：$name" }
}
$stagedVersion = (Get-Content -LiteralPath (Join-Path $stage "VERSION.txt") -Raw).Trim()
if ($stagedVersion -ne $version) {
    Fail "包内 VERSION.txt（$stagedVersion）与 VERSION.txt（$version）不一致"
}
# 绿色版数据必须落在解压目录 data\，打包不得夹带任何运行期数据。
$leaked = Get-ChildItem -LiteralPath $stage -Recurse -File |
    Where-Object { $_.Name -like "*.db" -or $_.Name -like "*.db-wal" -or $_.Name -like "*.db-shm" }
if ($leaked) {
    Fail "包内混入了数据库文件：$($leaked.FullName -join ', ')"
}
Write-Host "包内容自检：通过（$($required.Count) 项齐全、无运行期数据）"

# ---------- 4. zip ----------
$zip = Join-Path $OutDir "$stageName.zip"
if (Test-Path -LiteralPath $zip) { Remove-Item -LiteralPath $zip -Force }
Compress-Archive -Path $stage -DestinationPath $zip -CompressionLevel Optimal
if (-not (Test-Path -LiteralPath $zip)) { Fail "压缩失败：$zip 未生成" }

$bytes = (Get-Item -LiteralPath $zip).Length
$sha = (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash.ToLowerInvariant()

Write-Host ""
Write-Host "打包完成"
Write-Host "  产物：$zip"
Write-Host "  版本：$version"
Write-Host ("  体积：{0} bytes（{1:N2} MiB）" -f $bytes, ($bytes / 1MB))
Write-Host "  SHA256：$sha"
Write-Host "  解压后运行 DevCard.exe；数据目录为 exe 旁的 data\，卸载/升级时保留该目录。"
exit 0
