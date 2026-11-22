# DevCard WebView2 clean-VM verification
# 在"干净"Windows 10/11 虚拟机上验证安装器的 WebView2 可选任务：
#   1) 机器确实未安装 WebView2 Runtime（否则退出码 2，请换干净快照）
#   2) 静默安装并勾选 webview2 任务后，Runtime 被装上、版本可读
#   3) 安装目录文件齐全
#   4) DevCard.exe 能启动并保持存活（窗口标题可作为证据）
# 用法示例：
#   powershell -ExecutionPolicy Bypass -File .\test_webview2_clean_vm.ps1 `
#       -InstallerPath D:\DevCard-1.3.3-setup.exe
#   powershell -ExecutionPolicy Bypass -File .\test_webview2_clean_vm.ps1 `
#       -InstallerPath D:\DevCard-1.3.3-setup.exe -Mode Interactive
#   powershell -ExecutionPolicy Bypass -File .\test_webview2_clean_vm.ps1 `
#       -InstallerPath D:\DevCard-1.3.3-setup.exe -CheckOnly
#   powershell -ExecutionPolicy Bypass -File .\test_webview2_clean_vm.ps1 `
#       -InstallerPath D:\DevCard-1.3.3-setup.exe -SkipCleanCheck -UninstallAfter
# 退出码：0 全部通过；1 有检查失败；2 机器不干净/前置条件不满足。
# 说明：SSH/Session 0 等非交互会话没有可见桌面，会自动跳过窗口标题与截图校验，
#       仅保留进程存活检查，不计入 WARN，也不影响退出码。

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$InstallerPath,

    [string]$InstallDir = "$env:LOCALAPPDATA\Programs\DevCardTest",

    [ValidateSet("Silent", "Interactive")]
    [string]$Mode = "Silent",

    [int]$BootstrapperTimeoutSec = 180,

    [switch]$CheckOnly,

    # 已有 WebView2 的机器上只做流程冒烟（不验证“从无到有”），便于同一快照重复跑
    [switch]$SkipCleanCheck,

    [switch]$UninstallAfter,

    [switch]$NoScreenshot,

    [string]$LogDir = "$env:TEMP\DevCardWebView2Test"
)

$ErrorActionPreference = "Stop"
$failures = New-Object System.Collections.Generic.List[string]
$warnings = New-Object System.Collections.Generic.List[string]

function Write-Step([string]$text) {
    Write-Host ""
    Write-Host "==== $text ====" -ForegroundColor Cyan
}

function Add-Failure([string]$text) {
    $failures.Add($text) | Out-Null
    Write-Host "FAIL: $text" -ForegroundColor Red
}

function Add-Warning([string]$text) {
    $warnings.Add($text) | Out-Null
    Write-Host "WARN: $text" -ForegroundColor Yellow
}

function Get-WebView2Version {
    $keys = @(
        "HKLM:\SOFTWARE\WOW6432Node\Microsoft\EdgeUpdate\Clients\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}",
        "HKLM:\SOFTWARE\Microsoft\EdgeUpdate\Clients\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}",
        "HKCU:\Software\Microsoft\EdgeUpdate\Clients\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}"
    )
    foreach ($key in $keys) {
        $value = (Get-ItemProperty -Path $key -Name pv -ErrorAction SilentlyContinue).pv
        if (-not [string]::IsNullOrWhiteSpace([string]$value)) {
            return [string]$value
        }
    }
    return $null
}

function Save-Screenshot([string]$path) {
    if ($NoScreenshot) { return }
    if ($nonInteractive) {
        Write-Host "当前为非交互会话（Session 0），跳过截图。"
        return
    }
    try {
        Add-Type -AssemblyName System.Windows.Forms
        Add-Type -AssemblyName System.Drawing
        $bounds = [System.Windows.Forms.SystemInformation]::VirtualScreen
        $bitmap = New-Object System.Drawing.Bitmap $bounds.Width, $bounds.Height
        $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
        $graphics.CopyFromScreen($bounds.Location, [System.Drawing.Point]::Empty, $bounds.Size)
        $bitmap.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
        $graphics.Dispose()
        $bitmap.Dispose()
        Write-Host "截图已保存：$path"
    } catch {
        Add-Warning "截图失败（可能没有交互桌面）：$($_.Exception.Message)"
    }
}

function Get-DevCardProcessIds {
    return @(Get-Process -Name "DevCard" -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Id)
}

New-Item -ItemType Directory -Path $LogDir -Force | Out-Null
$stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$transcript = Join-Path $LogDir "test-$stamp.log"
$installerLog = Join-Path $LogDir "installer-$stamp.log"
Start-Transcript -Path $transcript -Force | Out-Null

$exitCode = 1
try {
    Write-Step "环境信息"
    $os = Get-CimInstance Win32_OperatingSystem
    Write-Host "OS        : $($os.Caption) $($os.Version) ($($os.OSArchitecture))"
    Write-Host "主机名    : $env:COMPUTERNAME"
    $principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
    $isAdmin = $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
    Write-Host "管理员    : $isAdmin"
    $nonInteractive = (Get-Process -Id $PID).SessionId -eq 0
    if ($nonInteractive) {
        Write-Host "交互桌面  : 无（Session 0，跳过窗口标题与截图校验）"
    } else {
        Write-Host "交互桌面  : 有"
    }
    Write-Host "日志      : $transcript"

    Write-Step "检查前置条件"
    if (-not (Test-Path -LiteralPath $InstallerPath)) {
        Write-Host "找不到安装包：$InstallerPath" -ForegroundColor Red
        $exitCode = 2
        exit $exitCode
    }
    $installer = Get-Item -LiteralPath $InstallerPath
    Write-Host "安装包    : $($installer.FullName)"
    Write-Host "版本      : $($installer.VersionInfo.ProductVersion)"
    Write-Host "SHA256    : $((Get-FileHash -LiteralPath $installer.FullName -Algorithm SHA256).Hash)"

    $webviewVersion = Get-WebView2Version
    if ($webviewVersion) {
        if ($SkipCleanCheck) {
            Add-Warning "机器已安装 WebView2 $webviewVersion，跳过'从无到有'断言（-SkipCleanCheck）"
        } else {
            Write-Host "WebView2  : 已安装 $webviewVersion" -ForegroundColor Yellow
            Write-Host "这台机器不满足'干净'前提，请使用未装 WebView2 Runtime 的虚拟机快照。" -ForegroundColor Yellow
            $exitCode = 2
            exit $exitCode
        }
    } else {
        Write-Host "WebView2  : 未安装（符合干净前提）" -ForegroundColor Green
    }

    if ($CheckOnly) {
        Write-Host ""
        Write-Host "CheckOnly 完成：机器干净，可以继续跑完整测试。" -ForegroundColor Green
        $exitCode = 0
        exit $exitCode
    }

    if (Test-Path -LiteralPath $InstallDir) {
        Add-Warning "安装目录已存在，将直接覆盖安装：$InstallDir"
    }
    $running = Get-DevCardProcessIds
    if ($running.Count -gt 0) {
        Add-Warning "检测到运行中的 DevCard（PID: $($running -join ', ')），先结束再测试。"
        $running | ForEach-Object { Stop-Process -Id $_ -Force -ErrorAction SilentlyContinue }
        Start-Sleep -Seconds 2
    }

    Write-Step "运行安装器（$Mode）"
    $installerArgs = @()
    if ($Mode -eq "Silent") {
        $installerArgs = @(
            "/VERYSILENT", "/SUPPRESSMSGBOXES", "/NORESTART", "/CLOSEAPPLICATIONS",
            "/MERGETASKS=webview2",
            "/DIR=`"$InstallDir`"",
            "/LOG=`"$installerLog`""
        )
    } else {
        Write-Host "请在弹出的向导中确认：" -ForegroundColor Yellow
        Write-Host "  1) 出现「运行环境：下载并安装 Microsoft Edge WebView2 Runtime」任务" -ForegroundColor Yellow
        Write-Host "  2) 该任务默认勾选（若未勾选请手动勾上）" -ForegroundColor Yellow
        Write-Host "  3) 记住安装目录，安装完成后脚本会自动检查" -ForegroundColor Yellow
        $installerArgs = @("/DIR=`"$InstallDir`"")
    }

    $installStart = Get-Date
    $installProcess = Start-Process -FilePath $installer.FullName -ArgumentList $installerArgs -PassThru -Wait
    $installSeconds = [int]((Get-Date) - $installStart).TotalSeconds
    Write-Host "安装器退出码：$($installProcess.ExitCode)，耗时 ${installSeconds}s，日志：$installerLog"
    if ($installProcess.ExitCode -ne 0) {
        Add-Failure "安装器退出码非 0：$($installProcess.ExitCode)"
        if (Test-Path -LiteralPath $installerLog) {
            Write-Host "---- 安装日志末尾 ----"
            Get-Content -LiteralPath $installerLog -Tail 20 | ForEach-Object { Write-Host $_ }
        }
    }

    Write-Step "等待 WebView2 Runtime 注册"
    $deadline = (Get-Date).AddSeconds($BootstrapperTimeoutSec)
    $webviewVersion = $null
    while ((Get-Date) -lt $deadline) {
        $webviewVersion = Get-WebView2Version
        if ($webviewVersion) { break }
        Start-Sleep -Seconds 5
    }
    if ($webviewVersion) {
        Write-Host "WebView2  : 安装成功 $webviewVersion" -ForegroundColor Green
        $webviewProcess = Get-Process -Name "msedgewebview2" -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($webviewProcess) {
            Write-Host "WebView2 进程已在运行：$($webviewProcess.ProcessName) (PID $($webviewProcess.Id))"
        }
    } else {
        Add-Failure "等待 $BootstrapperTimeoutSec 秒后仍未检测到 WebView2 Runtime"
        Write-Host "可能原因：无外网、代理拦截 fwlink、安装任务未勾选、或 Bootstrapper 失败。" -ForegroundColor Yellow
    }

    Write-Step "检查安装目录"
    $required = @("DevCard.exe", "devcard-server.exe", "unins000.exe")
    foreach ($name in $required) {
        $path = Join-Path $InstallDir $name
        if (Test-Path -LiteralPath $path) {
            Write-Host "存在：$path"
        } else {
            Add-Failure "缺少文件：$path"
        }
    }
    $fontDir = Join-Path $InstallDir "fonts"
    if (Test-Path -LiteralPath $fontDir) {
        $fontCount = @(Get-ChildItem -LiteralPath $fontDir -Filter *.ttf -ErrorAction SilentlyContinue).Count
        Write-Host "字体文件数：$fontCount"
        if ($fontCount -lt 2) { Add-Failure "字体文件不足（$fontCount）" }
    } else {
        Add-Failure "缺少 fonts 目录"
    }

    Write-Step "启动 DevCard.exe"
    $devCardExe = Join-Path $InstallDir "DevCard.exe"
    if (Test-Path -LiteralPath $devCardExe) {
        # 交互模式下安装器结尾会自动启动 DevCard（postinstall），优先复用该进程
        $process = $null
        for ($i = 0; $i -lt 5; $i++) {
            $process = Get-Process -Name "DevCard" -ErrorAction SilentlyContinue | Select-Object -First 1
            if ($process) { break }
            Start-Sleep -Seconds 2
        }
        if ($process) {
            Write-Host "复用安装器自动启动的 DevCard 进程，PID $($process.Id)"
        } else {
            $process = Start-Process -FilePath $devCardExe -PassThru
        }
        Start-Sleep -Seconds 8
        $process.Refresh()
        if ($process.HasExited) {
            Add-Failure "DevCard.exe 启动后已退出，退出码 $($process.ExitCode)"
        } else {
            Write-Host "DevCard.exe 存活，PID $($process.Id)，窗口标题：'$($process.MainWindowTitle)'"
            if ([string]::IsNullOrWhiteSpace($process.MainWindowTitle)) {
                if ($nonInteractive) {
                    Write-Host "非交互会话（Session 0），窗口标题不可用；进程存活即视为通过。"
                } else {
                    Add-Warning "窗口标题为空（可能无交互桌面或页面仍在加载）"
                }
            }
            Save-Screenshot (Join-Path $LogDir "devcard-$stamp.png")
        }
    } else {
        Add-Failure "找不到 DevCard.exe，跳过启动测试"
    }

    Get-Process -Name "DevCard", "devcard-server" -ErrorAction SilentlyContinue | ForEach-Object {
        Stop-Process -Id $_.Id -Force -ErrorAction SilentlyContinue
    }
    Start-Sleep -Seconds 2

    if ($UninstallAfter) {
        Write-Step "卸载 DevCard"
        $uninstaller = Join-Path $InstallDir "unins000.exe"
        if (Test-Path -LiteralPath $uninstaller) {
            # 不用 -Wait：Inno 卸载器会派生 _unins.tmp，无交互桌面时可能长期不退出
            Start-Process -FilePath $uninstaller `
                -ArgumentList @("/VERYSILENT", "/SUPPRESSMSGBOXES", "/NORESTART") | Out-Null
            $uninstallDeadline = (Get-Date).AddSeconds(120)
            while ((Test-Path -LiteralPath $devCardExe) -and (Get-Date) -lt $uninstallDeadline) {
                Start-Sleep -Seconds 2
            }
            if (Test-Path -LiteralPath $devCardExe) {
                Add-Failure "卸载 120 秒后 DevCard.exe 仍存在：$devCardExe"
            } else {
                Write-Host "DevCard.exe 已删除"
            }
            $graceDeadline = (Get-Date).AddSeconds(30)
            while ((Get-Process -Name "_unins*" -ErrorAction SilentlyContinue) -and (Get-Date) -lt $graceDeadline) {
                Start-Sleep -Seconds 2
            }
            Get-Process -Name "_unins*" -ErrorAction SilentlyContinue | ForEach-Object {
                if ($nonInteractive) {
                    Write-Host "非交互会话（Session 0），已结束卸载器残留进程：$($_.ProcessName) (PID $($_.Id))"
                } else {
                    Add-Warning "卸载器残留进程未退出，已强制结束：$($_.ProcessName) (PID $($_.Id))"
                }
                Stop-Process -Id $_.Id -Force -ErrorAction SilentlyContinue
            }
        } else {
            Add-Failure "找不到卸载器：$uninstaller"
        }
    }

    Write-Step "结果汇总"
    if ($failures.Count -eq 0) {
        Write-Host "PASS：WebView2 任务安装、文件、启动检查全部通过。" -ForegroundColor Green
        $exitCode = 0
    } else {
        Write-Host "FAIL：$($failures.Count) 项失败" -ForegroundColor Red
        $failures | ForEach-Object { Write-Host "  - $_" -ForegroundColor Red }
        $exitCode = 1
    }
    if ($warnings.Count -gt 0) {
        Write-Host "WARN：$($warnings.Count) 项" -ForegroundColor Yellow
        $warnings | ForEach-Object { Write-Host "  - $_" -ForegroundColor Yellow }
    }
    Write-Host "完整日志：$transcript"
} catch {
    Write-Host "脚本异常：$($_.Exception.Message)" -ForegroundColor Red
    Write-Host $_.ScriptStackTrace -ForegroundColor Red
    $exitCode = 1
} finally {
    Stop-Transcript | Out-Null
}

exit $exitCode
