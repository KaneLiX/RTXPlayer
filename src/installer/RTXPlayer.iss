; RTXPlayer installer (Inno Setup 7)
; Build: ISCC.exe RTXPlayer.iss   (packages the app folder two levels up)

#define AppName "RTXPlayer"
#define AppVersion "1.1.1"
#define AppExe "RTXPlayer.exe"
#define AppRoot SourcePath + "..\.."
#define ProgId "RTXPlayer.Video"

[Setup]
AppId={{C97F49E1-A1A4-41C5-BF5B-52BE0AB95E09}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher={#AppName}
VersionInfoVersion={#AppVersion}.0
VersionInfoDescription={#AppName} 安装程序
; Per-user install by default (no admin needed); the dialog offers "all users".
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
DefaultDirName={autopf}\{#AppName}
DisableProgramGroupPage=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0.17763
Compression=lzma2/ultra64
SolidCompression=yes
LZMANumBlockThreads=8
OutputDir={#AppRoot}\..\RTXPlayer_Installer
OutputBaseFilename={#AppName}-Setup-{#AppVersion}
SetupIconFile={#AppRoot}\src\app.ico
UninstallDisplayIcon={app}\{#AppExe}
UninstallDisplayName={#AppName}
WizardStyle=modern
ChangesAssociations=yes
CloseApplications=yes
RestartApplications=no

[Languages]
Name: "chinesesimplified"; MessagesFile: "compiler:Languages\ChineseSimplified.isl"
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"
Name: "openwith"; Description: "添加到视频文件的“打开方式”菜单（不会更改默认播放器）"; GroupDescription: "文件关联:"

[Files]
Source: "{#AppRoot}\{#AppExe}"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#AppRoot}\libmpv-2.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#AppRoot}\d3dcompiler_43.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#AppRoot}\使用说明.txt"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#AppRoot}\scripts\*"; DestDir: "{app}\scripts"; Excludes: "__pycache__\*,*.pyc"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#AppRoot}\shaders\*"; DestDir: "{app}\shaders"; Flags: ignoreversion
Source: "{#AppRoot}\vs\*"; DestDir: "{app}\vs"; Excludes: "__pycache__\*,*.pyc"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#AppRoot}\vsmlrt\*"; DestDir: "{app}\vsmlrt"; Excludes: "__pycache__\*,*.pyc"; Flags: ignoreversion recursesubdirs createallsubdirs
; Not packaged: engines\ (built per GPU on first run), settings/logs (user data in
; %LOCALAPPDATA%\RTXPlayer), the "portable" marker and src\.

[Icons]
Name: "{autoprograms}\{#AppName}"; Filename: "{app}\{#AppExe}"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#AppExe}"; Tasks: desktopicon

[Registry]
; ProgID used by "Open with" and by Windows Settings > Default apps.
Root: HKA; Subkey: "Software\Classes\{#ProgId}"; ValueType: string; ValueName: ""; ValueData: "RTXPlayer 视频文件"; Flags: uninsdeletekey; Tasks: openwith
Root: HKA; Subkey: "Software\Classes\{#ProgId}\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\{#AppExe},0"; Tasks: openwith
Root: HKA; Subkey: "Software\Classes\{#ProgId}\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\{#AppExe}"" ""%1"""; Tasks: openwith
Root: HKA; Subkey: "Software\Classes\Applications\{#AppExe}"; ValueType: string; ValueName: "FriendlyAppName"; ValueData: "{#AppName}"; Flags: uninsdeletekey; Tasks: openwith
Root: HKA; Subkey: "Software\Classes\Applications\{#AppExe}\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\{#AppExe}"" ""%1"""; Tasks: openwith
Root: HKA; Subkey: "Software\{#AppName}"; Flags: uninsdeletekeyifempty; Tasks: openwith
Root: HKA; Subkey: "Software\{#AppName}\Capabilities"; ValueType: string; ValueName: "ApplicationName"; ValueData: "{#AppName}"; Flags: uninsdeletekey; Tasks: openwith
Root: HKA; Subkey: "Software\{#AppName}\Capabilities"; ValueType: string; ValueName: "ApplicationDescription"; ValueData: "带 NVIDIA RTX 超分辨率与 AI 插帧的视频播放器"; Tasks: openwith
Root: HKA; Subkey: "Software\RegisteredApplications"; ValueType: string; ValueName: "{#AppName}"; ValueData: "Software\{#AppName}\Capabilities"; Flags: uninsdeletevalue; Tasks: openwith
#dim Exts[18] {".mkv", ".mp4", ".avi", ".mov", ".wmv", ".flv", ".webm", ".ts", ".m2ts", ".mts", ".rmvb", ".rm", ".mpg", ".mpeg", ".m4v", ".3gp", ".vob", ".ogv"}
#define i
#sub ExtEntries
Root: HKA; Subkey: "Software\Classes\{#Exts[i]}\OpenWithProgids"; ValueType: string; ValueName: "{#ProgId}"; ValueData: ""; Flags: uninsdeletevalue; Tasks: openwith
Root: HKA; Subkey: "Software\Classes\Applications\{#AppExe}\SupportedTypes"; ValueType: string; ValueName: "{#Exts[i]}"; ValueData: ""; Tasks: openwith
Root: HKA; Subkey: "Software\{#AppName}\Capabilities\FileAssociations"; ValueType: string; ValueName: "{#Exts[i]}"; ValueData: "{#ProgId}"; Tasks: openwith
#endsub
#for {i = 0; i < DimOf(Exts); i++} ExtEntries

[Run]
Filename: "{app}\使用说明.txt"; Description: "查看使用说明"; Flags: postinstall shellexec skipifsilent unchecked
Filename: "{app}\{#AppExe}"; Description: "{cm:LaunchProgram,{#AppName}}"; Flags: nowait postinstall skipifsilent

[UninstallDelete]
; Python may leave __pycache__ folders behind; these folders belong to us entirely.
Type: filesandordirs; Name: "{app}\scripts"
Type: filesandordirs; Name: "{app}\vs"
Type: filesandordirs; Name: "{app}\vsmlrt"

[Code]
function HasRtxGpu(): Boolean;
var
  Locator, Service, Items: Variant;
  I: Integer;
  Name: String;
begin
  Result := False;
  try
    Locator := CreateOleObject('WbemScripting.SWbemLocator');
    Service := Locator.ConnectServer('.', 'root\cimv2');
    Items := Service.ExecQuery('SELECT Name FROM Win32_VideoController');
    for I := 0 to Items.Count - 1 do
    begin
      Name := Uppercase(Items.ItemIndex(I).Name);
      if (Pos('NVIDIA', Name) > 0) and (Pos('RTX', Name) > 0) then
        Result := True;
    end;
  except
    Result := True;  { could not query WMI: do not block the install }
  end;
end;

function InitializeSetup(): Boolean;
begin
  Result := True;
  if not HasRtxGpu() then
    Result := SuppressibleMsgBox(
      '未检测到 NVIDIA RTX 显卡。' + #13#10#13#10 +
      'RTXPlayer 的超分辨率和插帧需要 RTX 显卡，没有的话只能作为普通播放器使用。' + #13#10#13#10 +
      '是否继续安装？', mbConfirmation, MB_YESNO, IDYES) = IDYES;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  DataDir: String;
begin
  if CurUninstallStep = usPostUninstall then
  begin
    DataDir := ExpandConstant('{localappdata}\{#AppName}');
    if DirExists(DataDir) and not UninstallSilent then
      if MsgBox('是否同时删除 RTXPlayer 的设置和 RTX 引擎缓存？' + #13#10#13#10 + DataDir + #13#10#13#10 +
                '选“否”会保留它们，以后重新安装无需再次优化引擎。', mbConfirmation, MB_YESNO) = IDYES then
        DelTree(DataDir, True, True, True);
  end;
end;
