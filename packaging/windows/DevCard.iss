; DevCard Windows 安装脚本（Inno Setup 6.x）
;
; 编译（Windows，需先构建好两个 exe 与字体）：
;   "C:\Program Files (x86)\Inno Setup 6\ISCC.exe" ^
;     /DSourceRoot=..\..\dist\Mote-Windows ^
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
  #define SourceRoot "..\..\dist\Mote-Windows"
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

[Languages]
Name: "en"; MessagesFile: "compiler:Default.isl"

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

[Run]
Filename: "{app}\DevCard.exe"; Description: "启动 DevCard"; Flags: nowait postinstall skipifsilent

[UninstallDelete]
; 只清理程序目录内的残留。数据目录 {localappdata}\DevCard\data 不在 {app} 下，
; 默认不会被删除；这里也不要加任何指向它的条目。
Type: filesandordirs; Name: "{app}\fonts"

[Code]
procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usPostUninstall then
    MsgBox('程序已卸载，台账数据仍保留在：' + #13#10 +
           ExpandConstant('{localappdata}\DevCard\data'),
           mbInformation, MB_OK);
end;
