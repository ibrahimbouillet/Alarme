; Copyright (c) 2026 Ibrahim Bouillet. SPDX-License-Identifier: MIT
; Build only through scripts/package-installer.ps1, with a validated release payload.
#if Ver < EncodeVer(6, 7, 0)
  #error Inno Setup 6.7 or newer is required for the green wizard background.
#endif
#ifndef PayloadDir
  #error PayloadDir is required.
#endif
#ifndef BrandingDir
  #error BrandingDir is required.
#endif
#ifndef AppVersion
  #error AppVersion is required.
#endif
#ifndef InstallerOutputDir
  #error InstallerOutputDir is required.
#endif

[Setup]
AppId=ALARM.IbrahimBouillet.Setup
AppName=ALARM
AppVersion={#AppVersion}
AppVerName=ALARM {#AppVersion}
AppPublisher=Ibrahim Bouillet
AppPublisherURL=https://github.com/ibrahimbouillet/Alarme
AppSupportURL=https://github.com/ibrahimbouillet/Alarme/issues
AppUpdatesURL=https://github.com/ibrahimbouillet/Alarme/releases
VersionInfoVersion={#AppVersion}.0
VersionInfoDescription=ALARM Windows installer
VersionInfoCopyright=Copyright (c) 2026 Ibrahim Bouillet
DefaultDirName={localappdata}\Programs\ALARM
DefaultGroupName=ALARM
DisableProgramGroupPage=yes
DisableDirPage=auto
DisableWelcomePage=no
DisableReadyPage=no
UsePreviousAppDir=yes
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
AllowRootDirectory=no
AllowNetworkDrive=no
AllowUNCPath=no
OutputDir={#InstallerOutputDir}
OutputBaseFilename=ALARM-{#AppVersion}-windows-x64-setup
SetupIconFile=..\assets\alarm.ico
UninstallDisplayIcon={app}\ALARM.exe
UninstallDisplayName=ALARM
Compression=lzma2/max
SolidCompression=yes
SetupLogging=yes
CloseApplications=no
RestartApplications=no
RestartIfNeededByRun=no
WizardStyle=modern dark hidebevels
WizardBackColor=#10291f
WizardImageBackColor=#10291f
WizardSmallImageBackColor=#10291f
WizardImageFile={#BrandingDir}\wizard-side.bmp
WizardSmallImageFile={#BrandingDir}\wizard-icon.bmp
WizardImageStretch=yes
WizardSizePercent=110
DefaultDialogFontName=Segoe UI
ShowLanguageDialog=auto
LicenseFile={#PayloadDir}\LICENSE
ChangesAssociations=yes

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "french"; MessagesFile: "compiler:Languages\French.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "{#PayloadDir}\ALARM.exe"; DestDir: "{app}"; Flags: ignoreversion
; Refresh installation guidance while keeping the released application unchanged.
Source: "..\INSTALL.md"; DestDir: "{app}"; DestName: "README.md"; Flags: ignoreversion
Source: "{#PayloadDir}\TECHNICAL_FEASIBILITY.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#PayloadDir}\LICENSE"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#PayloadDir}\THIRD_PARTY_NOTICES.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#PayloadDir}\docs\*"; DestDir: "{app}\docs"; Flags: ignoreversion recursesubdirs
Source: "{#PayloadDir}\licenses\*"; DestDir: "{app}\licenses"; Flags: ignoreversion recursesubdirs
Source: "{#BrandingDir}\.alarm-setup-owner"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{userprograms}\ALARM"; Filename: "{app}\ALARM.exe"; WorkingDir: "{app}"; Comment: "Every day, at your time."; AppUserModelID: "Ibrahim.ALARM.Desktop"; Flags: uninsneveruninstall
Name: "{userdesktop}\ALARM"; Filename: "{app}\ALARM.exe"; WorkingDir: "{app}"; Tasks: desktopicon; Flags: uninsneveruninstall

[Messages]
english.WelcomeLabel1=Your time. Your ALARM.
english.WelcomeLabel2=Install ALARM on this PC for your Windows account.%n%nA daily alarm with a clear, focused interface. Works offline, with no account required.%n%nSetup does not launch ALARM or enable an alarm automatically.
english.FinishedLabel=ALARM is ready.%n%nOpen ALARM from the Start menu, choose your time, then enable your alarm.%n%nYour personal settings are kept when you uninstall.
french.WelcomeLabel1=Votre heure. Votre ALARM.
french.WelcomeLabel2=Installez ALARM sur ce PC pour votre compte Windows.%n%nUne alarme quotidienne simple et soignée. Fonctionne hors ligne, sans compte.%n%nL'installation ne lance pas ALARM et n'active aucune alarme automatiquement.
french.FinishedLabel=ALARM est prêt.%n%nOuvrez ALARM depuis le menu Démarrer, choisissez votre heure, puis activez votre alarme.%n%nVos réglages personnels sont conservés à la désinstallation.

[CustomMessages]
english.LegacyInstall=An older PowerShell installation of ALARM was found. Quit ALARM, then uninstall that copy from Windows Installed apps (or its uninstall.ps1). Your settings are preserved. Run this installer again afterward.
english.OccupiedDirectory=This folder contains files that this installer does not own. Choose an empty folder. If it contains an older ALARM installation, uninstall that copy first; your settings will be preserved.
english.UnsafePath=Installation and removal through a symbolic link or junction are not supported. Choose a normal, local application folder.
english.OtherInstall=Another ALARM setup installation is registered. Uninstall it before choosing a different folder. Your settings will be preserved.
english.ShortcutConflict=An existing ALARM shortcut points to a different application or cannot be read. Move or remove that shortcut before continuing.
english.QuitAlarm=Quit ALARM from its tray menu before updating or uninstalling. Also check that the application folder is writable.
english.InvalidOwner=This folder is not a recognized ALARM setup installation. No files were removed.
english.DataDirectory=Choose an application folder outside ALARM's personal settings folders.
french.LegacyInstall=Une ancienne installation PowerShell d'ALARM a été détectée. Quittez ALARM, puis désinstallez cette copie depuis les applications installées de Windows (ou avec son script uninstall.ps1). Vos réglages sont conservés. Relancez ensuite cet installeur.
french.OccupiedDirectory=Ce dossier contient des fichiers qui n'appartiennent pas à cet installeur. Choisissez un dossier vide. S'il contient une ancienne installation d'ALARM, désinstallez-la d'abord ; vos réglages seront conservés.
french.UnsafePath=L'installation et la désinstallation via un lien symbolique ou une jonction ne sont pas prises en charge. Choisissez un dossier local normal.
french.OtherInstall=Une autre installation d'ALARM est enregistrée. Désinstallez-la avant de choisir un autre dossier. Vos réglages seront conservés.
french.ShortcutConflict=Un raccourci ALARM existant pointe vers une autre application ou ne peut pas être lu. Déplacez ou supprimez ce raccourci avant de continuer.
french.QuitAlarm=Quittez ALARM depuis son icône de notification avant la mise à jour ou la désinstallation. Vérifiez aussi que le dossier de l'application est accessible en écriture.
french.InvalidOwner=Ce dossier n'est pas une installation ALARM reconnue. Aucun fichier n'a été supprimé.
french.DataDirectory=Choisissez un dossier d'application distinct des dossiers de réglages personnels d'ALARM.

[Code]
const
  LegacyKey = 'Software\Microsoft\Windows\CurrentVersion\Uninstall\ALARM.IbrahimBouillet';
  SetupKey = 'Software\Microsoft\Windows\CurrentVersion\Uninstall\ALARM.IbrahimBouillet.Setup_is1';
  OwnerFile = '.alarm-setup-owner';
  OwnerText = 'ALARM-INNO-SETUP-v1';

function GetFileAttributesW(FileName: String): LongWord;
  external 'GetFileAttributesW@kernel32.dll stdcall';
function CreateFileW(FileName: String; Access, Share: LongWord; Security: Integer;
  Disposition, Attributes: LongWord; Template: Integer): THandle;
  external 'CreateFileW@kernel32.dll stdcall';
function CloseHandle(Handle: THandle): Boolean;
  external 'CloseHandle@kernel32.dll stdcall';

function SamePath(A, B: String): Boolean;
begin
  Result := CompareText(RemoveBackslashUnlessRoot(ExpandFileName(A)),
    RemoveBackslashUnlessRoot(ExpandFileName(B))) = 0;
end;

function HasReparseAncestor(Path: String): Boolean;
var
  Parent: String;
  Attributes: LongWord;
begin
  Result := False;
  repeat
    Attributes := GetFileAttributesW(Path);
    if (Attributes <> $FFFFFFFF) and ((Attributes and $400) <> 0) then begin
      Result := True;
      Exit;
    end;
    Parent := ExtractFileDir(Path);
    if SamePath(Parent, Path) then Exit;
    Path := Parent;
  until Path = '';
end;

function TreeHasReparsePoint(Path: String): Boolean;
var
  FindRec: TFindRec;
  Child: String;
begin
  Result := False;
  if FindFirst(AddBackslash(Path) + '*', FindRec) then begin
    try
      repeat
        if (FindRec.Name <> '.') and (FindRec.Name <> '..') then begin
          Child := AddBackslash(Path) + FindRec.Name;
          if ((FindRec.Attributes and $400) <> 0) then Result := True
          else if (FindRec.Attributes and FILE_ATTRIBUTE_DIRECTORY) <> 0 then
            Result := TreeHasReparsePoint(Child);
          if Result then Exit;
        end;
      until not FindNext(FindRec);
    finally
      FindClose(FindRec);
    end;
  end;
end;

function DirectoryIsEmpty(Path: String): Boolean;
var
  FindRec: TFindRec;
begin
  Result := True;
  if FindFirst(AddBackslash(Path) + '*', FindRec) then begin
    try
      repeat
        if (FindRec.Name <> '.') and (FindRec.Name <> '..') then begin
          Result := False;
          Exit;
        end;
      until not FindNext(FindRec);
    finally
      FindClose(FindRec);
    end;
  end;
end;

function HasOwner(Path: String): Boolean;
var
  Text: AnsiString;
begin
  Result := LoadStringFromFile(AddBackslash(Path) + OwnerFile, Text) and
    (Trim(String(Text)) = OwnerText);
end;

function ShortcutOwnedOrAbsent(Path, Target: String): Boolean;
var
  Shell, Link: Variant;
begin
  Result := not FileExists(Path);
  if Result then Exit;
  if HasReparseAncestor(Path) then Exit;
  try
    Shell := CreateOleObject('WScript.Shell');
    Link := Shell.CreateShortcut(Path);
    Result := SamePath(Link.TargetPath, Target);
  except
    Result := False;
  end;
end;

function ExecutableIsWritable(Path: String): Boolean;
var
  Handle: THandle;
begin
  Result := not FileExists(Path);
  if Result then Exit;
  { An executable mapped by a running process cannot be opened for writing. }
  Handle := CreateFileW(Path, $40000000, 7, 0, 3, $80, 0);
  Result := Handle <> THandle(-1);
  if Result then CloseHandle(Handle);
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  AppPath, Registered, DataPath: String;
begin
  Result := '';
  AppPath := RemoveBackslashUnlessRoot(ExpandConstant('{app}'));
  if RegKeyExists(HKCU, LegacyKey) or FileExists(AddBackslash(AppPath) + '.alarm-install.json') then begin
    Result := CustomMessage('LegacyInstall'); Exit;
  end;
  if HasReparseAncestor(AppPath) or TreeHasReparsePoint(AppPath) then begin
    Result := CustomMessage('UnsafePath'); Exit;
  end;
  DataPath := ExpandConstant('{localappdata}\Ibrahim\ALARM');
  if SamePath(AppPath, DataPath) or (Pos(Lowercase(AddBackslash(DataPath)), Lowercase(AddBackslash(AppPath))) = 1) then begin
    Result := CustomMessage('DataDirectory'); Exit;
  end;
  DataPath := GetEnv('USERPROFILE') + '\.alarm';
  if SamePath(AppPath, DataPath) or (Pos(Lowercase(AddBackslash(DataPath)), Lowercase(AddBackslash(AppPath))) = 1) then begin
    Result := CustomMessage('DataDirectory'); Exit;
  end;
  if RegQueryStringValue(HKCU, SetupKey, 'InstallLocation', Registered) then begin
    if not SamePath(Registered, AppPath) then begin
      Result := CustomMessage('OtherInstall'); Exit;
    end;
    if not HasOwner(AppPath) then begin
      Result := CustomMessage('OccupiedDirectory'); Exit;
    end;
  end else if DirExists(AppPath) and not DirectoryIsEmpty(AppPath) then begin
    Result := CustomMessage('OccupiedDirectory'); Exit;
  end;
  if not ShortcutOwnedOrAbsent(ExpandConstant('{userprograms}\ALARM.lnk'), AddBackslash(AppPath) + 'ALARM.exe') then begin
    Result := CustomMessage('ShortcutConflict'); Exit;
  end;
  if WizardIsTaskSelected('desktopicon') and not ShortcutOwnedOrAbsent(ExpandConstant('{userdesktop}\ALARM.lnk'), AddBackslash(AppPath) + 'ALARM.exe') then begin
    Result := CustomMessage('ShortcutConflict'); Exit;
  end;
  if not ExecutableIsWritable(AddBackslash(AppPath) + 'ALARM.exe') then
    Result := CustomMessage('QuitAlarm');
end;

function InitializeUninstall(): Boolean;
var
  AppPath: String;
begin
  AppPath := ExpandConstant('{app}');
  Result := False;
  if HasReparseAncestor(AppPath) or TreeHasReparsePoint(AppPath) then
    SuppressibleMsgBox(CustomMessage('UnsafePath'), mbError, MB_OK, IDOK)
  else if not HasOwner(AppPath) then
    SuppressibleMsgBox(CustomMessage('InvalidOwner'), mbError, MB_OK, IDOK)
  else if not ExecutableIsWritable(AddBackslash(AppPath) + 'ALARM.exe') then
    SuppressibleMsgBox(CustomMessage('QuitAlarm'), mbError, MB_OK, IDOK)
  else Result := True;
end;

procedure RemoveOwnedShortcut(Path, Target: String);
begin
  if FileExists(Path) and ShortcutOwnedOrAbsent(Path, Target) then DeleteFile(Path);
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  Exe, Command, Identity: String;
begin
  if CurUninstallStep <> usUninstall then Exit;
  Exe := ExpandConstant('{app}\ALARM.exe');
  RemoveOwnedShortcut(ExpandConstant('{userprograms}\ALARM.lnk'), Exe);
  RemoveOwnedShortcut(ExpandConstant('{userdesktop}\ALARM.lnk'), Exe);
  if RegQueryStringValue(HKCU, 'Software\Microsoft\Windows\CurrentVersion\Run', 'ALARM.Ibrahim', Command) and
    (Command = '"' + Exe + '" --background') then
    RegDeleteValue(HKCU, 'Software\Microsoft\Windows\CurrentVersion\Run', 'ALARM.Ibrahim');
  if RegQueryStringValue(HKCU, 'Software\Classes\ibrahim-alarm', 'ALARMIdentity', Identity) and
    (Identity = 'Ibrahim.ALARM.Desktop') and
    RegQueryStringValue(HKCU, 'Software\Classes\ibrahim-alarm\shell\open\command', '', Command) and
    (Command = '"' + Exe + '" --open') then begin
    RegDeleteKeyIncludingSubkeys(HKCU, 'Software\Classes\ibrahim-alarm');
    if RegQueryStringValue(HKCU, 'Software\Classes\AppUserModelId\Ibrahim.ALARM.Desktop', 'ALARMIdentity', Identity) and
      (Identity = 'Ibrahim.ALARM.Desktop') then
      RegDeleteKeyIncludingSubkeys(HKCU, 'Software\Classes\AppUserModelId\Ibrahim.ALARM.Desktop');
  end;
end;
