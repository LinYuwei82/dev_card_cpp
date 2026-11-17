# DevCard 更新清单生成：按线上清单格式输出，避免手写漏字段。
#
# 用法（仓库根目录或任意目录均可）：
#   powershell -ExecutionPolicy Bypass -File tools\package\make_update_json.ps1
#   ... -Platform windows -SetupPath dist\DevCard-1.2.2-setup.exe -Notes "本次更新内容"
#   ... -Platform macos -ExtraAssets dist\DevCard-1.2.3-macos-arm64.dmg
#   ... -BaseUrl https://dl.xiaotu.online/devcard/ -Channel stable
#
# 清单统一放在 <product>/updates/<channel>/ 下：Windows 输出 windows.json，
# macOS 输出 latest.json。清单只描述已经存在的文件：缺文件、空文件、
# 文件名与版本对不上都直接失败，不产出一个指向不存在资源的清单。

param(
    [string]$Version = "",
    [ValidateSet("windows", "macos")][string]$Platform = "windows",
    [string]$Channel = "stable",
    [string]$SetupPath = "",
    [string[]]$ExtraAssets = @(),
    [string]$BaseUrl = "https://dl.xiaotu.online/devcard/",
    # 下载页：三个产品共用同一个页面。清单里的 download_url 就是给人点的页面，
    # 不是安装包直链——早先这里写成 $Base+安装包名，「打开下载页」会直接把 exe 下载下来。
    [string]$DownloadPage = "https://dl.xiaotu.online/",
    [string]$Notes = "",
    [string]$OutFile = ""
)

$ErrorActionPreference = "Stop"

function Fail([string]$Message) {
    Write-Host ""
    Write-Host "[失败] $Message" -ForegroundColor Red
    exit 1
}

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$versionFile = Join-Path $repoRoot "VERSION.txt"
if (-not (Test-Path -LiteralPath $versionFile)) {
    Fail "找不到版本文件：$versionFile"
}
$fileVersion = (Get-Content -LiteralPath $versionFile -Raw).Trim()
if ([string]::IsNullOrWhiteSpace($fileVersion)) {
    Fail "VERSION.txt 为空"
}
if ([string]::IsNullOrWhiteSpace($Version)) {
    $Version = $fileVersion
} elseif ($Version -ne $fileVersion) {
    Fail "传入版本 $Version 与 VERSION.txt（$fileVersion）不一致；版本只认 VERSION.txt"
}

if ([string]::IsNullOrWhiteSpace($BaseUrl)) {
    Fail "-BaseUrl 不能为空"
}
$base = $BaseUrl.TrimEnd('/') + '/'
if ($base -notmatch '^https://') {
    Fail "-BaseUrl 必须是 https://（前端只接受 https 资源）"
}

$manifestName = if ($Platform -eq "macos") { "latest.json" } else { "windows.json" }

if ($Platform -eq "macos" -and -not [string]::IsNullOrWhiteSpace($SetupPath)) {
    Fail "-Platform macos 不接受 -SetupPath，请把 dmg 传给 -ExtraAssets"
}
if ($Platform -eq "windows") {
    if ([string]::IsNullOrWhiteSpace($SetupPath)) {
        $SetupPath = Join-Path $repoRoot "dist\DevCard-$Version-setup.exe"
    }
    if (-not (Test-Path -LiteralPath $SetupPath)) {
        Fail "找不到安装包：$SetupPath（先跑 Inno Setup 编译）"
    }
    $SetupPath = (Resolve-Path -LiteralPath $SetupPath).Path
    $setupName = Split-Path -Leaf $SetupPath
    $expectedName = "DevCard-$Version-setup.exe"
    if ($setupName -ne $expectedName) {
        Fail "安装包文件名是 $setupName，应为 $expectedName；文件名与版本不一致的包不能进清单"
    }
}

if ([string]::IsNullOrWhiteSpace($OutFile)) {
    $OutFile = Join-Path $repoRoot "dist\$manifestName"
}

# 单个资源：名称、下载地址、sha256 摘要、字节数。
# digest 必须带 sha256: 前缀——服务端与前端都按这个格式解析。
function New-Asset([string]$Path, [string]$Base) {
    $item = Get-Item -LiteralPath $Path
    if ($item.Length -le 0) {
        Fail "文件为空，不能进清单：$($item.FullName)"
    }
    if ($item.Name -match '\s') {
        Fail "文件名含空白，不适合直接拼进 URL：$($item.Name)"
    }
    $hash = (Get-FileHash -LiteralPath $item.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    return [ordered]@{
        name         = $item.Name
        download_url = $Base + $item.Name
        digest       = "sha256:$hash"
        size         = [int64]$item.Length
    }
}

$assets = @()
if ($Platform -eq "windows") {
    $assets += New-Asset $SetupPath $base
}

foreach ($extra in $ExtraAssets) {
    if ([string]::IsNullOrWhiteSpace($extra)) { continue }
    if (-not (Test-Path -LiteralPath $extra)) {
        Fail "找不到附加资源：$extra"
    }
    $full = (Resolve-Path -LiteralPath $extra).Path
    if ($SetupPath -and $full -eq $SetupPath) { continue }
    if ($assets | Where-Object { $_.name -eq (Split-Path -Leaf $full) }) {
        Fail "资源重名：$(Split-Path -Leaf $full)"
    }
    $assets += New-Asset $full $base
}

if ($assets.Count -eq 0) {
    Fail "清单至少需要一个产物：Windows 用 -SetupPath，macOS 用 -ExtraAssets"
}

if ([string]::IsNullOrWhiteSpace($Notes)) {
    $Notes = "DevCard $Version 版本发布。"
}

if ([string]::IsNullOrWhiteSpace($DownloadPage)) {
    Fail "-DownloadPage 不能为空"
}
$page = $DownloadPage.Trim()
if ($page -notmatch '^https://') {
    Fail "-DownloadPage 必须是 https://"
}
# 下载页必须指向目录/页面，而不是某个安装包文件；否则等于把「下载页」写成直链。
if ($page -match '\.(exe|zip|dmg)$') {
    Fail "-DownloadPage 不能指向具体文件：$page"
}

$manifest = [ordered]@{
    latest               = $Version
    download_url         = $page
    notes                = $Notes
    check_enabled        = $true
    check_endpoint       = $base + "updates/$Channel/$manifestName"
    check_interval_hours = 24
    assets               = $assets
}

$json = $manifest | ConvertTo-Json -Depth 6
$outDir = Split-Path -Parent $OutFile
if (-not [string]::IsNullOrWhiteSpace($outDir)) {
    New-Item -ItemType Directory -Force -Path $outDir | Out-Null
}
$utf8 = New-Object System.Text.UTF8Encoding($false)
[System.IO.File]::WriteAllText($OutFile, $json + "`n", $utf8)

# 写出去再读回来：字段缺失/类型不对在这里就暴露，不等到客户端解析失败。
# 显式 -Encoding UTF8：文件按无 BOM 的 UTF-8 写出，PowerShell 5 默认按 ANSI 读会截断引号。
$check = Get-Content -LiteralPath $OutFile -Raw -Encoding UTF8 | ConvertFrom-Json
if ($check.latest -ne $Version) { Fail "回读校验失败：latest=$($check.latest)" }
if ($check.assets.Count -ne $assets.Count) { Fail "回读校验失败：assets 数量不一致" }
foreach ($asset in $check.assets) {
    if ($asset.digest -notmatch '^sha256:[0-9a-f]{64}$') {
        Fail "回读校验失败：$($asset.name) 的 digest 格式不对"
    }
    # 客户端按「sha256 且 size>0」决定是否给出下载入口，size=0 的清单等于把按钮做灰。
    if ([int64]$asset.size -le 0) {
        Fail "回读校验失败：$($asset.name) 的 size 不是正数"
    }
    if ($asset.download_url -notmatch '^https://' -or $asset.download_url -match '/$') {
        Fail "回读校验失败：$($asset.name) 的 download_url 没有指向具体文件"
    }
    if (-not $asset.download_url.EndsWith('/' + $asset.name)) {
        Fail "回读校验失败：$($asset.name) 的 download_url 与文件名不一致"
    }
}
if ($check.download_url -ne $page) {
    Fail "回读校验失败：download_url 不是下载页（$page）"
}

Write-Host ""
Write-Host "更新清单已生成"
Write-Host "  文件：$OutFile"
Write-Host "  版本：$Version"
Write-Host "  资源：$($assets.Count) 个"
foreach ($asset in $assets) {
    Write-Host ("    {0}  {1} bytes  {2}" -f $asset.name, $asset.size, $asset.digest)
}
Write-Host "  检查地址：$($manifest.check_endpoint)"
Write-Host ""
Write-Host "提醒：上传安装包与 $manifestName 到 $base；"
Write-Host "      要让本机服务立即看到，还需把 update.json 复制到数据目录。"
exit 0
