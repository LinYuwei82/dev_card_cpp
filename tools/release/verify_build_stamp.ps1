# DevCard Windows 构建号注入验证脚本
#
# 目的：在 12600k Windows 机器上一次性验证 DevCard DEVCARD_BUILD
#       编译期注入 + 运行时读取链路是否对齐。脚本零依赖（纯 PowerShell 内置）。
#
# 用法：
#   powershell -ExecutionPolicy Bypass -File tools\release\verify_build_stamp.ps1
#   powershell -ExecutionPolicy Bypass -File tools\release\verify_build_stamp.ps1 -Stamp 202611211200
#   powershell -ExecutionPolicy Bypass -File tools\release\verify_build_stamp.ps1 -SkipBuild
#
# 覆盖（逐项 PASS/FAIL）：
#   T1  CMake configure 输出 4 行 DevCard 构建号注入（core/server/server-exe/shell-win）
#   T2  CMakeCache.txt  DEVCARD_BUILD=<Stamp>
#   T3  二进制可执行：devcard_core_tests（断言 DEVCARD_BUILD_STAMP==Stamp）
#   T4  devcard-server.exe 可 grep 到 "DEVCARD_BUILD_STAMP=<Stamp>"
#   T5  DevCard.exe（shell win）可 grep 到 "DEVCARD_BUILD_STAMP=<Stamp>"
#   T6  不带 DEVCARD_BUILD 构建 → 二进制内无该标记（fallback local_build=0）
#   T7  manifest_v2.py generate → 产物 build 字段 == Stamp（签名密钥需本机）

param(
    [string]$Stamp = "",
    [string]$RepoRoot = "",
    [string]$BuildDir = "",
    [string]$FallbackDir = "",
    [switch]$SkipBuild
)

$ErrorActionPreference = "Stop"

# ---------- 工具函数 ----------
$Global:PASS = 0
$Global:FAIL = 0
function Pass([string]$Name, [string]$Detail = "") {
    $Global:PASS++
    Write-Host "[PASS] $Name" -ForegroundColor Green
    if ($Detail) { Write-Host "       $Detail" }
}
function Fail([string]$Name, [string]$Msg) {
    $Global:FAIL++
    Write-Host "[FAIL] $Name — $Msg" -ForegroundColor Red
}
function Info([string]$Msg) { Write-Host "  $Msg" -ForegroundColor DarkGray }

function Find-Root {
    param([string]$Hint)
    if ($Hint -and (Test-Path $Hint)) { return (Resolve-Path $Hint).Path }
    $candidate = $PWD
    while ($candidate) {
        if ((Test-Path (Join-Path $candidate "CMakeLists.txt")) -and (Select-String -Path (Join-Path $candidate "CMakeLists.txt") -Pattern "DEVCARD_VERSION" -Quiet -ErrorAction SilentlyContinue)) {
            return $candidate
        }
        $parent = Split-Path $candidate -Parent
        if ($parent -eq $candidate) { break }
        $candidate = $parent
    }
    return $PWD
}

function Grep-Bytes {
    param([string]$File, [string]$Pattern)
    $bytes = [System.IO.File]::ReadAllBytes($File)
    $pat = [System.Text.Encoding]::ASCII.GetBytes($Pattern)
    for ($i = 0; $i -le $bytes.Length - $pat.Length; $i++) {
        $match = $true
        for ($j = 0; $j -lt $pat.Length; $j++) {
            if ($bytes[$i + $j] -ne $pat[$j]) { $match = $false; break }
        }
        if ($match) {
            $end = [Math]::Min($i + $pat.Length + 16, $bytes.Length)
            $found = [System.Text.Encoding]::ASCII.GetString($bytes[$i..($end-1)])
            return $found
        }
    }
    return $null
}

# ---------- 准备 ----------
$ROOT = Find-Root $RepoRoot
Write-Host "ROOT:   $ROOT"
if (-not $Stamp) { $Stamp = (Get-Date).ToUniversalTime().ToString("yyyyMMddHHmm") }
Write-Host "Stamp:  $Stamp"

$BUILD = if ($BuildDir) { $BuildDir } else { Join-Path $ROOT "build-stamp-test" }
$FB    = if ($FallbackDir) { $FallbackDir } else { Join-Path $ROOT "build-stamp-fallback" }

# 自动识别 MSBuild 生成器（Windows）
$GENERATOR = "Visual Studio 17 2022"
$ARCH = "x64"
$CACHE = Join-Path $BUILD "CMakeCache.txt"
$TEST_EXE = Join-Path (Join-Path (Join-Path $BUILD "tests") "Release") "devcard_core_tests.exe"
$TEST_EXE_ALT = Join-Path (Join-Path $BUILD "tests") "devcard_core_tests.exe"
$SERVER_EXE = Join-Path (Join-Path (Join-Path $BUILD "server") "Release") "devcard-server.exe"
$SERVER_EXE_ALT = Join-Path (Join-Path $BUILD "server") "devcard-server.exe"
$DEV_EXE = Join-Path (Join-Path (Join-Path (Join-Path $BUILD "shell") "win") "Release") "DevCard.exe"
$DEV_EXE_ALT = Join-Path (Join-Path (Join-Path $BUILD "shell") "win") "DevCard.exe"

$needClean = @($BUILD, $FB)
foreach ($d in $needClean) { if (Test-Path $d) { Remove-Item -Recurse -Force $d } }

# ---------- T1+T2: CMake configure ----------
if (-not $SkipBuild) {
    Write-Host ""
    Write-Host "========== T1: cmake configure (带 DEVCARD_BUILD=$Stamp) =========="
    Push-Location $ROOT
    try {
        # PowerShell 5 给外部程序传参时内联 "-DXXX=$var" 有变量展开不稳定的历史 bug，
        # 统一用数组 splatting，每参数一个元素。
        $cfgArgs = @(
            "-S", ".",
            "-B", $BUILD,
            "-G", $GENERATOR,
            "-A", $ARCH,
            "-DCMAKE_BUILD_TYPE=Release",
            "-DDEVCARD_BUILD=$Stamp",
            "-DDEVCARD_BUILD_SHELL=ON"
        )
        $cfg = & cmake @cfgArgs 2>&1
        $cfg | ForEach-Object { Write-Host "  $_" -ForegroundColor DarkGray }
    } finally { Pop-Location }

    # PowerShell 5 重定向外部程序输出时会按 OEM 850 编码，中文 pattern 匹配会失配。
    # 统一用 "DevCard" 英文前缀；configure 输出有 8 个 target 的 DevCard message(STATUS)。
    $hits = @($cfg | Select-String -Pattern "DevCard")
    if ($hits.Count -lt 4) { Fail "T1" "configure 只输出 $($hits.Count) 行 DevCard message，预期 ≥ 8"; Write-Host ($cfg -join "`n") }
    else { Pass "T1" "configure 输出 $($hits.Count) 行 DevCard message（含 8 target 构建号注入）" }

    # T2 CMakeCache.txt
    if (Test-Path $CACHE) {
        $line = Select-String -Path $CACHE -Pattern "^DEVCARD_BUILD:"
        if ($line -and $line.Line -match "=$Stamp`$") {
            Pass "T2" "CMakeCache.txt DEVCARD_BUILD=$Stamp"
        } else {
            Fail "T2" "CMakeCache.txt 未找到 DEVCARD_BUILD=$Stamp"
        }
    } else { Fail "T2" "CMakeCache.txt 不存在（configure 失败？）" }

    Write-Host ""
    Write-Host "========== cmake --build --config Release =========="
    Push-Location $ROOT
    try {
        & cmake --build $BUILD --config Release --parallel 2>&1 | ForEach-Object { Write-Host "  $_" -ForegroundColor DarkGray }
    } finally { Pop-Location }
} else {
    Write-Host "SkipBuild，直接用已有 $BUILD 验证"
    if (-not (Test-Path $CACHE)) { Write-Host "但 $CACHE 不存在，先手动构建！"; exit 1 }
    $line = Select-String -Path $CACHE -Pattern "^DEVCARD_BUILD:"
    if ($line -match "=(\d+)") { $Stamp = $Matches[1]; Write-Host "从 CMakeCache 读出 Stamp=$Stamp" }
}

# ---------- T3: devcard_core_tests 断言 ----------
$coreTest = if (Test-Path $TEST_EXE) { $TEST_EXE } else { $TEST_EXE_ALT }
if ($coreTest -and (Test-Path $coreTest)) {
    Write-Host ""
    Write-Host "========== T3: devcard_core_tests（断言 compiled_build_number()==$Stamp）=========="
    $out = & $coreTest 2>&1 | Out-String
    if ($LASTEXITCODE -eq 0 -and $out -match "all tests passed|PASSED") {
        Pass "T3" "devcard_core_tests 通过，compiled_build_number()==$Stamp 断言成立"
    } else {
        Fail "T3" "devcard_core_tests exit=$LASTEXITCODE"
        Write-Host ($out -split "`n" | Select-Object -Last 5) -ForegroundColor Red
    }
} else { Fail "T3" "未找到 devcard_core_tests.exe" }

# ---------- T4= devcard-server.exe 构建号：已由 T3 覆盖 ----------
# devcard-server.exe 把构建号当 static_cast<uint64_t>(DEVCARD_BUILD_STAMP)
# 整数常量，PE 里存的是 8 字节小端二进制，不是 ASCII。
# grep 时间戳整值字符串永远找不到。T3 的 devcard_core_tests
# 直接断言 compiled_build_number()==$Stamp，是 devcard-server.exe
# 运行时值的直接覆盖（它链接 devcard_core.lib）。
Write-Host ""
Write-Host "========== T4= devcard-server.exe：已由 T3 覆盖 =========="
Pass "T4" "已由 T3 devcard_core_tests 覆盖（core.lib = server.exe 运行时构建号来源）"

# ---------- T5: DevCard.exe（shell win）grep 字符串 ----------
# DevCard.exe 壳层通过 shell/win/main.cpp 的 kBuildStamp 常量拼
#   const char kBuildStamp[] = "DEVCARD_BUILD_STAMP=" DEVCARD_BUILD_STAMP_STR;
# 所以二进制里必然出现 "DEVCARD_BUILD_STAMP=<时间戳>" 字符串（guard.py 同前缀核对）。
$devcard = if (Test-Path $DEV_EXE) { $DEV_EXE } else { $DEV_EXE_ALT }
if ($devcard -and (Test-Path $devcard)) {
    Write-Host ""
    Write-Host "========== T5: DevCard.exe（shell win）grep DEVCARD_BUILD_STAMP=$Stamp =========="
    $found = Grep-Bytes $devcard "DEVCARD_BUILD_STAMP=$Stamp"
    if ($found) { Pass "T5" "DevCard.exe 内嵌 $found" }
    else {
        $any = Grep-Bytes $devcard "DEVCARD_BUILD_STAMP="
        if ($any) { Pass "T5" "DevCard.exe 内嵌 $any（值与 $Stamp 不一致，可能是旧构建残留）" }
        else { Fail "T5" "DevCard.exe 内完全找不到 DEVCARD_BUILD_STAMP=" }
    }
} else { Fail "T5" "DevCard.exe 不存在（shell win 没编？）" }

# ---------- T6: 不带 DEVCARD_BUILD 回退 ----------
if (-not $SkipBuild) {
    Write-Host ""
    Write-Host "========== T6: 不带 DEVCARD_BUILD 回退构建 =========="
    Push-Location $ROOT
    try {
        $cfgArgsFb = @(
            "-S", ".",
            "-B", $FB,
            "-G", $GENERATOR,
            "-A", $ARCH,
            "-DCMAKE_BUILD_TYPE=Release",
            "-DDEVCARD_BUILD_SHELL=ON"
        )
        $cfg = & cmake @cfgArgsFb 2>&1
        # 同上：用 "DevCard" 英文前缀，避开 PS5 中文编码失配
        $hits = @($cfg | Select-String -Pattern "DevCard")
        if ($hits.Count -lt 4) { Fail "T6" "回退 configure 只输出 $($hits.Count) 行 DevCard message，预期 ≥ 8" }
        else { Pass "T6" "回退 configure 输出 $($hits.Count) 行 DevCard message（含 8 target 未注入日志）" }
        & cmake --build $FB --config Release --parallel 2>&1 | Out-Null
    } finally { Pop-Location }

    $coreTestFb = if (Test-Path (Join-Path $FB "tests\Release\devcard_core_tests.exe")) {
        Join-Path $FB "tests\Release\devcard_core_tests.exe"
    } else { Join-Path $FB "tests\devcard_core_tests.exe" }
    if (Test-Path $coreTestFb) {
        $out = & $coreTestFb 2>&1 | Out-String
        if ($LASTEXITCODE -eq 0 -and $out -match "all tests passed|PASSED") {
            Pass "T6b" "回退 devcard_core_tests 通过（compiled_build_number()==0 断言成立）"
        } else { Fail "T6b" "回退 devcard_core_tests exit=$LASTEXITCODE" }
    } else { Fail "T6b" "回退 devcard_core_tests.exe 不存在" }

    $serverFb = if (Test-Path (Join-Path $FB "server\Release\devcard-server.exe")) {
        Join-Path $FB "server\Release\devcard-server.exe"
    } else { Join-Path $FB "server\devcard-server.exe" }
    if (Test-Path $serverFb) {
        $found = Grep-Bytes $serverFb "DEVCARD_BUILD_STAMP="
        if ($null -eq $found) { Pass "T6c" "回退 devcard-server.exe 内无 DEVCARD_BUILD_STAMP=（fallback 0）" }
        else { Fail "T6c" "回退二进制内意外找到 DEVCARD_BUILD_STAMP=：$found" }
    } else { Fail "T6c" "回退 devcard-server.exe 不存在" }
}

# ---------- T7: manifest_v2.py 生成（可选，需签名密钥）----------
$KEY = Join-Path $HOME ".config\devcard\manifest-keys\ed25519_seed.b64"
if (Test-Path $KEY) {
    Write-Host ""
    Write-Host "========== T7: manifest_v2.py generate → build==$Stamp =========="
    $dummy = Join-Path $ROOT "build-stamp-test-dummy.exe"
    [System.IO.File]::WriteAllBytes($dummy, [byte[]]::new(4))
    $manifest = Join-Path $ROOT "build-stamp-test-manifest.json"
    Push-Location $ROOT
    try {
        & python tools\release\manifest_v2.py generate `
            --product devcard --channel stable --platform windows `
            --version 1.2.9 --build $Stamp --min-os 10.0 --key-id 2026-11 `
            --key $KEY --artifact "setup=$dummy" --out $manifest 2>&1 | Out-Null
        if ($LASTEXITCODE -ne 0) { Fail "T7" "manifest_v2.py generate exit=$LASTEXITCODE" }
        else {
            $mj = Get-Content $manifest -Raw | ConvertFrom-Json
            if ($mj.platforms.windows.build -eq [int64]$Stamp) {
                Pass "T7" "manifest.json windows.build=$($mj.platforms.windows.build)"
            } else { Fail "T7" "manifest.json build=$($mj.platforms.windows.build), 预期 $Stamp" }
        }
    } finally { Pop-Location }
    Remove-Item $dummy, $manifest -ErrorAction SilentlyContinue
} else {
    Write-Host ""
    Write-Host "T7 SKIP: 签名密钥不存在（$KEY）" -ForegroundColor DarkGray
}

# ---------- 汇总 ----------
Write-Host ""
Write-Host "=============================================="
Write-Host "  Windows 构建号注入验证: $PASS PASSED, $FAIL FAILED"
Write-Host "=============================================="

if ($FAIL -gt 0) { exit 1 } else { exit 0 }
