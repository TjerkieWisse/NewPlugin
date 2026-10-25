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
AppPublisher=FortyVolts
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
Name: "ai"; Description: "AI models (drum separation and drum split, 570 MB)"; Types: full plugin custom

[Files]
; The plugin and the app, without the AI runtime: that is installed once, with the models.
Source: "{#Artefacts}\VST3\KITNAP.vst3\*"; DestDir: "{commoncf64}\VST3\KITNAP.vst3"; Excludes: "onnxruntime*.dll"; Components: vst3; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#Artefacts}\Standalone\*"; DestDir: "{app}"; Excludes: "onnxruntime*.dll"; Components: standalone; Flags: ignoreversion recursesubdirs createallsubdirs
; The AI runtime and models are shared by the plugin and the app, so they're installed once.
Source: "{#Artefacts}\Standalone\Resources\onnxruntime.dll"; DestDir: "{commonappdata}\KITNAP"; Components: ai; Flags: ignoreversion
Source: "{#AIFiles}\kitnap_drums_sw.onnx"; DestDir: "{commonappdata}\KITNAP"; Components: ai; Flags: ignoreversion nocompression
Source: "{#AIFiles}\kitnap_drumsep5.onnx"; DestDir: "{commonappdata}\KITNAP"; Components: ai; Flags: ignoreversion nocompression

[InstallDelete]
; Models older versions installed, which this one no longer uses.
Type: files; Name: "{commonappdata}\KITNAP\kitnap_drums.onnx"
Type: files; Name: "{commonappdata}\KITNAP\kitnap_drumsep.onnx"
Type: files; Name: "{commoncf64}\VST3\KITNAP.vst3\Contents\Resources\kitnap_drums.onnx"
Type: files; Name: "{app}\Resources\kitnap_drums.onnx"
; The runtime older versions put inside the plugin and the app: it would be found before the
; shared one, and may be an older version.
Type: files; Name: "{commoncf64}\VST3\KITNAP.vst3\Contents\Resources\onnxruntime.dll"
Type: files; Name: "{app}\Resources\onnxruntime.dll"

[Icons]
Name: "{autoprograms}\KITNAP"; Filename: "{app}\KITNAP.exe"; Components: standalone

[Run]
Filename: "{app}\KITNAP.exe"; Description: "Open KITNAP now"; Components: standalone; Flags: nowait postinstall skipifsilent unchecked

[Messages]
FinishedLabel=KITNAP is installed.%n%nIn FL Studio, open Options > Manage plugins and click "Find installed plugins". KITNAP then shows up as an instrument (add it from the Channel rack).%n%nIn other DAWs, rescan your VST3 plugins.
