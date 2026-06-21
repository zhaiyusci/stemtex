#ifndef SourceDir
  #error SourceDir must be passed with /DSourceDir=...
#endif

#ifndef OutputDir
  #define OutputDir "..\dist\installer"
#endif

#ifndef AppVersion
  #error AppVersion must be passed with /DAppVersion=...
#endif

[Setup]
AppId={{7B625C5A-3D8E-48D8-B65A-5357444E8C62}
AppName=StemTeX
AppVersion={#AppVersion}
AppPublisher=StemTeX
DefaultDirName=C:\StemTeX
DisableDirPage=no
SetupIconFile={#SourceDir}\runtime\StemTeX.ico
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin
OutputDir={#OutputDir}
OutputBaseFilename=StemTeX-{#AppVersion}-Setup
Compression=lzma2/ultra64
SolidCompression=yes
WizardStyle=modern
UninstallDisplayName=StemTeX
UninstallDisplayIcon={app}\runtime\StemTeX.ico

[Types]
Name: "full"; Description: "Full installation"
Name: "compact"; Description: "Runtime only"
Name: "custom"; Description: "Custom installation"; Flags: iscustom

[Components]
Name: "runtime"; Description: "StemTeX daemon runtime and C API"; Types: full compact custom; Flags: fixed
Name: "gui"; Description: "StemTeX Renderer GUI and profiles"; Types: full custom
Name: "texmf"; Description: "Bundled TeX macro and font tree"; Types: full custom

[Files]
Source: "{#SourceDir}\runtime\bin\*"; DestDir: "{app}\runtime\bin"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#SourceDir}\runtime\sdk\*"; DestDir: "{app}\runtime\sdk"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#SourceDir}\runtime\VERSION"; DestDir: "{app}\runtime"; Flags: ignoreversion
Source: "{#SourceDir}\runtime\StemTeX.ico"; DestDir: "{app}\runtime"; Flags: ignoreversion
Source: "{#SourceDir}\runtime\worker-template.tex"; DestDir: "{app}\runtime"; Flags: ignoreversion
Source: "{#SourceDir}\runtime\run-xelatexdaemon.bat"; DestDir: "{app}\runtime"; Flags: ignoreversion
Source: "{#SourceDir}\runtime\refresh-profile-cache.bat"; DestDir: "{app}\runtime"; Flags: ignoreversion
Source: "{#SourceDir}\runtime\texmf-var\web2c\*"; DestDir: "{app}\runtime\texmf-var\web2c"; Flags: ignoreversion recursesubdirs createallsubdirs; Excludes: "*.aux;*.log"
Source: "{#SourceDir}\runtime\texmf-var\fonts\conf\*"; DestDir: "{app}\runtime\texmf-var\fonts\conf"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#SourceDir}\runtime\texmf-dist\*"; DestDir: "{app}\runtime\texmf-dist"; Flags: ignoreversion recursesubdirs createallsubdirs; Components: texmf
Source: "{#SourceDir}\gui\*"; DestDir: "{app}\gui"; Flags: ignoreversion recursesubdirs createallsubdirs; Components: gui; Excludes: "*.aux;*.log;*.pdf;*.xdv;*.synctex.gz"

[Icons]
Name: "{autoprograms}\StemTeX Renderer GUI"; Filename: "{app}\gui\stemtex-renderer-gui.exe"; WorkingDir: "{app}\gui"; Components: gui

[Run]
Filename: "{app}\runtime\refresh-profile-cache.bat"; Parameters: """{app}\gui\profiles\chemistry"""; WorkingDir: "{app}\runtime"; Flags: runhidden waituntilterminated; StatusMsg: "Preparing StemTeX font and profile cache..."; Check: ShouldRunInstallWarmup

[UninstallDelete]
Type: filesandordirs; Name: "{app}\runtime\texmf-var\fonts\cache"
Type: filesandordirs; Name: "{app}\runtime\texmf-var\cache-warmup"
Type: files; Name: "{app}\gui\profiles\*\warmup.xdv"
Type: files; Name: "{app}\gui\profiles\*\warmup.aux"
Type: files; Name: "{app}\gui\profiles\*\warmup.log"

[Code]
function ShouldRunInstallWarmup: Boolean;
begin
  Result := WizardIsComponentSelected('gui') and WizardIsComponentSelected('texmf');
end;
