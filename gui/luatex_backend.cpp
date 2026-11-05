#include "luatex_backend.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QMap>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUuid>

#include <atomic>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>
#define NOMINMAX
#include <windows.h>

namespace stemtex {
namespace {

[[noreturn]] void fail(const QString &message) {
  throw std::runtime_error(message.toUtf8().constData());
}

QString pathIn(const QString &root, const QString &name) {
  return QDir::cleanPath(QDir(root).absoluteFilePath(name));
}

QByteArray readFile(const QString &path) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) fail(QString("Cannot read %1: %2").arg(path, file.errorString()));
  return file.readAll();
}

void writeFile(const QString &path, const QByteArray &bytes) {
  QSaveFile file(path);
  if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
    fail(QString("Cannot write %1: %2").arg(path, file.errorString()));
}

quint32 uintBe(const QByteArray &data, qsizetype pos, int count) {
  quint32 value = 0;
  for (int i = 0; i < count; ++i) value = (value << 8) | static_cast<quint8>(data.at(pos + i));
  return value;
}

void appendBe(QByteArray &data, quint32 value, int count) {
  for (int i = count - 1; i >= 0; --i) data.append(static_cast<char>(value >> (8 * i)));
}

struct DviSnapshot {
  QByteArray finalized;
  int pages = 0;
};

// Parse instructions, never scan for opcode-valued bytes inside glyph names or
// specials. A flushed live stream must end on EOP, before any final postamble.
DviSnapshot finalizeDvi(const QByteArray &data) {
  if (data.size() < 15 || static_cast<quint8>(data[0]) != 247 || static_cast<quint8>(data[1]) != 2)
    fail("LuaHBTeX did not produce standard DVI (format ID 2).");
  qsizetype pos = 0, lastBop = -1, lastEnd = -1;
  int pages = 0, depth = 0, maxDepth = 0;
  bool inPage = false;
  QMap<quint32, QByteArray> fonts;
  while (pos < data.size()) {
    const qsizetype start = pos;
    const int op = static_cast<quint8>(data[pos++]);
    quint64 extra = 0;
    if (op <= 127 || op == 138 || (op >= 140 && op <= 142) || op == 147 || op == 152 || op == 161 ||
        op == 166 || (op >= 171 && op <= 234)) {}
    else if (op >= 128 && op <= 131) extra = op - 127;
    else if (op == 132 || op == 137) extra = 8;
    else if (op >= 133 && op <= 136) extra = op - 132;
    else if (op == 139) extra = 44;
    else if (op >= 143 && op <= 146) extra = op - 142;
    else if (op >= 148 && op <= 151) extra = op - 147;
    else if (op >= 153 && op <= 156) extra = op - 152;
    else if (op >= 157 && op <= 160) extra = op - 156;
    else if (op >= 162 && op <= 165) extra = op - 161;
    else if (op >= 167 && op <= 170) extra = op - 166;
    else if (op >= 235 && op <= 238) extra = op - 234;
    else if (op >= 239 && op <= 242) {
      const int count = op - 238;
      if (pos + count > data.size()) fail("Incomplete DVI special length.");
      extra = quint64(count) + uintBe(data, pos, count);
    } else if (op >= 243 && op <= 246) {
      const int count = op - 242;
      if (pos + count + 14 > data.size()) fail("Incomplete DVI font definition.");
      extra = count + 14 + static_cast<quint8>(data[pos + count + 12]) +
              static_cast<quint8>(data[pos + count + 13]);
    } else if (op == 247) {
      if (start != 0 || pos + 14 > data.size()) fail("Invalid DVI preamble.");
      extra = 14 + static_cast<quint8>(data[pos + 13]);
    } else if (op == 255) extra = 1;
    else fail(QString("Unexpected live DVI opcode %1 at %2.").arg(op).arg(start));
    if (extra > quint64(data.size() - pos)) fail("DVI page has not been completely flushed.");
    pos += static_cast<qsizetype>(extra);
    if (op == 139) {
      if (inPage || qint32(uintBe(data, start + 41, 4)) != lastBop) fail("Invalid DVI page pointer chain.");
      inPage = true;
      lastBop = start;
      depth = 0;
    } else if (op == 141) {
      if (!inPage) fail("DVI stack outside page.");
      maxDepth = qMax(maxDepth, ++depth);
    } else if (op == 142) {
      if (!inPage || --depth < 0) fail("Invalid DVI stack.");
    } else if (op == 140) {
      if (!inPage || depth != 0) fail("Invalid DVI page ending.");
      inPage = false;
      lastEnd = pos;
      ++pages;
    } else if (op >= 243 && op <= 246) {
      fonts.insert(uintBe(data, start + 1, op - 242), data.mid(start, pos - start));
    }
  }
  if (inPage || pages == 0 || lastEnd != data.size()) fail("The live DVI does not end at a complete page.");
  if (pages > 65535 || maxDepth > 65535 || data.size() > std::numeric_limits<qint32>::max())
    fail("The live DVI exceeds standard DVI limits; restart the engine.");
  QByteArray result = data;
  result.append(char(248));
  appendBe(result, static_cast<quint32>(lastBop), 4);
  result.append(data.mid(2, 12));
  appendBe(result, 0x3fffffff, 4);
  appendBe(result, 0x3fffffff, 4);
  appendBe(result, static_cast<quint32>(maxDepth), 2);
  appendBe(result, static_cast<quint32>(pages), 2);
  for (auto it = fonts.cend(); it != fonts.cbegin();) result.append((--it).value());
  result.append(char(249));
  appendBe(result, static_cast<quint32>(data.size()), 4);
  result.append(data[1]);
  result.append(4, char(223));
  while (result.size() % 4) result.append(char(223));
  return {result, pages};
}

QByteArray workerSource(const QString &preamble) {
  // The same preview/minipage layout used by the XeTeX GUI, with a Lua
  // stdout flush and no checkpoint special. halt-on-error makes failures
  // unambiguous; the host starts a fresh worker after an invalid fragment.
  return QString::fromLatin1(R"TEX(\PassOptionsToPackage{xetex}{preview}
\input{\detokenize{%1}}
\newcount\snippetcount
\newdimen\stemtexfontsize
\newdimen\stemtexbaselineskip
\def\signal#1{\directlua{io.stdout:write(string.char(10).."#1"..string.char(10));io.stdout:flush()}}
\pagestyle{empty}
\makeatletter
% preview's DVI default is its dvips driver. Use its native DVI offsets
% instead, then emit page dimensions explicitly: LuaTeX does not generate
% XeTeX's papersize special from the pagewidth/pageheight registers.
\g@addto@macro\pr@ship@end{\setbox\pr@box\hbox{%
  \special{papersize=\the\pdfpagewidth,\the\pdfpageheight}\box\pr@box}}
\def\resetfragment{%
  \setcounter{page}{1}\setcounter{equation}{0}\setcounter{footnote}{0}%
  \setcounter{enumi}{0}\setcounter{enumii}{0}\setcounter{enumiii}{0}\setcounter{enumiv}{0}%
  \global\@listdepth=\z@ \@itemdepth=\z@ \@enumdepth=\z@
  \global\@inlabelfalse\global\@newlistfalse\global\@noparitemfalse\global\@noparlistfalse
  \@noitemargfalse\@nmbrlistfalse\global\setbox\@labels=\box\voidb@x}
\makeatother
\begin{document}
\signal{STEMTEX_LUA_READY}
\def\workerloop{%
  \advance\snippetcount by 1
  \signal{STEMTEX_LUA_WAIT:\the\snippetcount}%
  \read16 to\snippetwidth
  \read16 to\snippetfont
  \read16 to\snippetfile
  \begin{preview}%
  \begin{minipage}{\snippetwidth}%
    \hsize=\snippetwidth\parindent=0pt
    \begingroup\resetfragment
    \stemtexfontsize=\snippetfont
    \stemtexbaselineskip=1.2\stemtexfontsize
    \normalfont\fontsize{\the\stemtexfontsize}{\the\stemtexbaselineskip}\selectfont\normalcolor
    \input{\snippetfile}\par
    % Even an empty minipage must retain the requested width. A zero-height
    % rule gives its vbox that width without ink or extra vertical space.
    \hrule width\hsize height0pt depth0pt
    \endgroup
  \end{minipage}%
  \end{preview}%
  \signal{STEMTEX_LUA_DONE:\the\snippetcount}%
  \workerloop
}
\workerloop
)TEX").arg(preamble).toUtf8();
}

// The hot converter APIs use process-global kpathsea/fontconfig state. As with
// the XeTeX renderer, one GUI backend owns this environment until shutdown.
// The GUI serializes its lifecycle with the XeTeX renderer's lifecycle lock.
class ConverterEnvironment {
 public:
  explicit ConverterEnvironment(const QMap<QString, QString> &values) {
    try {
      for (auto it = values.cbegin(); it != values.cend(); ++it) {
        Entry entry;
        entry.name = it.key().toStdWString();
        SetLastError(ERROR_SUCCESS);
        const DWORD needed = GetEnvironmentVariableW(entry.name.c_str(), nullptr, 0);
        entry.hasWindowsValue = needed > 0 || GetLastError() != ERROR_ENVVAR_NOT_FOUND;
        if (needed) {
          entry.windowsValue.resize(needed);
          GetEnvironmentVariableW(entry.name.c_str(), entry.windowsValue.data(), needed);
          entry.windowsValue.resize(needed - 1);
        }
        if (const wchar_t *value = _wgetenv(entry.name.c_str())) {
          entry.hasCrtValue = true;
          entry.crtValue = value;
        }
        entries_.push_back(std::move(entry));
        const std::wstring value = it.value().toStdWString();
        if (_wputenv_s(entries_.back().name.c_str(), value.c_str()) != 0 ||
            !SetEnvironmentVariableW(entries_.back().name.c_str(), value.c_str()))
          fail("Cannot configure the resident converter environment: " + it.key());
      }
    } catch (...) {
      restore();
      throw;
    }
  }
  ~ConverterEnvironment() { restore(); }
  ConverterEnvironment(const ConverterEnvironment &) = delete;
  ConverterEnvironment &operator=(const ConverterEnvironment &) = delete;

 private:
  struct Entry {
    std::wstring name, windowsValue, crtValue;
    bool hasWindowsValue = false, hasCrtValue = false;
  };
  std::vector<Entry> entries_;
  void restore() noexcept {
    for (auto it = entries_.rbegin(); it != entries_.rend(); ++it) {
      _wputenv_s(it->name.c_str(), it->hasCrtValue ? it->crtValue.c_str() : L"");
      SetEnvironmentVariableW(it->name.c_str(), it->hasWindowsValue ? it->windowsValue.c_str() : nullptr);
    }
    entries_.clear();
  }
};

class ConverterModule {
 public:
  using Api = int(__cdecl *)(int, char **);
  using Shutdown = int(__cdecl *)();
  using IssueFlags = int(__cdecl *)();
  using Message = const char *(__cdecl *)();

  ~ConverterModule() {
    if (library_) {
      if (initAttempted_ && shutdown_) shutdown_();
      FreeLibrary(library_);
    }
  }
  void initialize(const QString &runtimeRoot, bool svg) {
    svg_ = svg;
    const QString base = svg ? "dvisvgmdaemon" : "dvipdfmxdaemon";
    const QString dll = pathIn(runtimeRoot, "bin/windows/" + base + ".dll");
    program_ = pathIn(runtimeRoot, svg ? "bin/windows/dvisvgmdaemon.exe" : "bin/windows/xdvipdfmxdaemon.exe").toUtf8();
    library_ = LoadLibraryExW(reinterpret_cast<LPCWSTR>(dll.utf16()), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!library_) fail(QString("Cannot load resident converter %1 (Windows error %2).").arg(dll).arg(GetLastError()));
    const auto symbol = [&](const char *suffix) { return GetProcAddress(library_, (base.toLatin1() + suffix).constData()); };
    const auto init = reinterpret_cast<Api>(symbol("_init"));
    convert_ = reinterpret_cast<Api>(symbol("_convert"));
    shutdown_ = reinterpret_cast<Shutdown>(symbol("_shutdown"));
    flags_ = svg ? nullptr : reinterpret_cast<IssueFlags>(symbol("_last_issue_flags"));
    message_ = reinterpret_cast<Message>(symbol(svg ? "_last_error_message" : "_last_issue_message"));
    if (!init || !convert_ || !shutdown_ || !message_ || (!svg && !flags_))
      fail("Resident converter is missing its hot-start API: " + dll);
    std::vector<QByteArray> arguments{program_};
    std::vector<char *> argv{arguments.front().data(), nullptr};
    initAttempted_ = true;
    const int code = init(1, argv.data());
    if (code != 0) fail(QString("%1_init returned %2: %3").arg(base).arg(code).arg(message()));
  }

  struct Result { int code = 0, issueFlags = 0; QString message; };
  Result convert(const QString &dvi, const QString &output, int page) {
    std::vector<QByteArray> arguments;
    if (svg_) {
      arguments = {program_, "--page", QByteArray::number(page), "--bbox", "papersize", "--exact-bbox",
                   "--no-fonts", "--output", output.toUtf8(), dvi.toUtf8()};
    } else {
      arguments = {program_, "-q", "-z", "1", "-C", "64", "-s", QByteArray::number(page),
                   "-o", output.toUtf8(), dvi.toUtf8()};
    }
    std::vector<char *> argv;
    for (auto &argument : arguments) argv.push_back(argument.data());
    argv.push_back(nullptr);
    Result result;
    result.code = convert_(static_cast<int>(arguments.size()), argv.data());
    result.issueFlags = flags_ ? flags_() : 0;
    result.message = message();
    if (result.code != 0 && (svg_ || result.code != 2))
      fail(QString("%1_convert returned %2: %3").arg(svg_ ? "dvisvgmdaemon" : "dvipdfmxdaemon")
               .arg(result.code).arg(result.message));
    return result;
  }

 private:
  QString message() const {
    const char *value = message_ ? message_() : nullptr;
    return value ? QString::fromUtf8(value) : QString();
  }
  HMODULE library_ = nullptr;
  Api convert_ = nullptr;
  Shutdown shutdown_ = nullptr;
  IssueFlags flags_ = nullptr;
  Message message_ = nullptr;
  QByteArray program_;
  bool svg_ = false, initAttempted_ = false;
};

struct HotConverters {
  // Destruction order is significant: shutdown/unload both DLLs before
  // restoring the environment from which their persistent state was built.
  std::unique_ptr<ConverterEnvironment> environment;
  ConverterModule pdf;
  ConverterModule svg;

  void initialize(const LuaBackendConfig &config, const QString &cacheRoot, const QString &sessionRoot) {
    const QString conf = pathIn(sessionRoot, "fontconfig/conf");
    const QString fontCache = pathIn(cacheRoot, "fontconfig/cache");
    if (!QDir().mkpath(conf + "/conf.d") || !QDir().mkpath(fontCache))
      fail("Cannot create the resident converter font cache.");
    const QString systemRoot = qEnvironmentVariable("SystemRoot", "C:/Windows");
    const QString xml = QString("<?xml version=\"1.0\"?>\n<!DOCTYPE fontconfig SYSTEM \"fonts.dtd\">\n"
        "<fontconfig><dir>%1</dir><dir>%2</dir><dir>%3</dir><cachedir>%4</cachedir>"
        "<include ignore_missing=\"yes\">conf.d</include><config><rescan><int>30</int></rescan></config></fontconfig>\n")
        .arg(pathIn(systemRoot, "Fonts").toHtmlEscaped(),
             pathIn(config.texmfRoot, "texmf-dist/fonts/opentype").toHtmlEscaped(),
             pathIn(config.texmfRoot, "texmf-dist/fonts/truetype").toHtmlEscaped(), fontCache.toHtmlEscaped());
    writeFile(pathIn(conf, "fonts.conf"), xml.toUtf8());
    writeFile(pathIn(conf, "conf.d/51-local.conf"), "<?xml version=\"1.0\"?><fontconfig></fontconfig>\n");
    const QString bin = pathIn(config.runtimeRoot, "bin/windows");
    const QString dist = pathIn(config.texmfRoot, "texmf-dist");
    const QString treeVar = pathIn(config.texmfRoot, "texmf-var");
    const QString treeConfig = pathIn(config.texmfRoot, "texmf-config");
    const QString fontMaps = pathIn(treeVar, "fonts/map/pdftex/updmap") + ";" +
        pathIn(treeVar, "fonts/map/dvipdfmx/updmap") + ";" + pathIn(dist, "fonts/map/dvipdfmx") + ";" +
        pathIn(dist, "fonts/map") + "//";
    QMap<QString, QString> overrides{
        {"PATH", bin + ";" + pathIn(systemRoot, "System32")}, {"TEXMFROOT", config.texmfRoot},
        {"TEXMFCNF", pathIn(dist, "web2c")}, {"TEXFORMATS", cacheRoot + "//;"},
        {"XE_FONTCONFIG_PATH", conf}, {"FONTCONFIG_PATH", conf}, {"XE_FC_CACHEDIR", fontCache},
        {"FC_CACHEDIR", fontCache}, {"TEXMFDIST", dist}, {"TEXMFSYSVAR", treeVar},
        {"TEXMFSYSCONFIG", treeConfig}, {"TEXMFVAR", pathIn(cacheRoot, "texmf-var")},
        {"TEXMFCONFIG", pathIn(cacheRoot, "texmf-config")}, {"TEXFONTMAPS", fontMaps},
        {"WEB2C", pathIn(dist, "web2c")}, {"W32TEX", config.runtimeRoot}, {"command_line_encoding", ""},
        {"STEMTEX_XDVIPDFMX_TRACE", pathIn(sessionRoot, "xdvipdfmx-init-trace.log")}};
    if (QDir(pathIn(bin, "icu-data")).exists()) overrides.insert("ICU_DATA", pathIn(bin, "icu-data"));
    environment = std::make_unique<ConverterEnvironment>(overrides);
    pdf.initialize(config.runtimeRoot, false);
    svg.initialize(config.runtimeRoot, true);
  }
};

}  // namespace

struct LuaBackend::Impl {
  explicit Impl(LuaBackendConfig value) : config(std::move(value)) {
    job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
    info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (job && !SetInformationJobObject(job, JobObjectExtendedLimitInformation, &info, sizeof(info))) {
      CloseHandle(job);
      job = nullptr;
    }
  }

  ~Impl() {
    stopWorker();
    if (job) CloseHandle(job);
  }

  LuaBackendConfig config;
  QProcessEnvironment environment;
  std::unique_ptr<QProcess> worker;
  std::unique_ptr<HotConverters> converters;
  HANDLE job = nullptr;
  std::atomic<bool> cancelled{false};
  std::atomic<bool> isReady{false};
  std::atomic<qint64> pid{0};
  QString sessionRoot, workRoot, formatFile, cacheRoot, lastSnapshotPath;
  QByteArray pending, transcript;
  int request = 0, pageCount = 0, workerGeneration = 0;
  bool initialized = false;

  void checkCancelled() const {
    if (cancelled.load()) fail("LuaHBTeX operation cancelled.");
  }

  void setupProcess(QProcess &process, const QString &workingDirectory) {
    process.setProcessEnvironment(environment);
    process.setWorkingDirectory(workingDirectory);
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *args) {
      args->flags |= CREATE_NO_WINDOW;
    });
  }

  void start(QProcess &process, const QString &program, const QStringList &arguments) {
    checkCancelled();
    process.start(program, arguments);
    if (!process.waitForStarted(10000)) fail(QString("Cannot start %1: %2").arg(program, process.errorString()));
    if (job) {
      HANDLE child = OpenProcess(PROCESS_SET_QUOTA | PROCESS_TERMINATE, FALSE, DWORD(process.processId()));
      if (!child || !AssignProcessToJobObject(job, child)) {
        const DWORD code = GetLastError();
        if (child) CloseHandle(child);
        process.kill();
        process.waitForFinished(5000);
        fail(QString("Cannot attach LuaHBTeX child process to its lifetime job (%1).").arg(code));
      }
      CloseHandle(child);
    }
  }

  QByteArray run(const QString &program, const QStringList &arguments, const QString &workingDirectory,
                 int timeout) {
    QProcess process;
    setupProcess(process, workingDirectory);
    start(process, program, arguments);
    QElapsedTimer timer;
    timer.start();
    QByteArray output;
    while (process.state() != QProcess::NotRunning) {
      if (cancelled.load() || timer.elapsed() >= timeout) {
        process.kill();
        process.waitForFinished(5000);
        output += process.readAll();
        if (cancelled.load()) fail("LuaHBTeX operation cancelled.");
        fail(QString("%1 timed out.\n%2").arg(QFileInfo(program).fileName(), QString::fromUtf8(output.right(6000))));
      }
      process.waitForFinished(50);
      output += process.readAll();
      if (output.size() > 1024 * 1024) output = output.right(1024 * 1024);
    }
    output += process.readAll();
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0)
      fail(QString("%1 exited with code %2.\n%3").arg(QFileInfo(program).fileName()).arg(process.exitCode())
               .arg(QString::fromUtf8(output.right(10000))));
    return output;
  }

  void stopWorker() {
    isReady.store(false);
    pid.store(0);
    if (worker) {
      if (worker->state() != QProcess::NotRunning) {
        worker->kill();
        worker->waitForFinished(5000);
      }
      if (!workRoot.isEmpty()) {
        QFile log(pathIn(workRoot, "host-transcript.log"));
        if (log.open(QIODevice::WriteOnly)) log.write(transcript + worker->readAll());
      }
      worker.reset();
    }
  }

  void waitMarker(const QByteArray &marker) {
    QElapsedTimer timer;
    timer.start();
    while (true) {
      checkCancelled();
      const QByteArray available = worker->readAll();
      pending += available;
      transcript += available;
      if (transcript.size() > 1024 * 1024) transcript = transcript.right(1024 * 1024);
      qsizetype newline;
      while ((newline = pending.indexOf('\n')) >= 0) {
        const QByteArray line = pending.left(newline).trimmed();
        pending.remove(0, newline + 1);
        if (line == marker) return;
      }
      if (pending.size() > 1024 * 1024) pending = pending.right(65536);
      if (worker->state() == QProcess::NotRunning)
        fail(QString("LuaHBTeX stopped before %1.\n%2").arg(QString::fromLatin1(marker), QString::fromUtf8(transcript.right(10000))));
      if (timer.elapsed() >= config.timeoutMs)
        fail(QString("LuaHBTeX timed out waiting for %1.\n%2").arg(QString::fromLatin1(marker), QString::fromUtf8(transcript.right(6000))));
      worker->waitForReadyRead(50);
    }
  }

  void startWorker() {
    stopWorker();
    checkCancelled();
    workRoot = pathIn(sessionRoot, QString("worker-%1").arg(++workerGeneration));
    if (!QDir().mkpath(workRoot)) fail("Cannot create the LuaHBTeX worker directory.");
    writeFile(pathIn(workRoot, "worker.tex"), workerSource(pathIn(config.profileRoot, "preamble.tex")));
    pending.clear();
    transcript.clear();
    request = pageCount = 0;
    lastSnapshotPath.clear();
    worker = std::make_unique<QProcess>();
    setupProcess(*worker, workRoot);
    start(*worker, config.enginePath,
          {"--fmt=" + formatFile, "--progname=lualatex", "--output-format=dvi", "--flush-output-on-shipout",
           "--interaction=errorstopmode", "--halt-on-error", "--no-shell-escape", "worker.tex"});
    pid.store(worker->processId());
    waitMarker("STEMTEX_LUA_READY");
    waitMarker("STEMTEX_LUA_WAIT:1");
    isReady.store(true);
  }

  void prepare(const std::function<void(const QString &)> &progress) {
    auto report = [&](const QString &message) { checkCancelled(); if (progress) progress(message); };
    report("Checking LuaHBTeX and the full TeX Live installation");
    if (!job) fail("Cannot create the LuaHBTeX child-process lifetime job.");
    if (config.enginePath.isEmpty()) config.enginePath = pathIn(config.repoRoot, "build/luahbtex-mingw/bin/luahbtex.exe");
    config.enginePath = QFileInfo(config.enginePath).absoluteFilePath();
    config.texmfRoot = QDir(config.texmfRoot).absolutePath();
    config.profileRoot = QDir(config.profileRoot).absolutePath();
    if (config.runtimeRoot.isEmpty()) config.runtimeRoot = pathIn(config.repoRoot, "staging/runtime");
    config.runtimeRoot = QDir(config.runtimeRoot).absolutePath();
    if (!QFileInfo::exists(config.enginePath))
      fail(QString("Patched LuaHBTeX is missing: %1\nBuild it with scripts/build-luahbtex-mingw.ps1.").arg(config.enginePath));
    const QString bin = pathIn(config.texmfRoot, "bin/windows");
    for (const QString &relative : {QString("texmf-dist/web2c/texmf.cnf"),
                                    QString("texmf-dist/tex/latex/tex-ini-files/lualatex.ini")})
      if (!QFileInfo::exists(pathIn(config.texmfRoot, relative)))
        fail(QString("LuaHBTeX needs a full TeX Live installation. Missing %1").arg(pathIn(config.texmfRoot, relative)));
    for (const QString &name : {QString("dvipdfmxdaemon.dll"), QString("dvisvgmdaemon.dll")}) {
      const QString path = pathIn(config.runtimeRoot, "bin/windows/" + name);
      if (!QFileInfo::exists(path)) fail("Resident converter is missing: " + path);
    }
    if (!QFileInfo::exists(pathIn(config.profileRoot, "preamble.tex"))) fail("The LuaHBTeX profile has no preamble.tex.");
    if (config.stateRoot.isEmpty()) config.stateRoot = pathIn(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation), "StemTeX/lua-gui");
    sessionRoot = pathIn(config.stateRoot, "sessions/" + QUuid::createUuid().toString(QUuid::WithoutBraces));
    if (!QDir().mkpath(sessionRoot)) fail("Cannot create the LuaHBTeX session directory.");
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(readFile(config.enginePath));
    hash.addData(config.texmfRoot.toUtf8());
    for (const QString &relative : {QString("texmf-dist/tex/latex/base/latex.ltx"), QString("texmf-dist/tex/latex/tex-ini-files/lualatex.ini")}) {
      QFileInfo file(pathIn(config.texmfRoot, relative));
      hash.addData(QByteArray::number(file.size()));
      hash.addData(QByteArray::number(file.lastModified().toMSecsSinceEpoch()));
    }
    cacheRoot = pathIn(config.stateRoot, "cache/" + QString::fromLatin1(hash.result().toHex().left(24)));
    if (!QDir().mkpath(cacheRoot)) fail("Cannot create the LuaHBTeX format cache directory.");
    formatFile = pathIn(cacheRoot, "lualatex.fmt");
    environment = QProcessEnvironment::systemEnvironment();
    environment.insert("TEXMFROOT", config.texmfRoot);
    environment.insert("TEXMFCNF", pathIn(config.texmfRoot, "texmf-dist/web2c"));
    environment.insert("TEXMFVAR", pathIn(cacheRoot, "texmf-var"));
    environment.insert("TEXMFCACHE", pathIn(cacheRoot, "texmf-var"));
    QDir().mkpath(pathIn(cacheRoot, "texmf-var"));
    environment.insert("TEXFORMATS", cacheRoot + "//;");
    environment.insert("TEXINPUTS", config.profileRoot + "//;");
    environment.insert("PATH", bin + ";" + environment.value("PATH"));
    environment.insert("max_print_line", "1000");
    report("Preparing a format matched to the patched LuaHBTeX engine");
    QLockFile lock(pathIn(cacheRoot, "format.lock"));
    lock.setStaleLockTime(0);
    QElapsedTimer lockTimer;
    lockTimer.start();
    while (!lock.tryLock(100)) {
      checkCancelled();
      if (lockTimer.elapsed() > qMax(config.timeoutMs, 180000)) fail("Timed out waiting for another LuaHBTeX format build.");
    }
    if (!QFileInfo::exists(formatFile) || QFileInfo(formatFile).size() == 0) {
      const QByteArray log = run(config.enginePath,
          {"--ini", "--interaction=nonstopmode", "--halt-on-error", "--no-shell-escape", "--progname=lualatex",
           "--jobname=lualatex", "--output-directory=" + sessionRoot, "lualatex.ini"}, sessionRoot, qMax(config.timeoutMs, 180000));
      writeFile(pathIn(sessionRoot, "format-build.log"), log);
      writeFile(formatFile, readFile(pathIn(sessionRoot, "lualatex.fmt")));
    }
    lock.unlock();
    report("Starting the resident PDF and SVG converters");
    converters = std::make_unique<HotConverters>();
    converters->initialize(config, cacheRoot, sessionRoot);
    report("Loading the LuaHBTeX profile and waiting for the first request");
    startWorker();
    initialized = true;
    report("LuaHBTeX ready (DVI streaming; errors restart the worker)");
  }
};

LuaBackend::LuaBackend(LuaBackendConfig config) : impl_(std::make_unique<Impl>(std::move(config))) {}
LuaBackend::~LuaBackend() = default;

bool LuaBackend::initialize(const std::function<void(const QString &)> &progress, QString *error) {
  try {
    impl_->prepare(progress);
    return true;
  } catch (const std::exception &e) {
    impl_->stopWorker();
    impl_->converters.reset();
    if (error) *error = QString::fromUtf8(e.what());
    return false;
  }
}

LuaRenderResult LuaBackend::render(const QByteArray &snippet, double widthPt, double fontSizePt, bool svg) {
  LuaRenderResult result;
  result.outputFormat = svg ? "svg" : "pdf";
  QElapsedTimer elapsed;
  elapsed.start();
  bool typesetting = true;
  try {
    impl_->checkCancelled();
    if (!impl_->initialized) fail("LuaHBTeX has not been initialized.");
    if (!std::isfinite(widthPt) || widthPt <= 0 || !std::isfinite(fontSizePt) || fontSizePt <= 0)
      fail("Width and font size must be positive finite numbers.");
    // Parsing cumulative DVI is deliberately simple in this experimental
    // adapter. Rotate before it becomes expensive, and stay well below the
    // 16-bit page count and 31-bit file offsets of standard DVI.
    if (!impl_->isReady.load() || impl_->pageCount >= 2000 ||
        QFileInfo(pathIn(impl_->workRoot, "worker.dvi")).size() >= 32 * 1024 * 1024)
      impl_->startWorker();
    impl_->isReady.store(false);
    const int request = ++impl_->request;
    const QString stem = QString("request-%1").arg(request);
    writeFile(pathIn(impl_->workRoot, stem + ".tex"), snippet + '\n');
    const QByteArray input = QByteArray::number(widthPt, 'f', 4) + "pt\n" +
                             QByteArray::number(fontSizePt, 'f', 4) + "pt\n" + stem.toUtf8() + ".tex\n";
    if (impl_->worker->write(input) != input.size()) fail("Cannot send the snippet to LuaHBTeX.");
    impl_->waitMarker("STEMTEX_LUA_DONE:" + QByteArray::number(request));
    impl_->waitMarker("STEMTEX_LUA_WAIT:" + QByteArray::number(request + 1));
    const qint64 typesetMs = elapsed.elapsed();
    const DviSnapshot snapshot = finalizeDvi(readFile(pathIn(impl_->workRoot, "worker.dvi")));
    if (snapshot.pages != impl_->pageCount + 1)
      fail(QString("A GUI snippet must ship exactly one preview page; this request shipped %1.").arg(snapshot.pages - impl_->pageCount));
    impl_->pageCount = snapshot.pages;
    result.pageNumber = snapshot.pages;
    result.workerPid = impl_->pid.load();
    const QString dvi = pathIn(impl_->workRoot, stem + ".dvi");
    writeFile(dvi, snapshot.finalized);
    if (!impl_->lastSnapshotPath.isEmpty()) QFile::remove(impl_->lastSnapshotPath);
    impl_->lastSnapshotPath = dvi;
    const qint64 convertStart = elapsed.elapsed();
    const qint64 finalizeDviMs = convertStart - typesetMs;
    typesetting = false;
    result.outputPath = pathIn(impl_->workRoot, stem + (svg ? ".svg" : ".pdf"));
    impl_->checkCancelled();
    ConverterModule::Result converted;
    {
      const QMap<QString, QString> trace{{"STEMTEX_XDVIPDFMX_TRACE", pathIn(impl_->workRoot, stem + ".xdvipdfmx-trace.log")}};
      ConverterEnvironment traceEnvironment(trace);
      converted = svg ? impl_->converters->svg.convert(dvi, result.outputPath, snapshot.pages)
                      : impl_->converters->pdf.convert(dvi, result.outputPath, snapshot.pages);
    }
    // Like the XeTeX path, the resident native APIs are synchronous: cancellation
    // is observed before/after conversion, and during TeX/format subprocess work.
    impl_->checkCancelled();
    const qint64 endMs = elapsed.elapsed();
    const qint64 convertMs = endMs - convertStart;
    writeFile(pathIn(impl_->workRoot, stem + ".converter.log"),
              QString("mode=daemon-dll\nreturnCode=%1\nissueFlags=%2\n%3\n")
                  .arg(converted.code).arg(converted.issueFlags).arg(converted.message).toUtf8());
    if (QFileInfo(result.outputPath).size() <= 0) fail("The DVI converter did not write a nonempty output file.");
    result.issueFlags = converted.issueFlags;
    result.recoverable = converted.code == 2 || converted.issueFlags != 0;
    result.outcomeMessage = converted.message;
    if (result.recoverable && result.outcomeMessage.isEmpty())
      result.outcomeMessage = "The resident PDF converter produced output with a recoverable issue.";
    impl_->isReady.store(true);
    const qint64 outputBytes = QFileInfo(result.outputPath).size();
    const QJsonObject summary{
        {"engine", "LuaHBTeX"}, {"outputFormat", result.outputFormat}, {"outputPath", result.outputPath},
        {"backend", svg ? "dvisvgmdaemon" : "xdvipdfmxdaemon"}, {"converterMode", "daemon-dll"},
        {"requestToOutputMs", endMs}, {"requestToPdfMs", svg ? 0 : endMs}, {"requestToSvgMs", svg ? endMs : 0},
        {"typesetMs", typesetMs}, {"finalizeDviMs", finalizeDviMs}, {"convertMs", convertMs},
        {"xdvipdfmxMs", svg ? 0 : convertMs}, {"dvisvgmMs", svg ? convertMs : 0},
        {"widthPt", widthPt}, {"fontSizePt", fontSizePt}, {"outputBytes", outputBytes},
        {"pdfBytes", svg ? 0 : outputBytes}, {"svgBytes", svg ? outputBytes : 0},
        {"workerPid", result.workerPid}, {"workerPage", snapshot.pages}, {"workerRequest", request},
        {"spareReady", 0}, {"spareTarget", 0}, {"checkpoint", false},
        {"finalDviBytes", snapshot.finalized.size()}, {"outcomeCode", result.recoverable ? 1 : 0},
        {"issueFlags", result.issueFlags}, {"outcomeMessage", result.outcomeMessage},
        {"xdvipdfmxOptions", "-q -z 1 -C 64"}, {"xdvipdfmxMode", svg ? "" : "daemon-dll"},
        {"xdvipdfmxReturnCode", svg ? 0 : converted.code}, {"xdvipdfmxIssueFlags", svg ? 0 : result.issueFlags},
        {"xdvipdfmxIssueMessage", svg ? QString() : result.outcomeMessage}, {"xdvipdfmxWarning", !svg && result.recoverable},
        {"dvisvgmOptions", "--bbox=papersize --exact-bbox --no-fonts"}, {"dvisvgmMode", svg ? "daemon-dll" : ""},
        {"dvisvgmReturnCode", svg ? converted.code : 0}, {"dvisvgmErrorMessage", svg ? converted.message : QString()}};
    result.summary = QString::fromUtf8(QJsonDocument(summary).toJson(QJsonDocument::Compact));
    result.ok = true;
  } catch (const std::exception &e) {
    result.error = QString::fromUtf8(e.what());
    if (typesetting || impl_->cancelled.load()) {
      impl_->stopWorker();
      if (impl_->initialized && !impl_->cancelled.load()) {
        try {
          impl_->startWorker();
          result.error += "\nLuaHBTeX restarted; the next valid snippet can be rendered.";
        } catch (const std::exception &restartError) {
          impl_->stopWorker();
          result.error += "\nRestart failed: " + QString::fromUtf8(restartError.what());
        }
      }
    } else {
      impl_->isReady.store(impl_->worker && impl_->worker->state() != QProcess::NotRunning);
    }
  }
  return result;
}

void LuaBackend::cancel() { impl_->cancelled.store(true); impl_->isReady.store(false); }
bool LuaBackend::ready() const { return impl_->isReady.load(); }
qint64 LuaBackend::workerPid() const { return impl_->pid.load(); }

}  // namespace stemtex
