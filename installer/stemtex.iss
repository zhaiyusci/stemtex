#ifndef SourceDir
  #error SourceDir must be passed with /DSourceDir=...
#endif

#ifndef OutputDir
  #define OutputDir "..\dist\installer"
#endif

#ifndef AppVersion
  #define AppVersion "0.1.0"
#endif

[Setup]
AppId={{7B625C5A-3D8E-48D8-B65A-5357444E8C62}
AppName=StemTeX
AppVersion={#AppVersion}
AppPublisher=StemTeX
DefaultDirName=C:\StemTeX
DisableDirPage=no
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin
OutputDir={#OutputDir}
OutputBaseFilename=StemTeX-{#AppVersion}-Setup
Compression=lzma2/ultra64
SolidCompression=yes
WizardStyle=modern
UninstallDisplayName=StemTeX

[Files]
Source: "{#SourceDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs; Excludes: "texmf-var\fonts\cache\*;texmf-var\cache-warmup\*"

[Run]
Filename: "{sys}\WindowsPowerShell\v1.0\powershell.exe"; Parameters: "-NoProfile -ExecutionPolicy Bypass -File ""{app}\refresh-font-cache.ps1"" -Clean -WarmupTex ""{app}\cache-warmup\warmup.tex"""; StatusMsg: "Building StemTeX font cache..."; Flags: waituntilterminated

[UninstallDelete]
Type: filesandordirs; Name: "{app}\texmf-var\fonts\cache"
Type: filesandordirs; Name: "{app}\texmf-var\cache-warmup"
