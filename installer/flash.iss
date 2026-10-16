; Installeur de FLASH : l'appli (Program Files\FLASH) et le plugin pour OBS Studio
; (ProgramData\obs-studio\plugins\flash-obs, qui survit à une réinstallation d'OBS).
; Compilé par « build.ps1 -Installeur », qui donne la version et les dossiers.

#ifndef Version
  #define Version "0.0.0"
#endif
#ifndef Build
  #error Build : dossier de compilation non défini
#endif
#ifndef Crt
  #error Crt : dossier du runtime Visual C++ non défini
#endif
#ifndef Deps
  #error Deps : dossier des dépendances non défini
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
PrivilegesRequired=admin
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
; La seule clé de l'utilisateur touchée est le lancement avec Windows, retiré à la désinstallation
UsedUserAreasWarning=no
VersionInfoVersion={#Version}
VersionInfoProductName=FLASH
VersionInfoDescription=Installation de FLASH

[Languages]
Name: "fr"; MessagesFile: "compiler:Languages\French.isl"

[Types]
Name: "complete"; Description: "Installation complète"
Name: "appli"; Description: "FLASH seul, sans le plugin pour OBS"
Name: "custom"; Description: "Installation personnalisée"; Flags: iscustom

[Components]
Name: "appli"; Description: "FLASH : envoyer ses écrans et ses sons, regarder un autre PC"; Types: complete appli custom; Flags: fixed
Name: "obs"; Description: "Plugin pour OBS Studio : recevoir FLASH dans OBS"; Types: complete

[Tasks]
Name: "parefeu"; Description: "Autoriser FLASH dans le pare-feu de Windows, sur les réseaux privés (nécessaire pour envoyer à un autre PC)"
Name: "bureau"; Description: "Créer une icône sur le bureau"; Flags: unchecked

[Files]
Source: "{#Build}\app\flash.exe"; DestDir: "{app}"; Components: appli; Flags: ignoreversion
Source: "{#Build}\app\srt.dll"; DestDir: "{app}"; Components: appli; Flags: ignoreversion
; Runtime Visual C++ dont a besoin srt.dll, livré à côté de l'appli
Source: "{#Crt}\msvcp140.dll"; DestDir: "{app}"; Components: appli; Flags: ignoreversion
Source: "{#Crt}\vcruntime140.dll"; DestDir: "{app}"; Components: appli; Flags: ignoreversion
Source: "{#Crt}\vcruntime140_1.dll"; DestDir: "{app}"; Components: appli; Flags: ignoreversion
Source: "{#Deps}\obs-deps\licenses\srt\LICENSE"; DestDir: "{app}\licences"; DestName: "srt.txt"; Components: appli; Flags: ignoreversion
Source: "{#Deps}\obs-deps\licenses\mbedtls\LICENSE"; DestDir: "{app}\licences"; DestName: "mbedtls.txt"; Components: appli; Flags: ignoreversion
Source: "{#Deps}\opus\opus-1.5.2\COPYING"; DestDir: "{app}\licences"; DestName: "opus.txt"; Components: appli; Flags: ignoreversion
Source: "..\third_party\monocypher\LICENCE.md"; DestDir: "{app}\licences"; DestName: "monocypher.txt"; Components: appli; Flags: ignoreversion
Source: "{#Deps}\obs-deps\licenses\nlohmann-json\LICENSE.MIT"; DestDir: "{app}\licences"; DestName: "nlohmann-json.txt"; Components: appli; Flags: ignoreversion
Source: "{#Deps}\obs-deps\licenses\nv-codec-headers\LICENSE"; DestDir: "{app}\licences"; DestName: "nv-codec-headers.txt"; Components: appli; Flags: ignoreversion
Source: "{#Deps}\webview2\LICENSE.txt"; DestDir: "{app}\licences"; DestName: "webview2.txt"; Components: appli; Flags: ignoreversion

Source: "{#Build}\obs\flash-obs.dll"; DestDir: "{#Plugin}\bin\64bit"; Components: obs; Flags: ignoreversion
Source: "..\obs\data\*"; DestDir: "{#Plugin}\data"; Components: obs; Flags: ignoreversion recursesubdirs

[Icons]
Name: "{autoprograms}\FLASH"; Filename: "{app}\flash.exe"
Name: "{autodesktop}\FLASH"; Filename: "{app}\flash.exe"; Tasks: bureau

[Registry]
; Lancement avec Windows : réglé dans l'appli, retiré avec elle
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: none; ValueName: "FLASH"; Flags: uninsdeletevalue dontcreatekey

[Run]
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall delete rule name=""FLASH"" program=""{app}\flash.exe"""; Flags: runhidden; Tasks: parefeu
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall add rule name=""FLASH"" dir=in action=allow program=""{app}\flash.exe"" enable=yes profile=private"; Flags: runhidden; Tasks: parefeu; StatusMsg: "Autorisation de FLASH dans le pare-feu..."
Filename: "{app}\flash.exe"; Description: "Ouvrir FLASH"; Flags: postinstall nowait skipifsilent runasoriginaluser

[UninstallRun]
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall delete rule name=""FLASH"" program=""{app}\flash.exe"""; Flags: runhidden; RunOnceId: "parefeu"

[Code]
const
  FlashMutex = 'Local\FLASH-appli';
  ObsMutex = 'OBSStudioCore';

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

// OBS doit être fermé pour que le plugin soit remplacé ou retiré : on le demande, sans le fermer
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

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  Result := '';
  if not FermerFlash() then
    Result := 'FLASH est encore ouvert. Quittez-le (clic droit sur son icône près de l''horloge, puis Quitter) et relancez l''installation.'
  else if WizardIsComponentSelected('obs') and not AttendreObs('installer le plugin FLASH') then
    Result := 'Installation annulée : OBS Studio est ouvert.';
end;

function InitializeUninstall(): Boolean;
begin
  Result := FermerFlash();
  if not Result then begin
    SuppressibleMsgBox('FLASH est encore ouvert. Quittez-le (clic droit sur son icône près de l''horloge, puis Quitter) et relancez la désinstallation.',
                       mbError, MB_OK, IDOK);
    Exit;
  end;
  if FileExists(ExpandConstant('{#Plugin}\bin\64bit\flash-obs.dll')) then
    Result := AttendreObs('retirer le plugin FLASH');
end;

// Réglages et appairages de FLASH et du plugin : gardés, sauf si on demande de les effacer
procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep <> usPostUninstall then
    Exit;
  if SuppressibleMsgBox('Effacer aussi les réglages de FLASH et ses appairages avec les autres PC ?',
                        mbConfirmation, MB_YESNO, IDNO) = IDYES then begin
    DelTree(ExpandConstant('{userappdata}\FLASH'), True, True, True);
    DelTree(ExpandConstant('{localappdata}\FLASH'), True, True, True);
    DelTree(ExpandConstant('{userappdata}\obs-studio\plugin_config\flash-obs'), True, True, True);
  end;
end;
