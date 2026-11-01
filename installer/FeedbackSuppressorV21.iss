; Inno Setup script for the Windows x64 installer of Feedback Suppressor V2.1.
; Build: iscc /DAppVersion=2.1.0 /DBuildDir=..\build-windows installer\FeedbackSuppressorV21.iss
#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#ifndef BuildDir
  #define BuildDir "..\build-windows"
#endif
#define Product "Feedback Suppressor V2.1"
#define Artefacts BuildDir + "\FeedbackSuppressor_artefacts\Release"

[Setup]
AppId={{3B8C6E52-1D7A-4C2F-A9E4-5F0B7D2C8A61}
AppName={#Product}
AppVersion={#AppVersion}
AppPublisher=sawada256
AppPublisherURL=https://github.com/sawada256/FeedbackSuppressor
DefaultDirName={autopf}\{#Product}
DefaultGroupName={#Product}
DisableProgramGroupPage=yes
UninstallDisplayName={#Product} {#AppVersion}
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputDir=..\dist
OutputBaseFilename=FeedbackSuppressorV2.1-{#AppVersion}-Windows-x64-Setup
Compression=lzma2
SolidCompression=yes
LicenseFile=..\LICENSE
WizardStyle=modern

[Languages]
Name: "japanese"; MessagesFile: "compiler:Languages\Japanese.isl"
Name: "english"; MessagesFile: "compiler:Default.isl"

[Components]
Name: "vst3"; Description: "VST3 plug-in"; Types: full compact custom; Flags: fixed
Name: "standalone"; Description: "Standalone application"; Types: full custom

[Files]
Source: "{#Artefacts}\VST3\{#Product}.vst3\*"; DestDir: "{commoncf64}\VST3\{#Product}.vst3"; Components: vst3; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#Artefacts}\Standalone\{#Product}.exe"; DestDir: "{app}"; Components: standalone; Flags: ignoreversion
Source: "..\README.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\LICENSE"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\THIRD_PARTY.md"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\{#Product}"; Filename: "{app}\{#Product}.exe"; Components: standalone

[UninstallDelete]
Type: filesandordirs; Name: "{commoncf64}\VST3\{#Product}.vst3"
