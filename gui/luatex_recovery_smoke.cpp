#include "luatex_backend.h"
#include "../profile-creator/stemtex_profile.h"

#include <QDir>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QPdfDocument>
#include <QPdfSelection>
#include <QSaveFile>
#include <QSet>
#include <QSvgRenderer>
#include <QTextStream>
#include <QThread>
#include <QUuid>
#include <QtMath>

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <stdexcept>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace {
void require(bool condition, const QString &message) {
  if (!condition) throw std::runtime_error(message.toUtf8().constData());
}

void check(QStringList &errors, bool condition, const QString &message) {
  if (!condition) errors.append(message);
}

QJsonArray strings(const QStringList &values) {
  QJsonArray array;
  for (const auto &value : values) array.append(value);
  return array;
}

QJsonObject jsonObject(const QString &source) {
  return QJsonDocument::fromJson(source.toUtf8()).object();
}

void saveReport(const QString &path, const QJsonObject &report) {
  const QByteArray bytes = QJsonDocument(report).toJson();
  QString detail;
  for (int attempt = 0; attempt < 10; ++attempt) {
    QSaveFile file(path);
    if (file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit()) return;
    detail = file.errorString();
    // A Windows progress reader can briefly deny deletion of the old target.
    // Recreate the atomic writer because a failed commit consumes its temp file.
    if (attempt != 9) QThread::msleep(20);
  }
  require(false, "Cannot save report: " + path + ": " + detail);
}

struct Profile {
  QString name;
  bool managed = false;
  bool cjk = false;
};

struct BadInput {
  QString name;
  QByteArray body;
  bool observational = false;
  QString expectedRecovery = "checkpoint";
  QByteArray preparation;
  bool richOnly = false;
};

QByteArray fixturePreamble() {
  // These objects must exist before the engine checkpoint. Recreating a missing
  // fixture during validation would hide incomplete Lua rollback.
  return R"TEX(
\newcount\StemTeXRecoveryCounter
\def\StemTeXRecoveryBaselineDepth{0}
\newdimen\StemTeXRecoveryDimen
\StemTeXRecoveryDimen=7pt
\newtoks\StemTeXRecoveryTokens
\StemTeXRecoveryTokens={BASE}
\newbox\StemTeXRecoveryBox
\setbox\StemTeXRecoveryBox=\hbox{\vrule width3pt height2pt depth0pt}
\def\StemTeXRecoveryExisting{BASE}
\let\StemTeXRecoveryExpected\StemTeXRecoveryExisting
\edef\StemTeXRecoveryCatcode{\number\catcode63}
\directlua{
  local p = {nested={value=17}, keep={value=19}, meta={tag=23}}
  stemtexRecovery = p
  p.alias = p.nested
  p.self = p
  setmetatable(p.nested,p.meta)
  local private = 29
  p.get = function() return private end
  p.set = function(value) private = value end
  local functions = lua.get_functions_table()
  p.functionSlot = luatexbase.new_luafunction("stemtex.recovery.baseline")
  p.addedFunctionSlot = luatexbase.new_luafunction("stemtex.recovery.added")
  p.registryFunction = function() return 31 end
  functions[p.functionSlot] = p.registryFunction
  package.loaded.stemtex_recovery_probe = {value=37}
  p.module = package.loaded.stemtex_recovery_probe
  p.node = node.new("kern")
  p.node.kern = 65536
  node.setproperty(p.node,{nested={value=41}})
  p.font = font.define{name="StemTeXRecoveryMetric",size=655360,designsize=655360,
    characters={[65]={width=655360,height=327680,depth=0}},parameters={quad=655360}}
  p.callback = function(head) return head end
  luatexbase.add_to_callback("pre_linebreak_filter",p.callback,"stemtex.recovery.baseline")
  p.validate = function()
    assert(stemtexRecovery==p,"RECOVERY FIXTURE IDENTITY LOST")
    assert(stemtexRecoveryAdded==nil,"RECOVERY LUA GLOBAL LEAK")
    assert(p.nested.value==17 and p.keep and p.keep.value==19 and p.alias==p.nested and p.self==p,
      "RECOVERY LUA TABLE LEAK")
    assert(getmetatable(p.nested)==p.meta and p.meta.tag==23,"RECOVERY METATABLE LEAK")
    assert(p.get()==29,"RECOVERY UPVALUE LEAK")
    p.set(30) assert(p.get()==30,"RECOVERY SHARED UPVALUE LOST") p.set(29)
    assert(functions==lua.get_functions_table() and functions[p.functionSlot]==p.registryFunction
      and functions[p.functionSlot]()==31,
      "RECOVERY REGISTRY LEAK")
    assert(functions[p.addedFunctionSlot]==false,"RECOVERY ADDED LUA FUNCTION LEAK")
    assert(package.loaded.stemtex_recovery_probe==p.module and p.module.value==37,
      "RECOVERY MODULE CACHE LEAK")
    assert(p.node.kern==65536 and node.getproperty(p.node).nested.value==41,
      "RECOVERY NODE OR PROPERTY LEAK")
    assert(font.getcopy(p.font).characters[65].width==655360,"RECOVERY FONT METRIC LEAK")
    assert(luatexbase.in_callback("pre_linebreak_filter","stemtex.recovery.baseline"),
      "RECOVERY BASELINE CALLBACK LOST")
    assert(not luatexbase.in_callback("pre_linebreak_filter","stemtex.recovery.added"),
      "RECOVERY CALLBACK LEAK")
  end
}
)TEX";
}

QByteArray fileHash(const QString &path) {
  QFile file(path);
  require(file.open(QIODevice::ReadOnly), "Cannot read published output: " + path);
  return QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256).toHex();
}

QString materialize(const stemtex::LuaBackendConfig &config, const Profile &profile,
                    const QString &profilesRoot) {
  if (!profile.managed) {
    QFile source(QDir(config.repoRoot).filePath("gui/profiles/unicodemath/preamble.tex"));
    require(source.open(QIODevice::ReadOnly), "Missing builtin profile: " + source.fileName());
    const QString path = QDir(profilesRoot).filePath(profile.name);
    require(QDir().mkpath(path), "Cannot create builtin test profile");
    QSaveFile target(QDir(path).filePath("preamble.tex"));
    const QByteArray preamble = source.readAll() + fixturePreamble();
    require(target.open(QIODevice::WriteOnly) && target.write(preamble) == preamble.size() && target.commit(),
            "Cannot prepare builtin test profile");
    return path;
  }
  const QByteArray texmf = config.texmfRoot.toUtf8(), name = profile.name.toUtf8();
  const QByteArray root = profilesRoot.toUtf8();
  const std::vector<const char *> packages = profile.cjk
      ? std::vector<const char *>{"mathtools", "mhchem", "physics", "siunitx", "graphicx", "xcolor", "tikz"}
      : std::vector<const char *>{};
  const StemTeXProfileContext context{texmf.constData()};
  StemTeXProfileSpecV4 spec{};
  spec.name_utf8 = name.constData();
  spec.text_font_id_utf8 = "latin-modern";
  spec.math_font_id_utf8 = "latin-modern-math";
  spec.cjk_font_id_utf8 = profile.cjk ? "fandol-song" : "none";
  spec.package_ids_utf8 = packages.data();
  spec.package_count = packages.size();
  const QByteArray userPreamble = fixturePreamble();
  spec.user_preamble_utf8 = userPreamble.constData();
  spec.engine = STEMTEX_PROFILE_ENGINE_LUAHBTEX;
  StemTeXProfileErrorCode code = STEMTEX_PROFILE_OK;
  char *result = nullptr, *error = nullptr;
  const bool ok = stemtex_profile_materialize_v4(&context, &spec, root.constData(), &result, &code, &error) != 0;
  const QString encoded = result ? QString::fromUtf8(result) : QString();
  const QString message = error ? QString::fromUtf8(error) : QString();
  stemtex_profile_free_string(result);
  stemtex_profile_free_string(error);
  require(ok, QString("Profile generation failed (%1): %2").arg(code).arg(message));
  const QString path = jsonObject(encoded).value("profilePath").toString();
  require(!path.isEmpty(), "Profile generation returned no path");
  return path;
}

QByteArray validSnippet(const QString &marker, bool cjk) {
  // Capture depth before our own conditionals. The protocol's input boundary is
  // at iflevel zero; its preamble fixture must survive an in-process rollback.
  QByteArray body = R"TEX(\edef\StemTeXRecoveryCurrentDepth{\number\currentiflevel}
\ifdefined\StemTeXRecoveryBaselineDepth
  \ifnum\StemTeXRecoveryCurrentDepth=\StemTeXRecoveryBaselineDepth\relax
  \else\errmessage{RECOVERY CONDITIONAL DEPTH LEAK}\fi
\else
  \errmessage{RECOVERY CONDITIONAL BASELINE LOST}
\fi
\ifdefined\StemTeXRecoveryPoison\errmessage{RECOVERY GLOBAL MACRO LEAK}\fi
\ifdefined\StemTeXRecoveryCounter
  \ifnum\StemTeXRecoveryCounter=0\relax
  \else\errmessage{RECOVERY GLOBAL COUNTER LEAK}\fi
\else\errmessage{RECOVERY COUNTER FIXTURE LOST}\fi
\ifx\StemTeXRecoveryExisting\StemTeXRecoveryExpected\else
  \errmessage{RECOVERY EXISTING MACRO LEAK}\fi
\ifdim\StemTeXRecoveryDimen=7pt\else\errmessage{RECOVERY DIMENSION LEAK}\fi
\edef\StemTeXRecoveryTokenText{\the\StemTeXRecoveryTokens}
\ifx\StemTeXRecoveryTokenText\StemTeXRecoveryExpected\else
  \errmessage{RECOVERY TOKEN REGISTER LEAK}\fi
\ifdim\wd\StemTeXRecoveryBox=3pt\else\errmessage{RECOVERY BOX WIDTH LEAK}\fi
\ifdim\ht\StemTeXRecoveryBox=2pt\else\errmessage{RECOVERY BOX HEIGHT LEAK}\fi
\ifnum\catcode63=\StemTeXRecoveryCatcode\relax\else\errmessage{RECOVERY CATCODE LEAK}\fi
\directlua{assert(stemtexRecovery,"RECOVERY LUA FIXTURE LOST");stemtexRecovery.validate()}
{\begingroup\relax\endgroup}\iftrue\relax\else\relax\fi
)TEX";
  body += "\\mbox{" + marker.toUtf8() + "}. \\mbox{STATEZERO}.\\par\n";
  body += "\\[E=mc^2,\\quad \\sum_{k=1}^{n}k=\\frac{n(n+1)}2\\]\n";
  if (cjk) {
    body += QString::fromUtf8("中文恢复测试，\\textbf{粗体}和\\textit{斜体}。\\par\n").toUtf8();
    body += "\\ce{H2O}. \\SI{9.81}{\\metre\\per\\second\\squared}. "
            "$\\dv{x}{t}$.\\par\n"
            "\\textcolor{blue}{Recovered graphics}. "
            "\\scalebox{1.2}{\\rule{8pt}{3pt}}\\quad"
            "\\begin{tikzpicture}[baseline]\\draw (0,0)--(8pt,4pt);\\end{tikzpicture}\n";
  }
  return body;
}

QByteArray poisoned(const BadInput &input) {
  return QByteArray(R"TEX(\global\def\StemTeXRecoveryPoison{POISONED}
\ifdefined\StemTeXRecoveryCounter\else\newcount\StemTeXRecoveryCounter\fi
\global\StemTeXRecoveryCounter=73\relax
)TEX") + input.body + '\n';
}

BadInput luaMutation(const QString &name, const QByteArray &mutation, const QByteArray &dirtyCheck) {
  const QByteArray prefix = "local p=stemtexRecovery;assert(p,'RECOVERY FIXTURE MISSING');";
  return {name, "\\directlua{" + prefix + dirtyCheck + "}\n@STEMTEX_MUTATION_PROOF@\n\\StemTeXUndefinedRecovery\n", false,
          "checkpoint", "\\directlua{" + prefix + mutation + dirtyCheck + "}\n"};
}

QByteArray mutationProofSource(const QByteArray &marker) {
  QByteArray codes;
  for (unsigned char value : marker) {
    if (!codes.isEmpty()) codes += ',';
    codes += QByteArray::number(value);
  }
  // The literal marker never appears in the TeX source, so echoed error context
  // cannot masquerade as proof that the preceding dirty-state checks executed.
  return "\\directlua{io.stdout:write(string.char(10)..string.char(" + codes +
         ")..string.char(10));io.stdout:flush()}\n";
}

std::vector<BadInput> pollutionInputs() {
  std::vector<BadInput> inputs{
    luaMutation("lua-global-table",
      "stemtexRecoveryAdded={value=99};p.nested.value=99;p.keep=nil;p.meta.tag=99;"
      "package.loaded.stemtex_recovery_probe.value=99;collectgarbage('collect');",
      "assert(stemtexRecoveryAdded.value==99 and p.nested.value==99 and p.keep==nil and "
      "p.meta.tag==99 and p.module.value==99,'GLOBAL MUTATION DID NOT APPLY');"),
    luaMutation("lua-registry",
      "local f=lua.get_functions_table();f[p.functionSlot]=function() return 99 end;"
      "f[p.addedFunctionSlot]=function() return 101 end;",
      "local f=lua.get_functions_table();assert(f[p.functionSlot]()==99 and f[p.addedFunctionSlot]()==101,"
      "'REGISTRY MUTATION DID NOT APPLY');"),
    luaMutation("lua-upvalue", "p.set(99);",
      "assert(p.get()==99,'UPVALUE MUTATION DID NOT APPLY');"),
    luaMutation("lua-callback",
      "luatexbase.remove_from_callback('pre_linebreak_filter','stemtex.recovery.baseline');"
      "luatexbase.add_to_callback('pre_linebreak_filter',p.callback,'stemtex.recovery.added');",
      "assert(not luatexbase.in_callback('pre_linebreak_filter','stemtex.recovery.baseline') and "
      "luatexbase.in_callback('pre_linebreak_filter','stemtex.recovery.added'),'CALLBACK MUTATION DID NOT APPLY');"),
    luaMutation("lua-node-property",
      "p.node.kern=131072;node.getproperty(p.node).nested.value=99;",
      "assert(p.node.kern==131072 and node.getproperty(p.node).nested.value==99,'NODE MUTATION DID NOT APPLY');"),
    luaMutation("lua-font-metric",
      "font.addcharacters(p.font,{characters={[65]={width=1310720,height=327680,depth=0}}});",
      "assert(font.getcopy(p.font).characters[65].width==1310720,'FONT MUTATION DID NOT APPLY');")
  };
  const QByteArray texCheck = R"TEX(
\ifnum\StemTeXRecoveryCounter=73\else\errmessage{COUNTER MUTATION DID NOT APPLY}\fi
\ifdim\StemTeXRecoveryDimen=19pt\else\errmessage{DIMENSION MUTATION DID NOT APPLY}\fi
\ifdim\wd\StemTeXRecoveryBox=9pt\else\errmessage{BOX MUTATION DID NOT APPLY}\fi
\ifnum\catcode63=11\relax\else\errmessage{CATCODE MUTATION DID NOT APPLY}\fi
)TEX";
  inputs.push_back({"tex-state", texCheck + "@STEMTEX_MUTATION_PROOF@\n\\StemTeXUndefinedRecovery\n", false, "checkpoint",
    QByteArray(R"TEX(
\global\StemTeXRecoveryCounter=73
\global\def\StemTeXRecoveryExisting{POISON}
\global\StemTeXRecoveryDimen=19pt
\global\StemTeXRecoveryTokens={POISON}
\global\setbox\StemTeXRecoveryBox=\hbox{\vrule width9pt height4pt depth0pt}
\global\catcode63=11
)TEX") + texCheck});
  inputs.push_back({"package-tikz-error",
    "\\begin{tikzpicture}\\draw[stemtex-recovery-unknown] (0,0)--(1,1);\\end{tikzpicture}",
    false, "checkpoint", {}, true});
  inputs.push_back({"package-siunitx-error", "\\SI{not-a-number}{\\metre}", false, "checkpoint", {}, true});
  return inputs;
}

QJsonObject inspectOutput(const stemtex::LuaRenderResult &result, const QString &marker,
                          double width, bool svg, bool cjk) {
  QSizeF points;
  QString text;
  QImage image;
  if (svg) {
    QSvgRenderer document(result.outputPath);
    require(document.isValid(), "Invalid SVG output");
    points = document.viewBoxF().size();
    require(points.isValid() && points.width() < 10000 && points.height() < 10000, "Invalid SVG dimensions");
    image = QImage(QSize(qCeil(points.width() * 2), qCeil(points.height() * 2)), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::white);
    QPainter painter(&image);
    document.render(&painter);
  } else {
    QPdfDocument document;
    require(document.load(result.outputPath) == QPdfDocument::Error::None && document.pageCount() == 1,
            "Expected one valid PDF page");
    points = document.pagePointSize(0);
    require(points.isValid() && points.width() < 10000 && points.height() < 10000, "Invalid PDF dimensions");
    text = document.getAllText(0).text();
    require(text.contains(marker), "PDF lost current unique marker: " + text.left(500));
    require(text.contains("STATEZERO"), "PDF lost clean-state marker");
    if (cjk) require(text.contains(QString::fromUtf8("中文恢复测试")), "PDF lost Chinese recovery text");
    const auto page = document.render(0, QSize(qCeil(points.width() * 2), qCeil(points.height() * 2)));
    require(!page.isNull(), "Cannot rasterize PDF");
    image = QImage(page.size(), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::white);
    QPainter painter(&image);
    painter.drawImage(0, 0, page);
  }
  require(!image.isNull(), "Empty output raster");
  require(std::abs(points.width() - (width + 2) * 72.0 / 72.27) < 0.3, "Output lost requested page width");
  int ink = 0;
  const auto pixels = image.convertToFormat(QImage::Format_ARGB32);
  for (int y = 0; y < pixels.height(); ++y) {
    const auto *row = reinterpret_cast<const QRgb *>(pixels.constScanLine(y));
    for (int x = 0; x < pixels.width(); ++x)
      if (qAlpha(row[x]) >= 128 && qGray(row[x]) < 220) ++ink;
  }
  require(ink > 50, "Output is visually blank");
  const QString preview = result.outputPath + ".png";
  require(image.save(preview), "Cannot save output evidence");
  return {{"widthBp", points.width()}, {"heightBp", points.height()}, {"inkPixels", ink},
          {"text", text}, {"preview", preview}};
}

struct Worker {
  qint64 pid = 0;
  int page = -1;
};

struct Processes {
  QSet<qint64> seen;
  std::vector<HANDLE> handles;
  ~Processes() { for (HANDLE handle : handles) CloseHandle(handle); }
  void track(qint64 pid) {
    if (pid <= 0 || seen.contains(pid)) return;
    HANDLE handle = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
    if (handle) { seen.insert(pid); handles.push_back(handle); }
  }
  bool stopped() const {
    for (HANDLE handle : handles) if (WaitForSingleObject(handle, 3000) != WAIT_OBJECT_0) return false;
    return true;
  }
};

struct Call {
  stemtex::LuaRenderResult result;
  QJsonObject record;
  bool clean = false;
};

struct Runner {
  const Profile &profile;
  stemtex::LuaBackend &backend;
  Processes &processes;
  std::array<Worker, 2> workers{};
  std::array<QString, 2> outputs{};
  int serial = 0;

  int slot(bool svg) const { return profile.managed && svg ? 1 : 0; }
  Call invoke(const QByteArray &body, bool svg, double width, double fontSize) {
    QElapsedTimer timer;
    timer.start();
    Call call;
    call.result = backend.render(body, width, fontSize, svg);
    const qint64 elapsed = timer.elapsed();
    const auto &result = call.result;
    call.record = {{"ok", result.ok}, {"wallMs", elapsed}, {"error", result.error},
                   {"outputFormat", svg ? "svg" : "pdf"}, {"outputPath", result.outputPath},
                   {"workerPid", result.workerPid}, {"page", result.pageNumber},
                   {"readyAfterCall", backend.ready()}, {"pidAfterCall", backend.workerPid()},
                   {"summary", jsonObject(result.summary)}};
    auto &state = workers[slot(svg)];
    if (result.ok) {
      state = {result.workerPid, result.pageNumber};
      outputs[slot(svg)] = result.outputPath;
    } else {
      const auto summary = jsonObject(result.summary);
      if (summary.value("checkpointRestored").toBool() || summary.value("workerRestarted").toBool() ||
          backend.workerPid() != state.pid)
        state = {backend.workerPid(), -1};
    }
    processes.track(backend.workerPid());
    return call;
  }

  Call valid(bool svg, double width, double fontSize, Worker expected, QStringList &errors,
             const QByteArray &afterValidBody = {}) {
    const QString marker = "REC" + QString::number(++serial).rightJustified(5, '0');
    Call call = invoke(validSnippet(marker, profile.cjk) + afterValidBody, svg, width, fontSize);
    QStringList failures;
    check(failures, call.result.ok, "Valid recovery input failed: " + call.result.error);
    if (call.result.ok) {
      if (expected.pid > 0)
        check(failures, call.result.workerPid == expected.pid, "Valid input unexpectedly replaced a retained worker");
      if (expected.page >= 0)
        check(failures, call.result.pageNumber == expected.page + 1, "Valid input did not advance exactly one page");
      else
        check(failures, call.result.pageNumber > 0, "Valid input returned no output page number");
      check(failures, call.result.issueFlags == 0 && !call.result.recoverable, "Converter reported a recoverable issue");
      check(failures, jsonObject(call.result.summary).value("converterMode").toString() == "daemon-dll",
            "Valid output did not use resident conversion");
      try { call.record.insert("inspection", inspectOutput(call.result, marker, width, svg, profile.cjk)); }
      catch (const std::exception &error) { failures.append(QString::fromUtf8(error.what())); }
    }
    call.clean = failures.isEmpty();
    call.record.insert("marker", marker);
    call.record.insert("checksPassed", call.clean);
    call.record.insert("failures", strings(failures));
    errors.append(failures);
    return call;
  }
};

QJsonObject statistics(std::vector<double> values) {
  if (values.empty()) return {{"count", 0}};
  std::sort(values.begin(), values.end());
  double total = 0;
  for (double value : values) total += value;
  const size_t middle = values.size() / 2;
  const double median = values.size() % 2 ? values[middle] : (values[middle - 1] + values[middle]) / 2;
  return {{"count", static_cast<int>(values.size())}, {"minMs", values.front()}, {"medianMs", median},
          {"p95Ms", values[static_cast<size_t>(std::ceil(values.size() * 0.95)) - 1]},
          {"maxMs", values.back()}, {"meanMs", total / values.size()}};
}

QJsonObject timings(const QJsonArray &cases) {
  std::vector<double> bad, detection, restart, restore, total, next, other;
  for (const auto &entry : cases) {
    const auto item = entry.toObject();
    const auto invalid = item.value("badInput").toObject();
    const auto summary = invalid.value("summary").toObject();
    if (invalid.contains("wallMs")) bad.push_back(invalid.value("wallMs").toDouble());
    if (summary.contains("failureDetectedMs")) detection.push_back(summary.value("failureDetectedMs").toDouble());
    if (summary.contains("restartMs")) restart.push_back(summary.value("restartMs").toDouble());
    if (summary.contains("restoreMs")) restore.push_back(summary.value("restoreMs").toDouble());
    if (summary.contains("errorTotalMs")) total.push_back(summary.value("errorTotalMs").toDouble());
    const auto same = item.value("nextSameOutput").toObject();
    if (same.value("ok").toBool()) next.push_back(same.value("wallMs").toDouble());
    const auto opposite = item.value("nextOtherOutput").toObject();
    if (opposite.value("ok").toBool()) other.push_back(opposite.value("wallMs").toDouble());
  }
  return {{"badInputWall", statistics(bad)}, {"failureDetection", statistics(detection)},
          {"workerRestart", statistics(restart)}, {"checkpointRestore", statistics(restore)},
          {"errorTotal", statistics(total)},
          {"nextSameOutputWall", statistics(next)}, {"nextOtherOutputWall", statistics(other)}};
}
}  // namespace

int main(int argc, char **argv) {
  QGuiApplication app(argc, argv);
  QTextStream out(stdout);
  const auto args = app.arguments();
  if (args.size() != 6 && args.size() != 7) {
    out << "Usage: stemtex-luatex-recovery-smoke REPO RUNTIME TEXLIVE ENGINE STATE "
           "[PROFILE[/CASE]|pollution|nested|conditional|default-timeout]\n";
    return 2;
  }
  const QString filter = args.size() == 7 ? args[6] : QString();
  const bool defaultTimeout = filter == "default-timeout";
  const bool conditionalOnly = filter == "conditional" || filter.endsWith("/conditional");
  const QString requestedCase = filter.contains('/') ? filter.section('/', 1) : QString();
  const bool pollutionOnly = filter == "pollution" || requestedCase == "pollution" ||
      requestedCase.startsWith("lua-") || requestedCase == "tex-state" || requestedCase.startsWith("package-");
  const bool nestedOnly = filter == "nested" || requestedCase == "nested" || requestedCase.startsWith("nested-");
  const bool hasRepeatOverride = qEnvironmentVariableIsSet("STEMTEX_RECOVERY_REPEATS");
  bool repeatOverrideValid = false;
  const int repeatOverride = qEnvironmentVariableIntValue("STEMTEX_RECOVERY_REPEATS", &repeatOverrideValid);
  const int repetitions = hasRepeatOverride ? repeatOverride
      : defaultTimeout || conditionalOnly || pollutionOnly || nestedOnly ? 1 : 2;
  stemtex::LuaBackendConfig config;
  config.repoRoot = QDir(args[1]).absolutePath();
  config.runtimeRoot = QDir(args[2]).absolutePath();
  config.texmfRoot = QDir(args[3]).absolutePath();
  config.enginePath = QFileInfo(args[4]).absoluteFilePath();
  config.stateRoot = QDir(args[5]).absolutePath();
  config.timeoutMs = 90000;
  QDir().mkpath(config.stateRoot);
  const QString runRoot = QDir(config.stateRoot).filePath("run-" + QUuid::createUuid().toString(QUuid::WithoutBraces));
  QDir().mkpath(runRoot);
  const QString reportPath = QDir(config.stateRoot).filePath("recovery-smoke-results.json");
  QJsonObject report{{"schemaVersion", 1}, {"status", "running"}, {"passed", false}, {"runRoot", runRoot},
                     {"filter", filter}, {"timeoutMs", config.timeoutMs}, {"timeoutMode", "production-default"},
                     {"engine", config.enginePath},
                     {"repeatsPerCase", repetitions},
                     {"timingMethod", "QElapsedTimer around synchronous backend calls; inspection excluded"}};
  if (hasRepeatOverride) report.insert("repeatOverride", qEnvironmentVariable("STEMTEX_RECOVERY_REPEATS"));
  QJsonArray profiles;
  QElapsedTimer total;
  total.start();
  bool passed = true;
  int executed = 0;
  auto persist = [&]() {
    report.insert("profiles", profiles);
    report.insert("elapsedMs", total.elapsed());
    saveReport(reportPath, report);
    saveReport(QDir(runRoot).filePath("recovery-smoke-results.json"), report);
  };
  try {
    persist();
    require(!hasRepeatOverride || (repeatOverrideValid && repeatOverride >= 1 && repeatOverride <= 1000),
            "STEMTEX_RECOVERY_REPEATS must be an integer from 1 to 1000");
    const std::vector<Profile> definitions{{"builtin", false, false}, {"latin", true, false}, {"rich-cjk", true, true}};
    std::vector<BadInput> badInputs{
      {"undefined-command", "\\StemTeXUndefinedRecovery"},
      {"missing-brace", "\\textbf{BROKEN"},
      {"unclosed-group", "{BROKEN"},
      {"missing-dollar", "$x+1"},
      {"mismatched-environment", "\\begin{equation}x=1\\end{align}"},
      {"directlua-error", "\\directlua{error(\"STEMTEX_TEST_LUA_ERROR\")}"},
      {"premature-end", "\\csname @@end\\endcsname"}
    };
    if (pollutionOnly) badInputs = pollutionInputs();
    if (nestedOnly) badInputs = {
      {"nested-tex-error", "\\directlua{stemtexRecovery.set(99);assert(stemtexRecovery.get()==99);"
                            "tex.error('STEMTEX OWNED TEX ERROR')}"},
      {"nested-runtoks-error", "\\directlua{stemtexRecovery.set(99);assert(stemtexRecovery.get()==99);"
          "tex.runtoks(function() tex.sprint(string.char(92)..'StemTeXUndefinedRecovery') end)}"},
      {"nested-tex-run", "\\directlua{stemtexRecovery.set(99);tex.run()}"},
      // No extra error after pcall: the engine's pending-abort guard must reject
      // the request even when user Lua catches the immediate reentry error.
      {"nested-pcall-tex-run", "\\directlua{stemtexRecovery.set(99);"
          "pcall(function() tex.run() end);stemtexRecovery.set(101)}"}
    };
    if (conditionalOnly) badInputs = {{"conditional", "\\iftrue UNCLOSED"}};
    if (defaultTimeout) badInputs = {{"infinite-loop", "\\loop\\iftrue\\repeat", false, "restart"}};
    const QString profileFilter = filter.section('/', 0, 0);
    const QString caseFilter = filter.contains('/') ? filter.section('/', 1) : QString();
    for (const auto &definition : definitions) {
      if (defaultTimeout && definition.name != "builtin") continue;
      if (!filter.isEmpty() && filter != "conditional" && filter != "pollution" && filter != "nested" && !defaultTimeout &&
          profileFilter != definition.name) continue;
      std::vector<BadInput> selected;
      for (const auto &input : badInputs)
        if ((!input.richOnly || definition.cjk) &&
            (caseFilter.isEmpty() || caseFilter == "pollution" || caseFilter == "nested" || caseFilter == input.name)) selected.push_back(input);
      if (selected.empty()) continue;
      QJsonObject profile{{"name", definition.name}, {"managed", definition.managed}, {"cjk", definition.cjk},
                           {"timeoutMs", config.timeoutMs}, {"repeatsPerCase", repetitions},
                           {"passed", false}, {"status", "initializing"}};
      const int profileIndex = profiles.size();
      profiles.append(profile);
      persist();
      QJsonArray cases;
      bool profilePassed = true;
      auto updateProfile = [&]() {
        profile.insert("cases", cases);
        profile.insert("timings", timings(cases));
        profiles[profileIndex] = profile;
        persist();
      };
      Processes processes;
      std::unique_ptr<stemtex::LuaBackend> backend;
      try {
        config.profileRoot = materialize(config, definition, QDir(runRoot).filePath("profiles"));
        profile.insert("profileRoot", config.profileRoot);
        backend = std::make_unique<stemtex::LuaBackend>(config);
        QString startupError;
        QElapsedTimer init;
        init.start();
        const bool ready = backend->initialize({}, &startupError);
        profile.insert("initializeMs", init.elapsed());
        require(ready, "Initialization failed: " + startupError);
        processes.track(backend->workerPid());
        Runner runner{definition, *backend, processes};
        runner.workers[0] = {backend->workerPid(), -1};
        QJsonArray warmup;
        QStringList warmErrors;
        for (bool svg : {false, true}) {
          if (svg && defaultTimeout) continue;
          const auto warm = runner.valid(svg, 320, 10, runner.workers[runner.slot(svg)], warmErrors);
          warmup.append(warm.record);
        }
        profile.insert("warmup", warmup);
        require(warmErrors.isEmpty(), "Warmup failed: " + warmErrors.join("; "));
        profile.insert("status", "running");
        updateProfile();
        out << definition.name << ": initialized " << profile.value("initializeMs").toInteger() << " ms; warmup passed\n";
        out.flush();
        const int repeats = repetitions;
        for (const auto &input : selected) for (int repeat = 1; repeat <= repeats; ++repeat) for (bool svg : {false, true}) {
          if (svg && defaultTimeout) continue;
          ++executed;
          QJsonObject item{{"name", input.name}, {"repeat", repeat}, {"outputFormat", svg ? "svg" : "pdf"},
                            {"observational", input.observational}, {"timeoutMs", config.timeoutMs},
                            {"expectedRecovery", input.expectedRecovery},
                            {"status", "running"}, {"stage", "bad-input"}};
          QStringList failures;
          const int active = runner.slot(svg), opposite = runner.slot(!svg);
          Worker previous = runner.workers[active];
          const Worker otherBefore = runner.workers[opposite];
          item.insert("workerPidBefore", previous.pid);
          item.insert("otherWorkerPidBefore", otherBefore.pid);
          const int caseIndex = cases.size();
          cases.append(item);
          profile.insert("currentCase", input.name + (svg ? "/svg/" : "/pdf/") + QString::number(repeat));
          updateProfile();
          try {
            if (!input.preparation.isEmpty()) {
              const auto prepared = runner.valid(svg, 300, 10, runner.workers[active], failures, input.preparation);
              item.insert("successfulMutation", prepared.record);
              previous = runner.workers[active];
              item.insert("workerPidBefore", previous.pid);
              cases[caseIndex] = item;
              updateProfile();
            }
            const QString publishedPath = runner.outputs[active];
            const QByteArray publishedHash = fileHash(publishedPath);
            item.insert("previousOutput", publishedPath);
            item.insert("previousOutputSha256", QString::fromLatin1(publishedHash));
            QByteArray badSource = poisoned(input);
            QString mutationMarker;
            if (!input.preparation.isEmpty()) {
              mutationMarker = QString("STEMTEX_TEST_MUTATION_OK_%1_%2_%3_%4_%5")
                  .arg(definition.name, input.name, QString::number(repeat), svg ? "svg" : "pdf",
                       QUuid::createUuid().toString(QUuid::WithoutBraces));
              require(badSource.contains("@STEMTEX_MUTATION_PROOF@"), "Mutation probe has no proof insertion point");
              badSource.replace("@STEMTEX_MUTATION_PROOF@", mutationProofSource(mutationMarker.toUtf8()));
              item.insert("mutationPersistenceMarker", mutationMarker);
            }
            const auto bad = runner.invoke(badSource, svg, 300, 10);
            item.insert("badInput", bad.record);
            if (!mutationMarker.isEmpty()) {
              QString transcript = bad.result.error;
              transcript.remove('\r');
              const bool observed = transcript.contains('\n' + mutationMarker + '\n');
              item.insert("mutationPersistenceVerified", observed);
              check(failures, observed, "Successful request mutation was not verified in the following input");
            }
            item.insert("stage", "next-same-output");
            cases[caseIndex] = item;
            updateProfile();
            if (!input.observational) check(failures, !bad.result.ok, "Malformed input unexpectedly succeeded");
            if (!bad.result.ok) {
              const auto summary = bad.record.value("summary").toObject();
              check(failures, !bad.result.error.isEmpty(), "Failure returned no error text");
              for (const QString &key : {QStringLiteral("failureDetectedMs"), QStringLiteral("errorTotalMs")})
                check(failures, summary.contains(key) && summary.value(key).toDouble(-1) >= 0, "Missing failure timing: " + key);
              const bool restored = summary.value("checkpointRestored").toBool();
              const bool restarted = summary.value("workerRestarted").toBool();
              check(failures, restored != restarted, "Failure did not report exactly one recovery mechanism");
              check(failures, backend->ready() && backend->workerPid() > 0, "Failure did not leave a ready worker");
              if (restored) {
                check(failures, summary.value("recoveryMode").toString() == "checkpoint", "Checkpoint recovery mode is inconsistent");
                check(failures, backend->workerPid() == previous.pid, "Checkpoint recovery changed the worker PID");
                check(failures, summary.contains("restoreMs") && summary.value("restoreMs").toDouble(-1) >= 0,
                      "Missing checkpoint restore timing");
              }
              if (restarted) {
                check(failures, summary.value("recoveryMode").toString() == "restart", "Restart recovery mode is inconsistent");
                check(failures, backend->workerPid() != previous.pid, "Restart recovery retained the failed worker PID");
                check(failures, summary.contains("restartMs") && summary.value("restartMs").toDouble(-1) >= 0,
                      "Missing restart timing");
              }
              if (input.expectedRecovery == "checkpoint")
                check(failures, restored && !restarted, "Ordinary error fell back to process restart instead of checkpoint recovery");
              if (input.expectedRecovery == "restart")
                check(failures, restarted && !restored, "Fatal timeout did not restart the worker");
              check(failures, summary.value("failedWorkerPid").toInteger() == previous.pid, "Error summary reported the wrong failed PID");
            }
            try {
              const bool retained = fileHash(publishedPath) == publishedHash;
              item.insert("previousOutputUnchanged", retained);
              check(failures, retained, "Recovery modified a previously published output");
            } catch (const std::exception &error) {
              item.insert("previousOutputUnchanged", false);
              failures.append(QString::fromUtf8(error.what()));
            }
            const Worker afterBad = runner.workers[active];
            const int variation = (repeat - 1) % 2 + 1;
            const auto next = runner.valid(svg, 280 + variation * 10, 10 + variation, afterBad, failures);
            item.insert("nextSameOutput", next.record);
            item.insert("stage", "next-other-output");
            cases[caseIndex] = item;
            updateProfile();
            if (input.name == "conditional") {
              item.insert("badInputAccepted", bad.result.ok);
              item.insert("nextInputClean", next.clean);
              item.insert("conditionalLeakDetected", next.result.error.contains("CONDITIONAL DEPTH LEAK"));
            }
            bool needsRepair = !next.clean;
            if (!defaultTimeout) {
              const Worker expectedOther = definition.managed ? otherBefore : runner.workers[opposite];
              const auto other = runner.valid(!svg, 320, 10, expectedOther, failures);
              item.insert("nextOtherOutput", other.record);
              needsRepair = needsRepair || !other.clean;
            }
            if (needsRepair) {
              QJsonArray repair;
              QStringList repairErrors;
              for (bool format : {false, true}) {
                if (format && defaultTimeout) continue;
                repair.append(runner.valid(format, 320, 10, runner.workers[runner.slot(format)], repairErrors).record);
              }
              item.insert("rebaseline", repair);
              item.insert("rebaselinePassed", repairErrors.isEmpty());
              failures.append(repairErrors);
            }
            try {
              const bool retained = fileHash(publishedPath) == publishedHash;
              item.insert("previousOutputUnchangedAfterRecovery", retained);
              check(failures, retained, "A later recovery render overwrote a previously published output");
            } catch (const std::exception &error) {
              item.insert("previousOutputUnchangedAfterRecovery", false);
              failures.append(QString::fromUtf8(error.what()));
            }
          } catch (const std::exception &error) {
            failures.append(QString::fromUtf8(error.what()));
          }
          item.insert("passed", failures.isEmpty());
          item.insert("failures", strings(failures));
          item.insert("status", "complete");
          item.remove("stage");
          cases[caseIndex] = item;
          profilePassed = profilePassed && failures.isEmpty();
          updateProfile();
          const auto bad = item.value("badInput").toObject();
          const auto summary = bad.value("summary").toObject();
          out << definition.name << '/' << input.name << '/' << (svg ? "svg" : "pdf") << '/' << repeat
              << ": " << (failures.isEmpty() ? "PASS" : "FAIL") << " bad=" << bad.value("wallMs").toInteger()
              << " detect=" << summary.value("failureDetectedMs").toInteger(-1)
              << " restore=" << summary.value("restoreMs").toInteger(-1)
              << " restart=" << summary.value("restartMs").toInteger(-1)
              << " next=" << item.value("nextSameOutput").toObject().value("wallMs").toInteger() << " ms\n";
          if (!failures.isEmpty()) out << "  " << failures.join("; ") << '\n';
          out.flush();
        }
      } catch (const std::exception &error) {
        profilePassed = false;
        profile.insert("error", QString::fromUtf8(error.what()));
        out << definition.name << ": FAILED " << error.what() << '\n';
        out.flush();
      }
      backend.reset();
      const bool workersStopped = processes.stopped();
      profilePassed = profilePassed && workersStopped;
      profile.insert("workersStopped", workersStopped);
      profile.insert("passed", profilePassed);
      profile.insert("status", "complete");
      profile.remove("currentCase");
      updateProfile();
      passed = passed && profilePassed;
    }
    require(!profiles.isEmpty(), "No recovery cases matched filter: " + filter);
  } catch (const std::exception &error) {
    passed = false;
    report.insert("error", QString::fromUtf8(error.what()));
    out << "FAILED: " << error.what() << '\n';
  }
  report.insert("executedCases", executed);
  report.insert("passed", passed);
  report.insert("status", "complete");
  try { persist(); }
  catch (const std::exception &error) { out << error.what() << '\n'; return 1; }
  out << (passed ? "PASS: " : "FAIL: ") << reportPath << '\n';
  return passed ? 0 : 1;
}
