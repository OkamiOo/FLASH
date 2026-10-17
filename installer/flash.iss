; Installeur de FLASH : le dossier de l'édition portable, installé pour l'utilisateur seul
; (%LOCALAPPDATA%\Programs\FLASH), sans élévation : les mises à jour se font ensuite sans
; demander l'autorisation de Windows. Le plugin pour OBS Studio (dans ProgramData) est proposé
; par l'appli au premier lancement. Compilé par « build.ps1 -Installeur ».

#ifndef Version
  #define Version "0.0.0"
#endif
#ifndef Dossier
  #error Dossier : dossier de l'édition portable non défini
#endif

#define Plugin "{commonappdata}\obs-studio\plugins\flash-obs"

[Setup]
AppId={{6F0C2B1E-5D8A-4C3B-9E27-1A4F8D6B3C90}
AppName=FLASH
AppVersion={#Version}
AppVerName=FLASH {#Version}
AppPublisher=Oka
DefaultDirName={autopf}\FLASH
DefaultGroupName=FLASH
DisableProgramGroupPage=yes
DisableDirPage=yes
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0.19041
OutputBaseFilename=FLASH-{#Version}-installation
SetupIconFile=..\app\ui\flash.ico
UninstallDisplayIcon={app}\flash.exe
UninstallDisplayName=FLASH
#ifdef SansCompression
; Copie de contrôle : contenu lisible tel quel
Compression=none
#else
Compression=lzma2/max
SolidCompression=yes
#endif
WizardStyle=modern
ShowLanguageDialog=no
; FLASH et OBS sont gérés par le code plus bas, rien n'est fermé d'office
CloseApplications=no
RestartApplications=no
VersionInfoVersion={#Version}
VersionInfoProductName=FLASH
VersionInfoDescription=Installation de FLASH

[Languages]
Name: "fr"; MessagesFile: "compiler:Languages\French.isl"

[Tasks]
Name: "bureau"; Description: "Créer une icône sur le bureau"; Flags: unchecked

[Files]
Source: "{#Dossier}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs

[InstallDelete]
; Restes d'une mise à jour automatique
Type: files; Name: "{app}\*.ancien"
Type: filesandordirs; Name: "{app}\maj"

[UninstallDelete]
Type: files; Name: "{app}\*.ancien"
Type: filesandordirs; Name: "{app}\maj"

[Icons]
Name: "{autoprograms}\FLASH"; Filename: "{app}\flash.exe"
Name: "{autodesktop}\FLASH"; Filename: "{app}\flash.exe"; Tasks: bureau

[Registry]
; Lancement avec Windows : réglé dans l'appli, retiré avec elle
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: none; ValueName: "FLASH"; Flags: uninsdeletevalue dontcreatekey

[Run]
Filename: "{app}\flash.exe"; Description: "Ouvrir FLASH"; Flags: postinstall nowait skipifsilent

[Code]
const
  FlashMutex = 'Local\FLASH-appli';
  ObsMutex = 'OBSStudioCore';
  AncienneCle = 'Software\Microsoft\Windows\CurrentVersion\Uninstall\{6F0C2B1E-5D8A-4C3B-9E27-1A4F8D6B3C90}_is1';

// FLASH ouvert (souvent dans la zone de notification) : on lui demande de quitter, comme son
// menu Quitter, puis on attend qu'il soit fermé
function FermerFlash(): Boolean;
var
  Fenetre: HWND;
  i: Integer;
begin
  Result := True;
  if not CheckForMutexes(FlashMutex) then
    Exit;
  Fenetre := FindWindowByClassName('FlashFenetre');
  if Fenetre <> 0 then
    PostMessage(Fenetre, RegisterWindowMessage('FLASH-quitter'), 0, 0);
  for i := 1 to 50 do begin
    if not CheckForMutexes(FlashMutex) then
      Exit;
    Sleep(100);
  end;
  Result := False;
end;

// OBS doit être fermé pour que le plugin soit retiré : on le demande, sans le fermer
function AttendreObs(const Action: String): Boolean;
begin
  Result := True;
  while CheckForMutexes(ObsMutex) do begin
    if SuppressibleMsgBox('OBS Studio est ouvert. Fermez-le pour ' + Action + ', puis cliquez sur OK.',
                          mbInformation, MB_OKCANCEL, IDCANCEL) = IDCANCEL then begin
      Result := False;
      Exit;
    end;
  end;
end;

// Les versions 0.1 et 0.2 s'installaient pour tous les utilisateurs (Program Files) : deux FLASH
// côte à côte se disputeraient les mêmes réglages et le même plugin
function InitializeSetup(): Boolean;
var
  Desinstalleur: String;
  Code: Integer;
begin
  Result := True;
  if not RegQueryStringValue(HKLM64, AncienneCle, 'UninstallString', Desinstalleur) then
    Exit;
  if SuppressibleMsgBox('Une version précédente de FLASH est installée pour tous les utilisateurs. ' +
                        'Elle doit être désinstallée d''abord (Windows demandera l''autorisation). La désinstaller maintenant ?',
                        mbConfirmation, MB_YESNO, IDYES) = IDYES then begin
    if not ShellExec('runas', RemoveQuotes(Desinstalleur), '', '', SW_SHOWNORMAL, ewWaitUntilTerminated, Code) then
      SuppressibleMsgBox('Désinstallation impossible : ' + SysErrorMessage(Code), mbError, MB_OK, IDOK);
  end;
  Result := not RegKeyExists(HKLM64, AncienneCle);
  if not Result then
    SuppressibleMsgBox('Installation arrêtée : désinstallez d''abord l''ancienne version de FLASH (Paramètres, Applications).',
                       mbError, MB_OK, IDOK);
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  Result := '';
  if not FermerFlash() then
    Result := 'FLASH est encore ouvert. Quittez-le (clic droit sur son icône près de l''horloge, puis Quitter) et relancez l''installation.';
end;

function InitializeUninstall(): Boolean;
begin
  Result := FermerFlash();
  if not Result then begin
    SuppressibleMsgBox('FLASH est encore ouvert. Quittez-le (clic droit sur son icône près de l''horloge, puis Quitter) et relancez la désinstallation.',
                       mbError, MB_OK, IDOK);
    Exit;
  end;
  if DirExists(ExpandConstant('{#Plugin}')) then
    Result := AttendreObs('retirer le plugin FLASH');
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  Code: Integer;
begin
  // Plugin d'OBS : retiré par l'appli, qui demande l'élévation seulement si Windows l'exige
  if (CurUninstallStep = usUninstall) and DirExists(ExpandConstant('{#Plugin}')) then
    Exec(ExpandConstant('{app}\flash.exe'), '--plugin-retrait', '', SW_HIDE, ewWaitUntilTerminated, Code);
  if CurUninstallStep <> usPostUninstall then
    Exit;
  // Réglages et appairages de FLASH et du plugin : gardés, sauf si on demande de les effacer
  if SuppressibleMsgBox('Effacer aussi les réglages de FLASH et ses appairages avec les autres PC ?',
                        mbConfirmation, MB_YESNO, IDNO) = IDYES then begin
    DelTree(ExpandConstant('{userappdata}\FLASH'), True, True, True);
    DelTree(ExpandConstant('{localappdata}\FLASH'), True, True, True);
    DelTree(ExpandConstant('{userappdata}\obs-studio\plugin_config\flash-obs'), True, True, True);
  end;
end;
