; Inno Setup 6 script: installs LookMatch.aex into Adobe's shared MediaCore folder and shows
; which Premiere Pro / After Effects versions it found.
;   iscc /DAppVersion=1.0.0 /DPluginPath=..\..\build\src\premiere\Release\LookMatch.aex LookMatch.iss

#ifndef AppVersion
  #define AppVersion "1.0.0"
#endif
#ifndef PluginPath
  #define PluginPath "..\..\build\src\premiere\Release\LookMatch.aex"
#endif

[Setup]
AppId={{6D1C2E4A-7B1F-4C2D-9E57-4C4F4F4B4D41}
AppName=LookMatch
AppVersion={#AppVersion}
AppPublisher=PRO PLUGS
DefaultDirName={commoncf64}\Adobe\Common\Plug-ins\7.0\MediaCore
DisableDirPage=yes
DisableProgramGroupPage=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin
OutputDir=dist
OutputBaseFilename=LookMatch-{#AppVersion}-Setup
WizardStyle=modern

[Files]
Source: "{#PluginPath}"; DestDir: "{app}"; Flags: ignoreversion

[Code]
const
  MIN_PREMIERE = 9;   { Premiere Pro CC 2015 }
  MIN_AE = 13;        { After Effects CC 2015 (13.5) }

var
  HostPage: TOutputMsgMemoWizardPage;

{ Each installed version has a key like HKLM\SOFTWARE\Adobe\Premiere Pro\23.0 }
procedure ScanHosts(const KeyName, Label: String; MinMajor: Integer; var Found, Old: String);
var
  Names: TArrayOfString;
  I, Major, Dot: Integer;
  Path: String;
begin
  if not RegGetSubkeyNames(HKLM64, 'SOFTWARE\Adobe\' + KeyName, Names) then Exit;
  for I := 0 to GetArrayLength(Names) - 1 do
  begin
    if not RegQueryStringValue(HKLM64, 'SOFTWARE\Adobe\' + KeyName + '\' + Names[I], 'InstallPath', Path) then Continue;
    Dot := Pos('.', Names[I]);
    if Dot > 0 then Major := StrToIntDef(Copy(Names[I], 1, Dot - 1), 0) else Major := StrToIntDef(Names[I], 0);
    Found := Found + Label + ' ' + Names[I] + #13#10;
    if (Major > 0) and (Major < MinMajor) then Old := Old + Label + ' ' + Names[I] + #13#10;
  end;
end;

procedure InitializeWizard;
var
  Found, Old, Msg: String;
begin
  ScanHosts('Premiere Pro', 'Premiere Pro', MIN_PREMIERE, Found, Old);
  ScanHosts('After Effects', 'After Effects', MIN_AE, Found, Old);
  if Found = '' then
    Msg := 'No Premiere Pro or After Effects installation was found.' + #13#10 +
           'LookMatch will still be installed and will appear once you install Premiere Pro or After Effects CC 2015 or newer.'
  else
  begin
    Msg := 'LookMatch will be available in:' + #13#10 + Found;
    if Old <> '' then
      Msg := Msg + #13#10 + 'These versions are older than CC 2015 and will not load LookMatch:' + #13#10 + Old;
  end;
  HostPage := CreateOutputMsgMemoPage(wpWelcome, 'Adobe apps on this PC',
    'LookMatch installs once into Adobe''s shared plug-in folder and works in every supported version.', '', Msg);
end;
