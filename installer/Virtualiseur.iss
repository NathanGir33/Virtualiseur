; Installateur de Virtualiseur (Inno Setup 6).
; Compilation : .\build.ps1 -Installer  ->  installer\Output\Virtualiseur-Setup.exe
;
; Installe dist\ dans Program Files (runtime Visual C++ compris), crée les
; raccourcis, active au besoin la fonctionnalité Windows « Plateforme de
; l'hyperviseur Windows » et enregistre le désinstallateur dans les
; Paramètres de Windows. Les machines virtuelles restent dans
; Documents\Virtualiseur\VMs : la désinstallation propose de les supprimer.

#define AppName "Virtualiseur"
#define AppVersion "0.2.0"
#define AppExe "VirtualiseurManager.exe"

[Setup]
AppId={{E1D571A7-866A-4604-BA1E-BB4B5E8FC00C}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher={#AppName}
AppComments=Hyperviseur pour Windows construit sur Windows Hypervisor Platform
VersionInfoVersion={#AppVersion}
VersionInfoDescription=Installation de {#AppName}
DefaultDirName={autopf}\{#AppName}
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
; Program Files et l'activation de la fonctionnalité Windows demandent l'administrateur.
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
; Windows 10 version 2004 : WHP complet, y compris sur processeurs AMD.
MinVersion=10.0.19041
OutputDir=Output
OutputBaseFilename=Virtualiseur-Setup
SetupIconFile=..\src\app\VirtualiseurManager\Virtualiseur.ico
UninstallDisplayIcon={app}\{#AppExe}
UninstallDisplayName={#AppName}
Compression=lzma2/ultra64
SolidCompression=yes
WizardStyle=modern
; Ferme le gestionnaire et les VM en cours avant de remplacer ou supprimer les fichiers.
CloseApplications=yes
RestartApplications=no
InfoAfterFile=apres-installation.txt

[Languages]
Name: "french"; MessagesFile: "compiler:Languages\French.isl"

[Tasks]
Name: "desktopicon"; Description: "Créer un raccourci sur le Bureau"; GroupDescription: "Raccourcis :"
Name: "enablewhp"; Description: "Activer la Plateforme de l'hyperviseur Windows (indispensable, redémarrage requis)"; GroupDescription: "Windows :"; Check: not WhpEnabled

[Files]
Source: "..\dist\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "..\README.md"; DestDir: "{app}\docs"; Flags: ignoreversion
Source: "..\docs\run-linux-v1.md"; DestDir: "{app}\docs"; DestName: "guide.md"; Flags: ignoreversion

[Icons]
Name: "{group}\{#AppName}"; Filename: "{app}\{#AppExe}"; Comment: "Gestionnaire de machines virtuelles"
Name: "{group}\Guide d'utilisation"; Filename: "{app}\docs\guide.md"
Name: "{group}\Désinstaller {#AppName}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#AppExe}"; Tasks: desktopicon

[Run]
Filename: "{sys}\dism.exe"; Parameters: "/online /enable-feature /featurename:HypervisorPlatform /all /norestart /quiet"; StatusMsg: "Activation de la Plateforme de l'hyperviseur Windows (cela peut prendre une minute)..."; Flags: runhidden waituntilterminated; Tasks: enablewhp
Filename: "{app}\{#AppExe}"; Description: "Lancer {#AppName}"; Flags: nowait postinstall skipifsilent; Check: not NeedRestart

[UninstallDelete]
; Fichiers créés après l'installation dans le dossier du programme.
Type: filesandordirs; Name: "{app}"

[Code]
var
  WhpChecked: Boolean;
  WhpState: Boolean;

{ État de la fonctionnalité Windows « HypervisorPlatform » (mis en cache). }
function WhpEnabled(): Boolean;
var
  ResultCode: Integer;
begin
  if not WhpChecked then
  begin
    WhpState := Exec(ExpandConstant('{sys}\WindowsPowerShell\v1.0\powershell.exe'),
      '-NoProfile -NonInteractive -Command "if ((Get-WindowsOptionalFeature -Online -FeatureName HypervisorPlatform).State -eq ''Enabled'') { exit 0 } else { exit 1 }"',
      '', SW_HIDE, ewWaitUntilTerminated, ResultCode) and (ResultCode = 0);
    WhpChecked := True;
  end;
  Result := WhpState;
end;

{ Après l'activation de la fonctionnalité, Windows doit redémarrer. }
function NeedRestart(): Boolean;
begin
  Result := WizardIsTaskSelected('enablewhp');
end;

{ Désinstallation : propose de supprimer aussi les machines virtuelles. }
procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  Library, Root: String;
begin
  if CurUninstallStep <> usPostUninstall then
    Exit;
  Root := ExpandConstant('{userdocs}\Virtualiseur');
  Library := Root + '\VMs';
  if UninstallSilent() or not DirExists(Library) then
    Exit;
  if MsgBox('Supprimer aussi vos machines virtuelles ?' + #13#10#13#10 +
            'Le dossier suivant et tous les disques virtuels qu''il contient seront effacés définitivement :' + #13#10 +
            Library + #13#10#13#10 +
            'Choisissez « Non » pour les garder (une réinstallation les retrouvera).',
            mbConfirmation, MB_YESNO or MB_DEFBUTTON2) = IDYES then
  begin
    DelTree(Library, True, True, True);
    DeleteFile(Root + '\manager.ini');
    { Le dossier Virtualiseur n'est supprimé que s'il est vide. }
    RemoveDir(Root);
  end;
end;
