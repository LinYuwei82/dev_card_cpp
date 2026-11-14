# 下载并校验 WebView2 SDK（M5 Windows 壳构建用）。
#
# 为什么不用「装了 VS 就有」：WebView2 SDK 是 NuGet 包，不作为 Windows SDK 的一部分，
# 也不入库（二进制大件 + Microsoft 许可）。这里按固定版本 + 固定 SHA256 取一次，
# 之后 configure 只读 third_party/webview2，不再联网。
#
# 用法：
#   powershell -ExecutionPolicy Bypass -File shell/win/fetch-webview2.ps1
#   powershell -ExecutionPolicy Bypass -File shell/win/fetch-webview2.ps1 -LocalPackage D:\offline\Microsoft.Web.WebView2.1.0.2903.40.nupkg
#
# 校验失败一律非零退出，不降级、不跳过。

param(
    [string]$Version = "1.0.2903.40",
    [string]$ExpectedSha256 = "EF128016DD1E51C59178C827ED5B8AA3322C57AFA8675D930F8109505542AD74",
    [string]$Destination = "",
    [string]$LocalPackage = ""
)

$ErrorActionPreference = "Stop"

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
if ([string]::IsNullOrWhiteSpace($Destination)) {
    $Destination = Join-Path $repoRoot "third_party\webview2"
}

$packageName = "Microsoft.Web.WebView2.$Version.nupkg"
$workDir = Join-Path ([System.IO.Path]::GetTempPath()) ("devcard-webview2-" + [Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $workDir | Out-Null

try {
    if (-not [string]::IsNullOrWhiteSpace($LocalPackage)) {
        $packagePath = (Resolve-Path $LocalPackage).Path
        Write-Host "离线包：$packagePath"
    } else {
        $packagePath = Join-Path $workDir $packageName
        $url = "https://www.nuget.org/api/v2/package/Microsoft.Web.WebView2/$Version"
        Write-Host "下载：$url"
        Invoke-WebRequest -Uri $url -OutFile $packagePath -UseBasicParsing
    }

    $actual = (Get-FileHash -Path $packagePath -Algorithm SHA256).Hash.ToUpperInvariant()
    if ($actual -ne $ExpectedSha256.ToUpperInvariant()) {
        throw "SHA256 不匹配：期望 $ExpectedSha256，实际 $actual。拒绝解压。"
    }
    Write-Host "SHA256 校验通过：$actual"

    $extractDir = Join-Path $workDir "sdk"
    # Expand-Archive 只认 .zip 后缀。
    $zipPath = Join-Path $workDir $packageName.Replace(".nupkg", ".zip")
    Copy-Item -Path $packagePath -Destination $zipPath
    Expand-Archive -Path $zipPath -DestinationPath $extractDir -Force

    $header = Join-Path $extractDir "build\native\include\WebView2.h"
    $staticLib = Join-Path $extractDir "build\native\x64\WebView2LoaderStatic.lib"
    if (-not (Test-Path $header) -or -not (Test-Path $staticLib)) {
        throw "包内缺少 build/native/include/WebView2.h 或 build/native/x64/WebView2LoaderStatic.lib。"
    }

    if (Test-Path $Destination) {
        Remove-Item -Path $Destination -Recurse -Force
    }
    New-Item -ItemType Directory -Path $Destination | Out-Null
    Copy-Item -Path (Join-Path $extractDir "build") -Destination $Destination -Recurse -Force

    $versionFile = Join-Path $Destination "VERSION.txt"
    "Microsoft.Web.WebView2 $Version`nSHA256 $ExpectedSha256" | Set-Content -Path $versionFile -Encoding ASCII

    Write-Host "WebView2 SDK $Version 已就位：$Destination"
    exit 0
}
catch {
    Write-Error $_
    exit 1
}
finally {
    Remove-Item -Path $workDir -Recurse -Force -ErrorAction SilentlyContinue
}
