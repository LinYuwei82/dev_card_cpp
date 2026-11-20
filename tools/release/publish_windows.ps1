# DevCard Windows 一条命令发布：绿色包 + 安装包 → manifest.json（v2）→ 上传 OSS。
#
# 用法（PowerShell，仓库根目录或任意目录均可）：
#   powershell -ExecutionPolicy Bypass -File tools\release\publish_windows.ps1
#   ... -SkipBuild                  # 复用 build\windows 产物
#   ... -Notes "本次更新内容"        # 仅提示用途；v2 清单不再写说明文本
#   ... -Build 2                    # 构建号（发布 tag 的 seq）；缺省时脚本走本地兜底取号
#   ... -DryRun                     # 只出包与清单，不上传
#   ... -Rollout 10                 # 灰度 10%
#   ... -Mandatory -MandatoryReason SECURITY -MandatoryDeadline 2026-12-01T00:00:00Z
#
# 清单为统一 Schema v2（见 mote/docs/更新清单Schema-v2.md）：manifest.json
# （platforms.windows，Ed25519 签名）。v1 清单（latest.json / windows.json）已于
# 2026-11-18 全量删除，本脚本只生成 manifest.json（v2）。
#
# 构建号：v2 清单 windows 平台必须带 build（客户端同版本比对构建号），缺省由
# tools\release\gen_build.py 生成（与 macOS 同口径，自动校验严格大于线上 build）。
#
# 签名种子：优先环境变量 DEVCARD_UPDATE_SIGNING_KEY（CI Secrets，32 字节种子 base64），
# 其次本地 ~/.config/devcard/manifest-keys/ed25519_seed.b64。
#
# 前置条件：
#   - Inno Setup 6（ISCC.exe），找不到会用 choco 安装
#   - OSS 凭据在 %USERPROFILE%\.config\devcard\release.env
#   - python3 / python（manifest_v2.py / guard.py 纯标准库；上传用 tools\release\upload_oss.py）
#
# 版本只认仓库根目录 VERSION.txt。

param(
    [switch]$SkipBuild,
    [string]$Notes = "",
    [string]$Build = "",
    [int]$Rollout = 100,
    [switch]$Mandatory,
    [string]$MandatoryReason = "",
    [string]$MandatoryDeadline = "",
    [switch]$DryRun
)

$ErrorActionPreference = "Stop"

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path

function Fail([string]$Message) {
    Write-Host ""
    Write-Host "[失败] $Message" -ForegroundColor Red
    exit 1
}

# 灰度 / 强制更新参数校验：唯一实现在 tools/release/publish_args.py（三仓库共享）。
# 非 0 退出直接 exit 2，脚本里不再重复 if 校验。
# Windows 上 python3 常是 Microsoft Store 的占位程序（退出码 9009），优先用 python
$pyArgs = Get-Command python -ErrorAction SilentlyContinue
if (-not $pyArgs) { $pyArgs = Get-Command python3 -ErrorAction SilentlyContinue }
if (-not $pyArgs) { Fail "找不到 python（publish_args / manifest_v2 / gen_build 需要）" }
$mandatoryArg = if ($Mandatory) { "1" } else { "0" }
$argsOutput = & $pyArgs.Source (Join-Path $repoRoot "tools\release\publish_args.py") validate `
    "--product=devcard" "--rollout=$Rollout" "--mandatory=$mandatoryArg" `
    "--mandatory-reason=$MandatoryReason" "--mandatory-deadline=$MandatoryDeadline" --emit sh
if ($LASTEXITCODE -ne 0) {
    $argsOutput | Out-Host
    Write-Host "[失败] 发布参数校验失败（publish_args.py 退出码 $LASTEXITCODE）"
    exit 2
}
foreach ($line in @($argsOutput)) {
    if ($line -notmatch "^([A-Z_]+)='(.*)'$") { continue }
    $key = $Matches[1]
    $val = $Matches[2].Replace("'\''", "'")
    switch ($key) {
        "ROLLOUT_PERCENT"    { $Rollout = [int]$val }
        "MANDATORY_ON"       { $Mandatory = ($val -eq "1") }
        "MANDATORY_REASON"   { $MandatoryReason = $val }
        "MANDATORY_DEADLINE" { $MandatoryDeadline = $val }
    }
}

$versionFile = Join-Path $repoRoot "VERSION.txt"
if (-not (Test-Path -LiteralPath $versionFile)) { Fail "找不到 $versionFile" }
$version = (Get-Content -LiteralPath $versionFile -Raw).Trim()
if ([string]::IsNullOrWhiteSpace($version)) { Fail "VERSION.txt 为空" }

# 构建号：缺省走 gen_build.py（year*10^12 + git_count，自动校验 > 线上），
# 不再用 UTC 时间戳——三仓库统一口径，避免 macOS/Windows 各自生成导致撞号。
if ([string]::IsNullOrWhiteSpace($Build)) {
    $genBuild = Join-Path $repoRoot "tools\release\gen_build.py"
    if (-not (Test-Path -LiteralPath $genBuild)) { Fail "找不到 gen_build.py：$genBuild" }
    $py = Get-Command python -ErrorAction SilentlyContinue
    if (-not $py) { $py = Get-Command python3 -ErrorAction SilentlyContinue }
    if (-not $py) { Fail "找不到 python，无法运行 gen_build.py" }
    $Build = (& $py.Source $genBuild --product devcard --repo-root $repoRoot 2>$null | Select-Object -Last 1)
    if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($Build)) {
        Fail "gen_build.py 失败，无法生成构建号（可用 -Build 显式指定）"
    }
    $Build = $Build.Trim()
    Write-Host "build：$Build（来源 gen_build.py）"
}
if ($Build -notmatch '^\d+$') { Fail "非法构建号：$Build（应为纯数字）" }

# 签名种子：环境变量优先（CI），其次本地种子文件
$signKeyPath = $null
if (-not [string]::IsNullOrWhiteSpace($env:DEVCARD_UPDATE_SIGNING_KEY)) {
    $signKeyPath = Join-Path $env:TEMP ("devcard-manifest-key-" + [guid]::NewGuid().ToString("N") + ".b64")
    [IO.File]::WriteAllText($signKeyPath, $env:DEVCARD_UPDATE_SIGNING_KEY.Trim())
} else {
    $signKeyPath = Join-Path $env:USERPROFILE ".config\devcard\manifest-keys\ed25519_seed.b64"
}
if (-not (Test-Path -LiteralPath $signKeyPath)) {
    Fail "v2 清单签名种子不存在：$signKeyPath（或设置环境变量 DEVCARD_UPDATE_SIGNING_KEY）"
}

$dist = Join-Path $repoRoot "dist"
$baseUrl = "https://dl.xiaotu.online/devcard"
$channel = "stable"
$minOs = "10.0"
$keyId = "2026-11"
$stage = Join-Path $dist "DevCard-$version-win64-portable"
$zip = "$stage.zip"
$setup = Join-Path $dist "DevCard-$version-setup.exe"
$manifest = Join-Path $dist "manifest.json"

# v2 安装包命名：<product>-<version>-<os>-<arch>-<type>.<ext>
$v2ZipName = "devcard-$version-win64-portable.zip"
$v2SetupName = "devcard-$version-win64-setup.exe"
$v2Zip = Join-Path $dist $v2ZipName
$v2Setup = Join-Path $dist $v2SetupName

Push-Location $repoRoot
try {
    Write-Host "===== 1/5 打绿色包"
    & (Join-Path $repoRoot "tools\package\package_windows.ps1") `
        -SkipBuild:$SkipBuild -BuildDir build/windows -OutDir dist -Build $Build
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

    Write-Host "===== 3/5 按 v2 命名复制产物并生成 manifest.json"
    # Windows 文件系统大小写不敏感：仅大小写差异时 Copy-Item 会报「覆盖自身」，改用改名
    if ($zip -ieq $v2Zip) {
        Rename-Item -LiteralPath $zip -NewName (Split-Path $v2Zip -Leaf)
    } else {
        Copy-Item -LiteralPath $zip -Destination $v2Zip -Force
    }
    Copy-Item -LiteralPath $setup -Destination $v2Setup -Force

    # 发布说明：仓库存在 docs\release-notes-<版本>.html 时随发布上传，并写进清单
    # platforms.windows.notes_url（与 mote / 小兔一致）；缺页不写字段，避免客户端死链。
    $notesPage = Join-Path $repoRoot "docs\release-notes-$version.html"
    $notesKey = "release-notes-$version.html"
    $notesUrl = $env:DEVCARD_NOTES_URL
    $uploadNotes = $false
    if (-not $notesUrl -and (Test-Path -LiteralPath $notesPage)) {
        $notesUrl = "$baseUrl/$notesKey"
        $uploadNotes = $true
    }

    $manifestArgs = @(
        "generate", "--product", "devcard", "--channel", $channel,
        "--platform", "windows", "--version", $version, "--build", $Build,
        "--min-os", $minOs, "--key-id", $keyId,
        "--artifact", "setup=$v2Setup", "--artifact", "portable=$v2Zip",
        "--key", $signKeyPath, "--out", $manifest
    )
    # 另一平台段可能已在线上：先取线上清单，--prev 合并后整体重签，避免覆盖另一平台段
    $manifestArgs += @("--rollout", "$Rollout")
    if ($Mandatory) {
        $manifestArgs += @("--mandatory", "--mandatory-reason", $MandatoryReason)
        if ($MandatoryDeadline) { $manifestArgs += @("--mandatory-deadline", $MandatoryDeadline) }
    }
    if ($notesUrl) { $manifestArgs += @("--notes-url", $notesUrl) }
    if (-not (Test-Path -LiteralPath $manifest)) {
        try { Invoke-WebRequest -Uri "$baseUrl/updates/$channel/manifest.json" -OutFile $manifest -UseBasicParsing } catch {
            Remove-Item -LiteralPath $manifest -ErrorAction SilentlyContinue
        }
    }
    if (Test-Path -LiteralPath $manifest) { $manifestArgs += @("--prev", $manifest) }
    & python (Join-Path $repoRoot "tools\release\manifest_v2.py") @manifestArgs
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    if (-not (Test-Path -LiteralPath $manifest)) { Fail "没有找到清单：$manifest" }

    # PS5 重定向 python 输出会把 UTF-8 全角冒号按 OEM 编码变乱码，
    # -replace '.*：', '' 正则匹配不上；用临时 .py 文件（写文件时编码固定 UTF-8，
    # Python 读 subprocess 内部输出也是 UTF-8）绕开所有 PS5 重定向编码问题。
    $extractPubPy = Join-Path $env:TEMP "devcard_extract_pub_$(Get-Random).py"
    @'
import sys, subprocess, re
sign_key, manifest_v2 = sys.argv[1], sys.argv[2]
out = subprocess.check_output([sys.executable, manifest_v2, "pubkey", "--key", sign_key], text=True, encoding="utf-8")
m = re.findall(r"[0-9a-fA-F]{64}", out)
if m: print(m[-1].strip())
else: print("ERR: no 64-hex match", file=sys.stderr); sys.exit(1)
'@ | Out-File -FilePath $extractPubPy -Encoding utf8
    try {
        $pubHex = & python $extractPubPy $signKeyPath (Join-Path $repoRoot "tools\release\manifest_v2.py") 2>$null
    } finally {
        Remove-Item -LiteralPath $extractPubPy -Force -ErrorAction SilentlyContinue
    }
    $pubHex = ($pubHex -join "").Trim()
    if ([string]::IsNullOrWhiteSpace($pubHex) -or $pubHex.Length -ne 64) { Fail "无法解析 Ed25519 公钥 hex（PS5 重定向编码问题）" }

    Write-Host "===== 3.5/5 发布门禁（guard，v2）"
    $python = $null
    foreach ($candidate in @("python", "python3")) {
        $cmd = Get-Command $candidate -ErrorAction SilentlyContinue
        if ($cmd -and $cmd.Source -notmatch "WindowsApps") { $python = $cmd.Source; break }
    }
    if (-not $python) {
        # 兜底：扫常见安装路径
        foreach ($p in @(
            "$env:LOCALAPPDATA\Programs\Python\Python311\python.exe",
            "$env:LOCALAPPDATA\Programs\Python\Python312\python.exe",
            "$env:LOCALAPPDATA\Programs\Python\Python310\python.exe",
            "$env:ProgramFiles\Python311\python.exe",
            "$env:ProgramFiles\Python312\python.exe"
        )) { if (Test-Path -LiteralPath $p) { $python = $p; break } }
    }
    if (-not $python) { Fail "找不到 python（guard 需要，Windows Store python3 别名已排除）" }
    $guardArgs = @(
        "--product", "devcard", "--kind", "win", "--channel", $channel,
        "--file", $v2Setup, "--zip", $v2Zip,
        "--manifest", $manifest, "--pub-hex", $pubHex, "--build", $Build
    )
    # guard.py 已移除 --schema 参数（argparse 无此选项），强制 v2
    if ($DryRun) { $guardArgs += "--offline" }
    & $python (Join-Path $repoRoot "tools\release\guard.py") preflight @guardArgs
    if ($LASTEXITCODE -ne 0) { Fail "guard preflight 未通过（v2 命名/版本/覆盖/内嵌地址/签名）" }

    Write-Host "===== 4/5 上传 OSS"
    if ($DryRun) {
        Write-Host "  (dry-run：跳过上传)"
        Write-Host "  $v2Setup`n  $v2Zip`n  $manifest"
        if ($uploadNotes) { Write-Host "  $notesPage → devcard/$notesKey" }
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
    if ($uploadNotes) {
        & $python $uploader $notesPage --prefix "devcard/"
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    }
    & $python $uploader $v2Setup $v2Zip --prefix "devcard/"
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    & $python $uploader $manifest --prefix "devcard/updates/$channel/"
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

    Write-Host "===== 5/5 线上校验"
    $checkUrls = @(
        "$baseUrl/$v2SetupName",
        "$baseUrl/$v2ZipName",
        "$baseUrl/updates/$channel/manifest.json"
    )
    if ($notesUrl) { $checkUrls += $notesUrl }
    foreach ($url in $checkUrls) {
        try {
            $resp = Invoke-WebRequest -Uri $url -Method Head -UseBasicParsing -TimeoutSec 20
            if ($resp.StatusCode -ne 200) { Fail "$url 返回 HTTP $($resp.StatusCode)" }
            Write-Host "  ✓ $url"
        } catch {
            Fail "$url 校验失败：$($_.Exception.Message)"
        }
    }

    Write-Host "===== 5.5/5 线上门禁（guard，v2）"
    & $python (Join-Path $repoRoot "tools\release\guard.py") verify-live @guardArgs
    if ($LASTEXITCODE -ne 0) { Fail "guard verify-live 未通过（清单/大小/SHA/Content-Type）" }

    Write-Host ""
    Write-Host "发布完成：DevCard $version（Windows x64，build $Build）"
}
finally {
    Pop-Location
    if ($env:DEVCARD_UPDATE_SIGNING_KEY -and $signKeyPath -and (Test-Path -LiteralPath $signKeyPath) -and
        $signKeyPath.StartsWith($env:TEMP)) {
        Remove-Item -LiteralPath $signKeyPath -Force -ErrorAction SilentlyContinue
    }
}
