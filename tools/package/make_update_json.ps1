# DevCard 更新清单生成：按线上 update.json 的格式输出，避免手写漏字段。
#
# 用法（仓库根目录或任意目录均可）：
#   powershell -ExecutionPolicy Bypass -File tools\package\make_update_json.ps1
#   ... -SetupPath dist\DevCard-1.2.0-setup.exe -Notes "本次更新内容"
#   ... -ExtraAssets dist\DevCard-1.2.0-macos-arm64.dmg
#   ... -BaseUrl https://dl.xiaotu.online/devcard/
#
# 默认：版本取 VERSION.txt，安装包取 dist\DevCard-<版本>-setup.exe，
# 输出 dist\update.json。清单只描述已经存在的文件：缺文件、空文件、
# 文件名与版本对不上都直接失败，不产出一个指向不存在资源的清单。

param(
    [string]$Version = "",
    [string]$SetupPath = "",
    [string[]]$ExtraAssets = @(),
    [string]$BaseUrl = "https://dl.xiaotu.online/devcard/",
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

if ([string]::IsNullOrWhiteSpace($OutFile)) {
    $OutFile = Join-Path $repoRoot "dist\update.json"
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
$assets += New-Asset $SetupPath $base

foreach ($extra in $ExtraAssets) {
    if ([string]::IsNullOrWhiteSpace($extra)) { continue }
    if (-not (Test-Path -LiteralPath $extra)) {
        Fail "找不到附加资源：$extra"
    }
    $full = (Resolve-Path -LiteralPath $extra).Path
    if ($full -eq $SetupPath) { continue }
    if ($assets | Where-Object { $_.name -eq (Split-Path -Leaf $full) }) {
        Fail "资源重名：$(Split-Path -Leaf $full)"
    }
    $assets += New-Asset $full $base
}

if ([string]::IsNullOrWhiteSpace($Notes)) {
    $Notes = "DevCard $Version 版本发布。"
}

$manifest = [ordered]@{
    latest               = $Version
    download_url         = $base + $setupName
    notes                = $Notes
    check_enabled        = $true
    check_endpoint       = $base + "update.json"
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
Write-Host "提醒：上传安装包与 update.json 到 $base；"
Write-Host "      要让本机服务立即看到，还需把 update.json 复制到数据目录。"
exit 0
