; Chordbloom installer (Inno Setup 6)
; Build: "%LOCALAPPDATA%\Programs\Inno Setup 6\ISCC.exe" installer\Chordbloom.iss  -> dist\Chordbloom-Setup-<version>.exe
; Payload: installer\payload\{VST3\Chordbloom.vst3, CLAP\Chordbloom.clap, Standalone\Chordbloom.exe}
;          built with the static MSVC runtime (-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded), so no VC++ redistributable needed.

#define AppName "Chordbloom"
#define AppVersion "1.0.0"
#define AppPublisher "Kujar3"

[Setup]
AppId={{6B1E3C2A-8F4D-4C1B-9E7A-2D5F0C9B7A41}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher={#AppPublisher}
DefaultDirName={autopf}\{#AppName}
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputDir=..\dist
OutputBaseFilename=Chordbloom-Setup-{#AppVersion}
SetupIconFile=chordbloom.ico
UninstallDisplayIcon={app}\Chordbloom.exe
UninstallDisplayName={#AppName} {#AppVersion}
InfoAfterFile=QuickStart.txt
WizardStyle=modern
Compression=lzma2/max
SolidCompression=yes
CloseApplications=yes

[Messages]
WelcomeLabel2=This will install [name/ver] - a chord progression plugin for FL Studio and other DAWs.%n%nPlease close FL Studio before continuing.

[Types]
Name: "full"; Description: "Full installation (recommended)"
Name: "custom"; Description: "Custom installation"; Flags: iscustom

[Components]
Name: "vst3"; Description: "VST3 plugin (use this one in FL Studio)"; Types: full custom; Flags: fixed
Name: "clap"; Description: "CLAP plugin"; Types: full
Name: "standalone"; Description: "Standalone app (play without a DAW)"; Types: full

[Files]
Source: "payload\VST3\Chordbloom.vst3\*"; DestDir: "{commoncf64}\VST3\Chordbloom.vst3"; Components: vst3; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "payload\CLAP\Chordbloom.clap"; DestDir: "{commoncf64}\CLAP"; Components: clap; Flags: ignoreversion
Source: "payload\Standalone\Chordbloom.exe"; DestDir: "{app}"; Components: standalone; Flags: ignoreversion
Source: "QuickStart.txt"; DestDir: "{app}"; Flags: ignoreversion

[InstallDelete]
Type: filesandordirs; Name: "{commoncf64}\VST3\Chordbloom.vst3"

[UninstallDelete]
Type: dirifempty; Name: "{commoncf64}\VST3\Chordbloom.vst3"

[Icons]
Name: "{group}\Chordbloom"; Filename: "{app}\Chordbloom.exe"; Components: standalone
Name: "{group}\Chordbloom Quick Start"; Filename: "{app}\QuickStart.txt"
Name: "{group}\Uninstall Chordbloom"; Filename: "{uninstallexe}"
