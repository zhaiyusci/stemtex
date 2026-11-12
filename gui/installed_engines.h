#pragma once

#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>

namespace stemtex {

struct InstalledEngines {
  bool xetex;
  bool luahbtex;
};

inline InstalledEngines installedEngines(const QString &runtimeRoot, const QString &luaEnginePath = {}) {
  const QDir runtime(runtimeRoot);
  return {QFileInfo(runtime.filePath("bin/windows/xetexdaemon.exe")).isFile(),
          QFileInfo(luaEnginePath.isEmpty() ? runtime.filePath("bin/luatex/luahbtex.exe") : luaEnginePath).isFile()};
}

inline constexpr const char *luaLatexTreeFiles[] = {
    "texmf-dist/web2c/texmf.cnf", "texmf-dist/tex/latex/tex-ini-files/lualatex.ini",
    "texmf-dist/tex/luatex/luaotfload/luaotfload-main.lua"};

inline bool hasLuaLatexTree(const QString &root) {
  if (root.isEmpty()) return false;
  for (const char *file : luaLatexTreeFiles)
    if (!QFileInfo(QDir(root).filePath(file)).isFile()) return false;
  return true;
}

inline QString bundledLuaTexmfRoot(const QString &runtimeRoot) {
  return QDir::cleanPath(QDir(runtimeRoot).absoluteFilePath("luatex"));
}

inline QString findLuaTexmfRoot(const QString &runtimeRoot, const QString &explicitRoot) {
  if (!explicitRoot.isEmpty()) return QDir::cleanPath(QDir(explicitRoot).absolutePath());
  const QString bundled = bundledLuaTexmfRoot(runtimeRoot);
  if (installedEngines(runtimeRoot).luahbtex || QDir(bundled).exists()) return bundled;
  // A developer-supplied engine can run without a packaged runtime.
  QStringList candidates{qEnvironmentVariable("TEXMFROOT")};
  const QString lualatex = QStandardPaths::findExecutable("lualatex.exe");
  if (!lualatex.isEmpty()) candidates << QDir(QFileInfo(lualatex).absolutePath()).filePath("../..");
  QDir texlive("C:/texlive");
  for (const QFileInfo &entry : texlive.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name | QDir::Reversed))
    candidates << entry.absoluteFilePath();
  for (const QString &candidate : candidates)
    if (hasLuaLatexTree(candidate)) return QDir::cleanPath(QDir(candidate).absolutePath());
  return bundled;
}

}  // namespace stemtex
