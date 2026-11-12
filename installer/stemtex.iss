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
; Keep the historical full/compact IDs so upgrades retain the original XeTeX choices.
Name: "full"; Description: "XeTeX with GUI and bundled TeX tree"
Name: "lua"; Description: "LuaTeX with GUI and bundled TeX tree"
Name: "both"; Description: "XeTeX and LuaTeX with GUI"
Name: "compact"; Description: "XeTeX runtime only"
Name: "custom"; Description: "Custom installation"; Flags: iscustom

#define BundledTreeDescription "Bundled macros and fonts for the selected engines"
[Components]
Name: "runtime"; Description: "Shared converters and SDK"; Types: full lua both compact custom; Flags: fixed
Name: "xetex"; Description: "XeTeX engine"; Types: full both compact custom; Flags: disablenouninstallwarning
Name: "luatex"; Description: "LuaTeX engine (LuaHBTeX)"; Types: lua both; Flags: disablenouninstallwarning
Name: "gui"; Description: "Renderer GUI and Profile Creator"; Types: full lua both custom
Name: "texmf"; Description: "{#BundledTreeDescription}"; Types: full lua both custom

[Files]
Source: "{#SourceDir}\runtime\bin\windows\dvipdfmxdaemon.dll"; DestDir: "{app}\runtime\bin\windows"; Components: runtime; Flags: ignoreversion
Source: "{#SourceDir}\runtime\bin\windows\xdvipdfmxdaemon.exe"; DestDir: "{app}\runtime\bin\windows"; Components: runtime; Flags: ignoreversion
Source: "{#SourceDir}\runtime\bin\windows\dvisvgmdaemon.dll"; DestDir: "{app}\runtime\bin\windows"; Components: runtime; Flags: ignoreversion
Source: "{#SourceDir}\runtime\bin\windows\dvisvgmdaemon.exe"; DestDir: "{app}\runtime\bin\windows"; Components: runtime; Flags: ignoreversion
Source: "{#SourceDir}\runtime\bin\sdk\*"; DestDir: "{app}\runtime\bin\sdk"; Components: runtime; Flags: ignoreversion
Source: "{#SourceDir}\runtime\sdk\*"; DestDir: "{app}\runtime\sdk"; Components: runtime; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#SourceDir}\runtime\VERSION"; DestDir: "{app}\runtime"; Components: runtime; Flags: ignoreversion
Source: "{#SourceDir}\runtime\StemTeX.ico"; DestDir: "{app}\runtime"; Components: runtime; Flags: ignoreversion
Source: "{#SourceDir}\runtime\bin\windows\xetexdaemon.exe"; DestDir: "{app}\runtime\bin\windows"; Components: xetex; Flags: ignoreversion
Source: "{#SourceDir}\runtime\bin\windows\xetexdaemon.dll"; DestDir: "{app}\runtime\bin\windows"; Components: xetex; Flags: ignoreversion
Source: "{#SourceDir}\runtime\bin\windows\stemtex-worker-host.exe"; DestDir: "{app}\runtime\bin\windows"; Components: xetex; Flags: ignoreversion
Source: "{#SourceDir}\runtime\bin\windows\icu-data\icudt78l.dat"; DestDir: "{app}\runtime\bin\windows\icu-data"; Components: xetex; Flags: ignoreversion
Source: "{#SourceDir}\runtime\run-xelatexdaemon.bat"; DestDir: "{app}\runtime"; Components: xetex; Flags: ignoreversion
Source: "{#SourceDir}\runtime\refresh-profile-cache.bat"; DestDir: "{app}\runtime"; Components: xetex; Flags: ignoreversion
#define XeFormat SourceDir + "\runtime\texmf-var\web2c\xetex\xelatexdaemon.fmt"
Source: "{#XeFormat}"; DestDir: "{app}\runtime\texmf-var\web2c\xetex"; Components: xetex and texmf; Flags: ignoreversion skipifsourcedoesntexist
Source: "{#SourceDir}\runtime\texmf-var\fonts\conf\*"; DestDir: "{app}\runtime\texmf-var\fonts\conf"; Components: xetex; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#SourceDir}\runtime\bin\luatex\luahbtex.exe"; DestDir: "{app}\runtime\bin\luatex"; Components: luatex; Flags: ignoreversion
Source: "{#SourceDir}\runtime\bin\luatex\luahbtex.exe.stemtex-checkpoint"; DestDir: "{app}\runtime\bin\luatex"; Components: luatex; Flags: ignoreversion
Source: "{#SourceDir}\runtime\bin\luatex\build-manifest.json"; DestDir: "{app}\runtime\bin\luatex"; Components: luatex; Flags: ignoreversion
#define LuaBin SourceDir + "\runtime\bin\luatex"
#define LuaDllHandle FindFirst(LuaBin + "\*.dll", faAnyFile & ~faDirectory)
#define LuaDllResult LuaDllHandle
#if !LuaDllResult
  #error Missing LuaHBTeX runtime DLLs in the package stage
#endif
#sub LuaDllFile
  #define private DllName FindGetFileName(LuaDllHandle)
[Files]
Source: "{#LuaBin}\{#DllName}"; DestDir: "{app}\runtime\bin\luatex"; Components: luatex; Flags: ignoreversion
[InstallDelete]
Type: files; Name: "{app}\runtime\bin\luatex\{#DllName}"; Components: not luatex; Check: IsUpgradeInThisDirectory
#endsub
#for {LuaDllResult = LuaDllHandle; LuaDllResult; LuaDllResult = FindNext(LuaDllHandle)} LuaDllFile
#expr FindClose(LuaDllHandle)

[Files]
#define public LuaDataRoot "runtime\luatex"
#if !FileExists(SourceDir + "\" + LuaDataRoot + "\tree-id.txt") || !FileExists(SourceDir + "\" + LuaDataRoot + "\texmf-dist\tex\luatex\luaotfload\luaotfload-main.lua")
  #error Missing bundled LuaTeX default profile data in the package stage
#endif
Source: "{#SourceDir}\{#LuaDataRoot}\*"; DestDir: "{app}\{#LuaDataRoot}"; Components: luatex and texmf; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#SourceDir}\runtime\texmf-dist\*"; DestDir: "{app}\runtime\texmf-dist"; Flags: ignoreversion recursesubdirs createallsubdirs; Components: xetex and texmf
#define DefaultProfile SourceDir + "\gui\profiles\unicodemath"
Source: "{#DefaultProfile}\preamble.tex"; DestDir: "{app}\gui\profiles\unicodemath"; Components: texmf or gui; Flags: ignoreversion
Source: "{#DefaultProfile}\warmup.tex"; DestDir: "{app}\gui\profiles\unicodemath"; Components: texmf or gui; Flags: ignoreversion
Source: "{#SourceDir}\gui\*"; DestDir: "{app}\gui"; Flags: ignoreversion recursesubdirs createallsubdirs; Components: gui; Excludes: "profiles\unicodemath\preamble.tex,profiles\unicodemath\warmup.tex,build\*,*.aux,*.log,*.pdf,*.xdv,*.synctex.gz"

[Icons]
Name: "{autoprograms}\StemTeX Renderer GUI"; Filename: "{app}\gui\stemtex-renderer-gui.exe"; WorkingDir: "{app}\gui"; Components: gui
Name: "{autoprograms}\StemTeX Profile Creator"; Filename: "{app}\gui\stemtex-profile-creator.exe"; WorkingDir: "{app}\gui"; Components: gui

[Run]
Filename: "{app}\runtime\refresh-profile-cache.bat"; Parameters: """{app}\gui\profiles\unicodemath"""; WorkingDir: "{app}\runtime"; Flags: runhidden waituntilterminated; StatusMsg: "Preparing StemTeX font and profile cache..."; Components: xetex and gui and texmf

[InstallDelete]
; Inno retains files from deselected components on upgrade. Remove the engine's
; packaged files individually and preserve other files in these directories.
Type: files; Name: "{app}\runtime\bin\windows\xetexdaemon.exe"; Components: not xetex; Check: IsUpgradeInThisDirectory
Type: files; Name: "{app}\runtime\bin\windows\xetexdaemon.dll"; Components: not xetex; Check: IsUpgradeInThisDirectory
Type: files; Name: "{app}\runtime\bin\windows\stemtex-worker-host.exe"; Components: not xetex; Check: IsUpgradeInThisDirectory
Type: files; Name: "{app}\runtime\bin\windows\icu-data\icudt78l.dat"; Components: not xetex; Check: IsUpgradeInThisDirectory
Type: files; Name: "{app}\runtime\run-xelatexdaemon.bat"; Components: not xetex; Check: IsUpgradeInThisDirectory
Type: files; Name: "{app}\runtime\refresh-profile-cache.bat"; Components: not xetex; Check: IsUpgradeInThisDirectory
Type: files; Name: "{app}\runtime\texmf-var\web2c\xetex\xelatexdaemon.fmt"; Components: not (xetex and texmf); Check: IsUpgradeInThisDirectory
Type: files; Name: "{app}\runtime\texmf-var\fonts\conf\fonts.conf"; Components: not xetex; Check: IsUpgradeInThisDirectory
Type: files; Name: "{app}\runtime\texmf-var\fonts\conf\conf.d\51-local.conf"; Components: not xetex; Check: IsUpgradeInThisDirectory
Type: files; Name: "{app}\runtime\bin\luatex\luahbtex.exe"; Components: not luatex; Check: IsUpgradeInThisDirectory
Type: files; Name: "{app}\runtime\bin\luatex\luahbtex.exe.stemtex-checkpoint"; Components: not luatex; Check: IsUpgradeInThisDirectory
Type: files; Name: "{app}\runtime\bin\luatex\build-manifest.json"; Components: not luatex; Check: IsUpgradeInThisDirectory
; At build time, list the individual files shipped in each bundled TeX tree. An
; in-place upgrade can then drop that component without deleting local additions.
#define public TreeRoot LuaDataRoot
#define public TreeSelection "luatex and texmf"
#define public TreePath TreeRoot
#define public TreeName ""
#define public TreeBytes 0
#sub RemoveTreeEntry
  #if TreeName != "." && TreeName != ".."
    #define private RelativePath TreePath
    #define private InstallPath StringChange(RelativePath, "{", "{{")
    #if DirExists(SourceDir + "\" + RelativePath)
      #expr ListTreeDirectory
      Type: dirifempty; Name: "{app}\{#InstallPath}"; Components: not ({#TreeSelection}); Check: IsUpgradeInThisDirectory
    #else
      #expr TreeBytes = TreeBytes + FileSize(SourceDir + "\" + RelativePath)
      Type: files; Name: "{app}\{#InstallPath}"; Components: not ({#TreeSelection}); Check: IsUpgradeInThisDirectory
    #endif
  #endif
#endsub
#sub ListTreeDirectory
  #define private ParentPath TreePath
  #define private SearchHandle FindFirst(SourceDir + "\" + ParentPath + "\*", faAnyFile)
  #define private SearchResult
  #for {SearchResult = SearchHandle; SearchResult; SearchResult = FindNext(SearchHandle)} \
    TreeName = FindGetFileName(SearchHandle), TreePath = ParentPath + "\" + TreeName, RemoveTreeEntry
  #if SearchHandle
    #expr FindClose(SearchHandle)
  #endif
#endsub
#expr ListTreeDirectory
#define LuaTreeBytes TreeBytes
Type: dirifempty; Name: "{app}\{#TreeRoot}"; Components: not ({#TreeSelection}); Check: IsUpgradeInThisDirectory
#expr TreeRoot = "runtime\texmf-dist", TreePath = TreeRoot, TreeSelection = "xetex and texmf"
#expr TreeBytes = FileExists(XeFormat) ? FileSize(XeFormat) : 0
#expr ListTreeDirectory
#define XeTreeBytes TreeBytes
Type: dirifempty; Name: "{app}\{#TreeRoot}"; Components: not ({#TreeSelection}); Check: IsUpgradeInThisDirectory
Type: files; Name: "{app}\gui\profiles\*\warmup.xdv"
Type: files; Name: "{app}\gui\profiles\*\warmup.aux"
Type: files; Name: "{app}\gui\profiles\*\warmup.log"
Type: files; Name: "{app}\gui\profiles\*\warmup.pdf"
Type: files; Name: "{app}\gui\profiles\*\warmup.synctex.gz"

[UninstallDelete]
Type: filesandordirs; Name: "{app}\runtime\texmf-var\fonts\cache"
Type: files; Name: "{app}\gui\profiles\*\warmup.xdv"
Type: files; Name: "{app}\gui\profiles\*\warmup.aux"
Type: files; Name: "{app}\gui\profiles\*\warmup.log"

[Code]
function IsUpgradeInThisDirectory: Boolean;
var
  PreviousDir: String;
begin
  Result := RegQueryStringValue(HKA,
    'Software\Microsoft\Windows\CurrentVersion\Uninstall\' +
    ExpandConstant('{#SetupSetting("AppId")}') + '_is1', 'InstallLocation', PreviousDir)
    and (CompareText(RemoveBackslashUnlessRoot(PreviousDir),
                     RemoveBackslashUnlessRoot(ExpandConstant('{app}'))) = 0);
end;

const
  SelectEngineMessage = 'Select at least one engine (XeTeX or LuaTeX) to install.';

var
  TypeChanged, ComponentChecked: TNotifyEvent;

procedure UpdateBundledTreeSize;
var
  Bytes: Int64;
  Row: Integer;
  SizeText: String;
begin
  Bytes := 0;
  if WizardIsComponentSelected('xetex') then
    Bytes := Bytes + {#XeTreeBytes};
  if WizardIsComponentSelected('luatex') then
    Bytes := Bytes + {#LuaTreeBytes};
  { The GUI row already includes the two shared profile files. }
  if not WizardIsComponentSelected('gui') then
    Bytes := Bytes + {#FileSize(DefaultProfile + "\preamble.tex") + FileSize(DefaultProfile + "\warmup.tex")};
  Row := WizardForm.ComponentsList.Items.IndexOf('{#BundledTreeDescription}');
  SizeText := Format('%.1f', [((Bytes * 10 + 1048575) div 1048576) / 10.0]);
  WizardForm.ComponentsList.ItemSubItem[Row] := FmtMessage(SetupMessage(msgComponentSize2), [SizeText]);
end;

procedure SelectionChanged(Sender: TObject);
begin
  if Sender = WizardForm.TypesCombo then
    TypeChanged(Sender)
  else
    ComponentChecked(Sender);
  UpdateBundledTreeSize;
end;

procedure InitializeWizard;
begin
  TypeChanged := WizardForm.TypesCombo.OnChange;
  ComponentChecked := WizardForm.ComponentsList.OnClickCheck;
  WizardForm.TypesCombo.OnChange := @SelectionChanged;
  WizardForm.ComponentsList.OnClickCheck := @SelectionChanged;
end;

procedure CurPageChanged(CurPageID: Integer);
begin
  if CurPageID = wpSelectComponents then
    UpdateBundledTreeSize;
end;

function NextButtonClick(CurPageID: Integer): Boolean;
begin
  Result := True;
  if (CurPageID = wpSelectComponents) and not WizardSilent then begin
    Result := WizardIsComponentSelected('xetex or luatex');
    if not Result then
      MsgBox(SelectEngineMessage, mbInformation, MB_OK);
  end;
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  if not WizardIsComponentSelected('xetex or luatex') then
    Result := SelectEngineMessage;
end;
