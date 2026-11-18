# DevCard Windows 一条命令发布：绿色包 + 安装包 → windows.json → 上传 OSS。
#
# 用法（PowerShell，仓库根目录或任意目录均可）：
#   powershell -ExecutionPolicy Bypass -File tools\release\publish_windows.ps1
#   ... -SkipBuild                  # 复用 build-win 产物
#   ... -Notes "本次更新内容"        # 写进 windows.json
#   ... -DryRun                     # 只出包与清单，不上传
#
# 前置条件：
#   - Inno Setup 6（ISCC.exe），找不到会用 choco 安装
#   - OSS 凭据在 %USERPROFILE%\.config\devcard\release.env
#   - python3 / python（上传用 tools\release\upload_oss.py）
#
# 版本只认仓库根目录 VERSION.txt。

param(
    [switch]$SkipBuild,
    [string]$Notes = "",
    [switch]$DryRun
)

$ErrorActionPreference = "Stop"

function Fail([string]$Message) {
    Write-Host ""
    Write-Host "[失败] $Message" -ForegroundColor Red
    exit 1
}

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$versionFile = Join-Path $repoRoot "VERSION.txt"
if (-not (Test-Path -LiteralPath $versionFile)) { Fail "找不到 $versionFile" }
$version = (Get-Content -LiteralPath $versionFile -Raw).Trim()
if ([string]::IsNullOrWhiteSpace($version)) { Fail "VERSION.txt 为空" }

$dist = Join-Path $repoRoot "dist"
$baseUrl = "https://dl.xiaotu.online/devcard"
$channel = "stable"
$stage = Join-Path $dist "DevCard-$version-win64-portable"
$zip = "$stage.zip"
$setup = Join-Path $dist "DevCard-$version-setup.exe"
$manifest = Join-Path $dist "windows.json"

Push-Location $repoRoot
try {
    Write-Host "===== 1/5 打绿色包"
    & (Join-Path $repoRoot "tools\package\make_portable_win.ps1") `
        -SkipBuild:$SkipBuild -BuildDir build-win -OutDir dist
    if ($null -ne $LASTEXITCODE -and $LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    if (-not (Test-Path -LiteralPath $zip)) { Fail "没有找到产物：$zip" }

    Write-Host "===== 2/5 打安装包"
    $iscc = @(
        "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
        "$env:ProgramFiles\Inno Setup 6\ISCC.exe",
        "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe"
    ) | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
    if (-not $iscc) {
        Write-Host "  未找到 Inno Setup 6，尝试用 choco 安装…"
        choco install innosetup --no-progress -y
        $iscc = @(
            "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
            "$env:ProgramFiles\Inno Setup 6\ISCC.exe",
            "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe"
        ) | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
    }
    if (-not $iscc) { Fail "找不到 Inno Setup 6（ISCC.exe）" }

    & $iscc "/DSourceRoot=$stage" "packaging\windows\DevCard.iss"
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    if (-not (Test-Path -LiteralPath $setup)) { Fail "没有找到安装包：$setup" }

    Write-Host "===== 3/5 生成 windows.json"
    $manifestArgs = @{ Platform = "windows" }
    if (-not [string]::IsNullOrWhiteSpace($Notes)) { $manifestArgs.Notes = $Notes }
    & (Join-Path $repoRoot "tools\package\make_update_json.ps1") @manifestArgs
    if ($null -ne $LASTEXITCODE -and $LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    if (-not (Test-Path -LiteralPath $manifest)) { Fail "没有找到清单：$manifest" }

    Write-Host "===== 3.5/5 发布门禁（guard）"
    $python = $null
    foreach ($candidate in @("python3", "python")) {
        $cmd = Get-Command $candidate -ErrorAction SilentlyContinue
        if ($cmd) { $python = $cmd.Source; break }
    }
    if (-not $python) { Fail "找不到 python3 / python（guard 需要）" }
    $guardArgs = @("--product", "devcard", "--kind", "win",
                   "--file", $setup, "--zip", $zip)
    if ($DryRun) { $guardArgs += "--offline" }
    & $python (Join-Path $repoRoot "tools\release\guard.py") preflight @guardArgs
    if ($LASTEXITCODE -ne 0) { Fail "guard preflight 未通过（命名/版本/覆盖/内嵌地址）" }

    Write-Host "===== 4/5 上传 OSS"
    if ($DryRun) {
        Write-Host "  (dry-run：跳过上传)"
        Write-Host "  $setup`n  $zip`n  $manifest"
        return
    }

    $python = $null
    foreach ($candidate in @("python3", "python")) {
        $cmd = Get-Command $candidate -ErrorAction SilentlyContinue
        if ($cmd) { $python = $cmd.Source; break }
    }
    if (-not $python) { Fail "找不到 python3 / python（上传脚本需要）" }

    $envFile = Join-Path $env:USERPROFILE ".config\devcard\release.env"
    if (Test-Path -LiteralPath $envFile) {
        foreach ($line in Get-Content -LiteralPath $envFile) {
            $text = $line.Trim()
            if (-not $text -or $text.StartsWith("#")) { continue }
            if ($text.StartsWith("export ")) { $text = $text.Substring(7) }
            $eq = $text.IndexOf("=")
            if ($eq -lt 1) { continue }
            $key = $text.Substring(0, $eq).Trim()
            $value = $text.Substring($eq + 1).Trim().Trim('"').Trim("'")
            Set-Item -Path "Env:$key" -Value $value
        }
    }
    foreach ($name in @("OSS_ACCESS_KEY_ID", "OSS_ACCESS_KEY_SECRET", "OSS_ENDPOINT", "OSS_BUCKET")) {
        if (-not (Test-Path "Env:$name")) { Fail "环境变量 $name 未设置（见 $envFile）" }
    }

    $uploader = Join-Path $repoRoot "tools\release\upload_oss.py"
    & $python $uploader $setup $zip --prefix "devcard/"
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    & $python $uploader $manifest --prefix "devcard/updates/$channel/"
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

    Write-Host "===== 5/5 线上校验"
    foreach ($url in @(
        "$baseUrl/DevCard-$version-setup.exe",
        "$baseUrl/DevCard-$version-win64-portable.zip",
        "$baseUrl/updates/$channel/windows.json"
    )) {
        try {
            $resp = Invoke-WebRequest -Uri $url -Method Head -UseBasicParsing -TimeoutSec 20
            if ($resp.StatusCode -ne 200) { Fail "$url 返回 HTTP $($resp.StatusCode)" }
            Write-Host "  ✓ $url"
        } catch {
            Fail "$url 校验失败：$($_.Exception.Message)"
        }
    }

    Write-Host "===== 5.5/5 线上门禁（guard）"
    & $python (Join-Path $repoRoot "tools\release\guard.py") verify-live `
        --product devcard --kind win --file $setup --zip $zip
    if ($LASTEXITCODE -ne 0) { Fail "guard verify-live 未通过（清单/大小/SHA/Content-Type）" }

    Write-Host ""
    Write-Host "发布完成：DevCard $version（Windows x64）"
}
finally {
    Pop-Location
}
