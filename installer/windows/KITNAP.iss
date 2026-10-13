; Windows installer for KITNAP (Inno Setup 6).
; Built by CI:  iscc /DAppVersion=0.2.0 /DArtefacts=<build>\KITNAP_artefacts\Release /DAIFiles=<build>\kitnap-ai
;                installer\windows\KITNAP.iss

#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#ifndef Artefacts
  #define Artefacts "..\..\build\KITNAP_artefacts\Release"
#endif
#ifndef AIFiles
  #define AIFiles "..\..\build\kitnap-ai"
#endif

[Setup]
AppId={{BF9914D3-188B-44CD-9A26-EBCFC117A0C5}
AppName=KITNAP
AppVersion={#AppVersion}
AppVerName=KITNAP {#AppVersion}
AppPublisher=Kitnap Audio
DefaultDirName={autopf}\KITNAP
DisableDirPage=yes
DisableProgramGroupPage=yes
OutputDir=..\..\build\installer
OutputBaseFilename=KITNAP-{#AppVersion}-Windows-Setup
Compression=lzma2/ultra64
SolidCompression=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin
WizardStyle=modern
UninstallDisplayName=KITNAP
UninstallDisplayIcon={app}\KITNAP.exe
CloseApplications=yes

[Types]
Name: "full"; Description: "Plugin and standalone app"
Name: "plugin"; Description: "Plugin only"
Name: "custom"; Description: "Custom"; Flags: iscustom

[Components]
Name: "vst3"; Description: "VST3 plugin (FL Studio, Ableton Live, Cubase, Studio One, Reaper, ...)"; Types: full plugin custom
Name: "standalone"; Description: "Standalone app"; Types: full custom
Name: "ai"; Description: "AI models (drum separation and drum split, 650 MB)"; Types: full plugin custom

[Files]
; The whole KITNAP.vst3 folder, including the AI runtime and model in Contents\Resources.
Source: "{#Artefacts}\VST3\KITNAP.vst3\*"; DestDir: "{commoncf64}\VST3\KITNAP.vst3"; Components: vst3; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#Artefacts}\Standalone\*"; DestDir: "{app}"; Components: standalone; Flags: ignoreversion recursesubdirs createallsubdirs
; The AI models are shared by the plugin and the app, so they're installed once.
Source: "{#AIFiles}\kitnap_drums_sw.onnx"; DestDir: "{commonappdata}\KITNAP"; Components: ai; Flags: ignoreversion nocompression
Source: "{#AIFiles}\kitnap_drumsep.onnx"; DestDir: "{commonappdata}\KITNAP"; Components: ai; Flags: ignoreversion nocompression

[Icons]
Name: "{autoprograms}\KITNAP"; Filename: "{app}\KITNAP.exe"; Components: standalone

[Run]
Filename: "{app}\KITNAP.exe"; Description: "Open KITNAP now"; Components: standalone; Flags: nowait postinstall skipifsilent unchecked

[Messages]
FinishedLabel=KITNAP is installed.%n%nIn FL Studio, open Options > Manage plugins and click "Find installed plugins". KITNAP then shows up as an instrument (add it from the Channel rack).%n%nIn other DAWs, rescan your VST3 plugins.
