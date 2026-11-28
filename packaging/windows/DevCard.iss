; DevCard Windows 安装脚本（Inno Setup 6.x）
;
; 编译（Windows，需先构建好两个 exe 与字体）：
;   "C:\Program Files (x86)\Inno Setup 6\ISCC.exe" ^
;     /DSourceRoot=..\..\dist\DevCard-<版本>-win64-portable ^
;     packaging\windows\DevCard.iss
;
; 版本号只有一个真值：仓库根目录 VERSION.txt。这里不再写第二处字面量，
; 也不再接受外部 /DAppVersion 覆盖（覆盖等于又造一处真值）。
;
; 安装位置与数据位置刻意分开：
;   程序: %LOCALAPPDATA%\Programs\DevCard
;   数据: %LOCALAPPDATA%\DevCard\data（由 devcard-server.exe 首次启动创建）
; 卸载只删程序目录，数据目录保留并在结束时提示路径。

#define VersionFile FileOpen(AddBackslash(SourcePath) + "..\..\VERSION.txt")
#if !FileExists(AddBackslash(SourcePath) + "..\..\VERSION.txt")
  #error 找不到 VERSION.txt（安装包版本必须从它取，不许写第二处真值）
#endif
#define AppVersion Trim(FileRead(VersionFile))
#expr FileClose(VersionFile)
#if Len(AppVersion) == 0
  #error VERSION.txt 为空
#endif
#ifndef SourceRoot
  ; 默认取 package_windows.ps1 的打包目录命名；官方发布路径由
  ; publish_windows.ps1 显式传 -DSourceRoot，这里只服务于手工编译。
  #define SourceRoot "..\..\dist\DevCard-" + AppVersion + "-win64-portable"
#endif

[Setup]
AppId={{6F1E7A52-3C4B-4E8D-9A21-5B7C0D3E8F14}
AppName=DevCard
AppVersion={#AppVersion}
AppPublisher=深圳市安业物业管理有限公司
DefaultDirName={localappdata}\Programs\DevCard
DefaultGroupName=DevCard
DisableProgramGroupPage=yes
; 每用户安装：不弹 UAC、不需要管理员、不注册系统服务。
PrivilegesRequired=lowest
OutputDir=..\..\dist
OutputBaseFilename=DevCard-{#AppVersion}-setup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
ArchitecturesInstallIn64BitMode=x64compatible
UninstallDisplayIcon={app}\DevCard.exe
; 安装器图标与 exe 内嵌图标同源：tools/package/make_icons.sh 生成 shell/win/DevCard.ico。
SetupIconFile=..\..\shell\win\DevCard.ico

[Languages]
Name: "en"; MessagesFile: "compiler:Default.isl"

[Tasks]
; 与 Mote / 小兔库存的安装器保持一致：桌面快捷方式默认勾选，
; checkedonce 让升级安装沿用用户上次的选择，不反复把勾去掉/加上。
Name: "desktopicon"; Description: "创建桌面快捷方式"; GroupDescription: "附加任务："; Flags: checkedonce
; WebView2 Runtime 不是随包 DLL，而是系统共享组件；缺失时 DevCard 无法启动。
; 只在确实未检测到运行库时显示（Check），默认勾选；已安装的机器不会重复下载。
Name: "webview2"; Description: "下载并安装 Microsoft Edge WebView2 Runtime（需要联网）"; GroupDescription: "运行环境："; Check: WebView2Missing

[Files]
Source: "{#SourceRoot}\DevCard.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceRoot}\devcard-server.exe"; DestDir: "{app}"; Flags: ignoreversion
; 字体缺失时编译直接失败（不加 skipifsourcedoesntexist）。
Source: "{#SourceRoot}\fonts\NotoSansSC-*.subset.ttf"; DestDir: "{app}\fonts"; Flags: ignoreversion
; 不再打包磁盘 web/：服务端页面来自编译期内嵌资源，磁盘目录是死资源。
; 实测拿掉后 GET /、/styles.css、/app.js 的响应字节与内嵌资源完全一致。

[Icons]
Name: "{group}\DevCard"; Filename: "{app}\DevCard.exe"
Name: "{group}\DevCard 服务端"; Filename: "{app}\devcard-server.exe"; Parameters: "--server"
Name: "{group}\卸载 DevCard"; Filename: "{uninstallexe}"
Name: "{autodesktop}\DevCard"; Filename: "{app}\DevCard.exe"; WorkingDir: "{app}"; Tasks: desktopicon

[Run]
Filename: "{app}\DevCard.exe"; Description: "启动 DevCard"; Flags: nowait postinstall skipifsilent
; 应用内更新：壳引擎拉起安装器时传 /RELAUNCH=1，静默安装完成后自动启动新版本
Filename: "{app}\DevCard.exe"; Flags: nowait; Check: ShouldRelaunch

[UninstallDelete]
; 只清理程序目录内的残留。数据目录 {localappdata}\DevCard\data 不在 {app} 下，
; 默认不会被删除；这里也不要加任何指向它的条目。
Type: filesandordirs; Name: "{app}\fonts"

[Code]
const
  WebView2Key = 'Software\Microsoft\EdgeUpdate\Clients\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}';
  WebView2Url = 'https://go.microsoft.com/fwlink/p/?LinkId=2124703';

// 应用内更新（shell/win/updater_win.cpp）传入 /RELAUNCH=1：
// 静默安装结束后自动启动新版本，用户不用手动重开。
function ShouldRelaunch: Boolean;
begin
  Result := ExpandConstant('{param:RELAUNCH|0}') = '1';
end;

function RuntimeVersionInRoot(RootKey: Integer; const SubKey: String): String;
var
  Value: String;
begin
  if RegQueryStringValue(RootKey, SubKey, 'pv', Value) then
    Result := Trim(Value)
  else
    Result := '';
end;

// WebView2 可能装在 HKLM（64/32 位视图）或当前用户下，三处都要查。
function WebView2RuntimeVersion: String;
begin
  Result := RuntimeVersionInRoot(HKLM64, WebView2Key);
  if Result = '' then
    Result := RuntimeVersionInRoot(HKLM32, WebView2Key);
  if Result = '' then
    Result := RuntimeVersionInRoot(HKCU, WebView2Key);
end;

function WebView2Missing: Boolean;
begin
  Result := WebView2RuntimeVersion = '';
end;

// 下载官方 Evergreen Bootstrapper 后静默安装；非管理员安装到当前用户，不需要 UAC。
// 下载失败不强行继续：明确提示手动安装地址，让用户自己决定。
procedure InstallWebView2;
var
  SetupFile, ScriptFile: String;
  ResultCode: Integer;
begin
  SetupFile := ExpandConstant('{tmp}\MicrosoftEdgeWebview2Setup.exe');
  ScriptFile := ExpandConstant('{tmp}\devcard-webview2-download.ps1');
  SaveStringToFile(ScriptFile,
    '$ProgressPreference = ''SilentlyContinue'';' + #13#10 +
    'Invoke-WebRequest -Uri "' + WebView2Url + '" -OutFile "' + SetupFile + '"',
    False);
  if (not Exec(ExpandConstant('{sys}\WindowsPowerShell\v1.0\powershell.exe'),
        '-NoProfile -ExecutionPolicy Bypass -File "' + ScriptFile + '"',
        '', SW_HIDE, ewWaitUntilTerminated, ResultCode))
      or (ResultCode <> 0) or (not FileExists(SetupFile)) then
  begin
    // 静默安装时不弹阻塞对话框，只写安装日志，避免无人值守部署卡住。
    SuppressibleMsgBox('WebView2 Runtime 下载失败（需要联网）。' + #13#10 +
      '请手动安装后重新运行 DevCard：' + #13#10 + WebView2Url,
      mbError, MB_OK, IDOK);
    exit;
  end;
  if (not Exec(SetupFile, '/silent /install', '',
        SW_SHOW, ewWaitUntilTerminated, ResultCode))
      or (ResultCode <> 0) then
    SuppressibleMsgBox('WebView2 Runtime 安装未成功（退出码 ' + IntToStr(ResultCode) + '）。' + #13#10 +
      '请手动安装：' + #13#10 + WebView2Url,
      mbError, MB_OK, IDOK);
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if (CurStep = ssPostInstall) and WebView2Missing
      and WizardIsTaskSelected('webview2') then
    InstallWebView2;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usPostUninstall then
    // 用 SuppressibleMsgBox：静默卸载（/SUPPRESSMSGBOXES）直接取默认值，
    // 避免普通 MsgBox 阻塞导致卸载器残留 _unins.tmp。
    SuppressibleMsgBox('程序已卸载，台账数据仍保留在：' + #13#10 +
           ExpandConstant('{localappdata}\DevCard\data'),
           mbInformation, MB_OK, IDOK);
end;
