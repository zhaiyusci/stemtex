#include "luatex_backend.h"
#include "../profile-creator/stemtex_profile.h"

#include <QDir>
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
#include <QProcess>
#include <QProcessEnvironment>
#include <QSvgRenderer>
#include <QTextStream>
#include <QUuid>
#include <QtMath>

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

QByteArray readFile(const QString &path) {
  QFile file(path);
  require(file.open(QIODevice::ReadOnly), QStringLiteral("Cannot read ") + path);
  return file.readAll();
}

QJsonObject parseJson(const QByteArray &text, const QString &source) {
  QJsonParseError error;
  const auto json = QJsonDocument::fromJson(text, &error);
  require(error.error == QJsonParseError::NoError && json.isObject(), source + ": invalid JSON");
  return json.object();
}

QSize rasterSize(const QSizeF &points) {
  require(points.isValid() && points.width() > 0 && points.height() > 0,
          QStringLiteral("Output page has invalid dimensions"));
  QSize size(qMax(1, qCeil(points.width() * 2.0)), qMax(1, qCeil(points.height() * 2.0)));
  if (size.width() > 2048 || size.height() > 2048) size.scale(2048, 2048, Qt::KeepAspectRatio);
  return size;
}

struct PixelEvidence {
  int ink = 0, red = 0, blue = 0, green = 0;
  QRect redBounds;
};

PixelEvidence inspectPixels(const QImage &source) {
  const auto image = source.convertToFormat(QImage::Format_ARGB32);
  PixelEvidence result;
  for (int y = 0; y < image.height(); ++y) {
    const auto *row = reinterpret_cast<const QRgb *>(image.constScanLine(y));
    for (int x = 0; x < image.width(); ++x) {
      const auto pixel = row[x];
      if (qAlpha(pixel) < 128) continue;
      const int red = qRed(pixel), green = qGreen(pixel), blue = qBlue(pixel);
      if (qGray(pixel) < 220) ++result.ink;
      if (red > 180 && green < 80 && blue < 80) {
        ++result.red;
        result.redBounds = result.redBounds.united(QRect(x, y, 1, 1));
      }
      if (blue > 140 && red < 100 && green < 140) ++result.blue;
      if (green > 60 && green > red + 40 && green > blue + 40) ++result.green;
    }
  }
  return result;
}

struct ProfileCase {
  QString name;
  const char *cjk = "none";
  std::vector<const char *> packages;
  QString body;
  QString marker;
  QString chinese;
  bool graphics = false;
  bool generatedWarmup = false;
};

struct ProcessHandles {
  std::vector<HANDLE> values;
  ~ProcessHandles() { for (HANDLE value : values) CloseHandle(value); }
  void track(qint64 pid) {
    HANDLE value = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
    require(value != nullptr, QStringLiteral("Cannot inspect live worker PID %1").arg(pid));
    values.push_back(value);
  }
  void verifyStopped() const {
    for (HANDLE value : values)
      require(WaitForSingleObject(value, 3000) == WAIT_OBJECT_0,
              "A parked LuaHBTeX worker survived backend destruction");
  }
};

QJsonObject inspectOutput(const stemtex::LuaRenderResult &result, const ProfileCase &test,
                          double width, bool svg) {
  QSizeF points;
  QImage raster;
  QString text;
  if (svg) {
    QSvgRenderer document(result.outputPath);
    require(document.isValid(), "Invalid SVG: " + result.outputPath);
    points = document.viewBoxF().size();
    raster = QImage(rasterSize(points), QImage::Format_ARGB32_Premultiplied);
    raster.fill(Qt::white);
    QPainter painter(&raster);
    document.render(&painter);
  } else {
    QPdfDocument document;
    require(document.load(result.outputPath) == QPdfDocument::Error::None && document.pageCount() == 1,
            "Expected one valid PDF page: " + result.outputPath);
    points = document.pagePointSize(0);
    const auto page = document.render(0, rasterSize(points));
    require(!page.isNull(), "Cannot rasterize " + result.outputPath);
    raster = QImage(page.size(), QImage::Format_ARGB32_Premultiplied);
    raster.fill(Qt::white);
    {
      QPainter painter(&raster);
      painter.drawImage(0, 0, page);
    }
    text = document.getAllText(0).text();
    require(text.contains(test.marker), "PDF lost the current profile marker: " + text.left(1000));
    require(text.contains("USERPREAMBLE"), "Generated profile did not execute its user preamble: " + text.left(1000));
    if (test.generatedWarmup)
      require(text.contains("Virtual font probe."), "PDF lost the legacy virtual-font probe");
    if (!test.chinese.isEmpty())
      require(text.contains(test.chinese), "PDF is missing the expected Chinese text: " + text.left(1000));
  }
  require(!raster.isNull(), "Cannot rasterize " + result.outputPath);
  const QString previewPath = result.outputPath + ".png";
  require(raster.save(previewPath), "Cannot save rendered evidence: " + previewPath);
  require(std::abs(points.width() - (width + 2.0) * 72.0 / 72.27) < 0.3,
          QStringLiteral("Generated profile lost the requested page width: %1").arg(points.width()));
  const auto pixels = inspectPixels(raster);
  require(pixels.ink > 50, "Generated profile produced a blank page: " + result.outputPath);
  if (test.graphics) {
    require(pixels.red > 100 && pixels.blue > 20 && pixels.green > 100,
            QStringLiteral("DVI graphics drivers lost color: red=%1 blue=%2 green=%3")
                .arg(pixels.red).arg(pixels.blue).arg(pixels.green));
    // The only red object is a 10 x 6 pt rule scaled by graphicx to 20 x 12 pt.
    require(std::abs(pixels.redBounds.width() - 40) <= 3 &&
                std::abs(pixels.redBounds.height() - 24) <= 3,
            QStringLiteral("graphicx scaling was not applied: red rectangle %1 x %2 px")
                .arg(pixels.redBounds.width()).arg(pixels.redBounds.height()));
  }
  return {{"widthBp", points.width()}, {"heightBp", points.height()}, {"text", text},
          {"inkPixels", pixels.ink}, {"redPixels", pixels.red}, {"bluePixels", pixels.blue},
          {"greenPixels", pixels.green}, {"preview", previewPath}};
}

QString materialize(const StemTeXProfileContext &context, const ProfileCase &test,
                    const QString &profilesRoot) {
  const QByteArray name = test.name.toUtf8(), root = profilesRoot.toUtf8();
  const QByteArray userPreamble = R"TEX(\makeatletter
\ExplSyntaxOn
\ifdefined\StemTeXOutputSVG
  \str_if_eq:VnF \c_sys_backend_str {dvisvgm}
    {\errmessage{Generated Lua SVG profile selected the wrong LaTeX backend}}
\else
  \str_if_eq:VnF \c_sys_backend_str {dvipdfmx}
    {\errmessage{Generated Lua PDF profile selected the wrong LaTeX backend}}
\fi
\cs_if_exist:NT \Gin@driver
  {
    \ifdefined\StemTeXOutputSVG
      \str_if_eq:VnF \Gin@driver {dvisvgm.def}
        {\errmessage{Generated Lua SVG profile selected the wrong graphics driver}}
    \else
      \str_if_eq:VnF \Gin@driver {dvipdfmx.def}
        {\errmessage{Generated Lua PDF profile selected the wrong graphics driver}}
    \fi
  }
\ExplSyntaxOff
\makeatother
\newcommand\ProfileSmokeUserMacro{\mbox{USERPREAMBLE}}
)TEX";
  StemTeXProfileSpecV4 spec{};
  spec.name_utf8 = name.constData();
  spec.text_font_id_utf8 = "latin-modern";
  spec.math_font_id_utf8 = "latin-modern-math";
  spec.cjk_font_id_utf8 = test.cjk;
  spec.package_ids_utf8 = test.packages.data();
  spec.package_count = test.packages.size();
  spec.user_preamble_utf8 = userPreamble.constData();
  spec.engine = STEMTEX_PROFILE_ENGINE_LUAHBTEX;
  StemTeXProfileErrorCode code = STEMTEX_PROFILE_OK;
  char *result = nullptr, *error = nullptr;
  const bool ok = stemtex_profile_materialize_v4(&context, &spec, root.constData(), &result, &code, &error) != 0;
  const QByteArray json = result ? QByteArray(result) : QByteArray();
  const QString message = error ? QString::fromUtf8(error) : QString();
  stemtex_profile_free_string(result);
  stemtex_profile_free_string(error);
  require(ok, QStringLiteral("Cannot materialize %1 (code %2): %3").arg(test.name).arg(code).arg(message));
  const auto metadata = parseJson(json, "materialize result");
  const QString path = metadata.value("profilePath").toString();
  require(!path.isEmpty() && QFileInfo::exists(QDir(path).filePath("profile.json")),
          "Materialize did not produce a profile manifest");
  return path;
}

bool hasFont(const StemTeXProfileContext &context, const QString &id) {
  StemTeXProfileErrorCode code = STEMTEX_PROFILE_OK;
  char *error = nullptr;
  char *value = stemtex_profile_font_catalog_for_engine_json(
      &context, STEMTEX_PROFILE_ENGINE_LUAHBTEX, &code, &error);
  const QByteArray json = value ? QByteArray(value) : QByteArray();
  const QString message = error ? QString::fromUtf8(error) : QString();
  stemtex_profile_free_string(value);
  stemtex_profile_free_string(error);
  require(!json.isEmpty(), "Cannot inspect the Lua font catalog: " + message);
  const auto catalog = parseJson(json, "font catalog");
  for (const auto &value : catalog.value("fonts").toArray()) {
    const auto font = value.toObject();
    if (font.value("id").toString() == id && font.value("role").toString() == "cjk")
      return font.value("available").toBool();
  }
  return false;
}

QJsonObject verifyWrongEngine(const QString &texmfRoot, const QString &profileRoot, const QString &stateRoot) {
  const QString executable = QDir(texmfRoot).filePath("bin/windows/xelatex.exe");
  require(QFileInfo::exists(executable), "XeLaTeX is required for the wrong-engine guard check");
  const QString output = QDir(stateRoot).filePath("wrong-engine");
  require(QDir().mkpath(output + "/cache"), "Cannot create wrong-engine test directory");
  auto environment = QProcessEnvironment::systemEnvironment();
  environment.insert("TEXMFROOT", texmfRoot);
  environment.insert("TEXMFCNF", QDir(texmfRoot).filePath("texmf-dist/web2c"));
  environment.insert("TEXMFVAR", output + "/cache");
  environment.insert("TEXMFCACHE", output + "/cache");
  environment.insert("TEXINPUTS", profileRoot + "//;");
  environment.insert("PATH", QDir(texmfRoot).filePath("bin/windows") + ";" + environment.value("PATH"));
  QProcess process;
  process.setWorkingDirectory(profileRoot);
  process.setProcessEnvironment(environment);
  process.setProcessChannelMode(QProcess::MergedChannels);
  process.start(executable, {"-no-pdf", "-interaction=nonstopmode", "-halt-on-error", "-no-shell-escape",
                             "-output-directory=" + output, "warmup.tex"});
  require(process.waitForStarted(10000), "Cannot start XeLaTeX for the guard check");
  const bool finished = process.waitForFinished(30000);
  if (!finished) { process.kill(); process.waitForFinished(5000); }
  const QByteArray log = process.readAll();
  QFile file(QDir(output).filePath("console.log"));
  if (file.open(QIODevice::WriteOnly)) file.write(log);
  require(finished && process.exitStatus() == QProcess::NormalExit && process.exitCode() != 0 &&
              log.contains("LuaHBTeX is required"),
          "Generated Lua profile did not reject XeTeX with its engine guard: " + QString::fromUtf8(log.right(3000)));
  return {{"rejected", true}, {"exitCode", process.exitCode()}, {"log", QDir(output).filePath("console.log")}};
}

}  // namespace

int main(int argc, char **argv) {
  QGuiApplication application(argc, argv);
  const auto args = application.arguments();
  QTextStream out(stdout);
  if (args.size() != 6 && args.size() != 7) {
    out << "Usage: stemtex-luatex-profile-smoke REPO_ROOT RUNTIME_ROOT TEXLIVE_ROOT ENGINE_EXE STATE_ROOT [CASE_NAME]\n";
    return 2;
  }
  stemtex::LuaBackendConfig config;
  config.repoRoot = QDir(args[1]).absolutePath();
  config.runtimeRoot = QDir(args[2]).absolutePath();
  config.texmfRoot = QDir(args[3]).absolutePath();
  config.enginePath = QFileInfo(args[4]).absoluteFilePath();
  config.stateRoot = QDir(args[5]).absolutePath();
  config.timeoutMs = 90000;
  QDir().mkpath(config.stateRoot);
  const QString runRoot = QDir(config.stateRoot).filePath("run-" + QUuid::createUuid().toString(QUuid::WithoutBraces));
  const QString profilesRoot = QDir(runRoot).filePath("profiles");
  const QByteArray texmf = config.texmfRoot.toUtf8();
  StemTeXProfileContext context{texmf.constData()};
  QJsonObject report{{"passed", false}, {"runRoot", runRoot}};
  QJsonArray profiles;
  QElapsedTimer timer;
  timer.start();
  int exitCode = 1;
  try {
    std::vector<ProfileCase> cases{
      {"lua-latin-math", "none", {"mathtools", "mhchem", "siunitx", "array", "booktabs"},
       QStringLiteral("LATINPROFILE. \\ProfileSmokeUserMacro.\\par\n"
         "\\[a\\coloneqq b,\\quad\\sum_{k=1}^n k=\\frac{n(n+1)}2\\]\n"
         "\\ce{2H2 + O2 -> 2H2O}. \\SI{9.81}{\\metre\\per\\second\\squared}.\\par\n"
         "\\begin{tabular}{lr}\\toprule Quantity & Value\\\\\\midrule Test & 1\\\\\\bottomrule\\end{tabular}\n"),
       "LATINPROFILE", {}, false},
      {"lua-graphics%{test}", "none", {"xcolor", "graphicx", "tikz", "adjustbox"},
       QStringLiteral("GRAPHICSPROFILE. \\ProfileSmokeUserMacro.\\par\n"
         "\\textcolor{blue}{Blue text}.\\par\n"
         "\\scalebox{2}{\\textcolor{red}{\\rule{10pt}{6pt}}}\\quad\n"
         "\\begin{tikzpicture}[baseline]\\fill[green!60!black] (0,0) rectangle (18pt,10pt);"
         "\\draw[blue,line width=1pt] (0,0)--(18pt,10pt);\\end{tikzpicture}\\par\n"
         "\\adjustbox{frame}{adjustbox}.\n"),
       "GRAPHICSPROFILE", {}, true},
      {"lua-fandol", "fandol-song", {"mathtools"},
       QString::fromUtf8("FANDOLPROFILE. \\ProfileSmokeUserMacro.\\par\n"
         "中文数学排版，标点和字体。\\par\\textsf{中文无衬线文字}。\\par\n"
         "\\textbf{中文粗体}，\\textit{中文斜体}。\\par\n"
         "\\[E=mc^2,\\quad a\\coloneqq b\\]\n"),
       "FANDOLPROFILE", QString::fromUtf8("中文数学排版"), false}
    };
    if (hasFont(context, "simsun")) {
      cases.push_back({"lua-simsun", "simsun", {"mathtools"},
        QString::fromUtf8("SIMSUNPROFILE. \\ProfileSmokeUserMacro.\\par\n"
          "中文数学排版，宋体与黑体。\\par\\textsf{中文无衬线文字}。\\par\n"
          "\\textbf{中文粗体}，\\textit{中文斜体}。\\par\n"
          "\\[\\int_0^1x^2\\,dx=\\frac13\\]\n"),
        "SIMSUNPROFILE", QString::fromUtf8("中文数学排版"), false});
    } else {
      report.insert("simsunSkipped", "SimSun/SimHei recipe is unavailable on this system");
    }
    cases.insert(cases.begin() + 2, ProfileCase{"lua-all-packages", "none",
      {"mathtools", "mhchem", "physics", "siunitx", "xcolor", "cancel", "graphicx", "array",
       "booktabs", "tabularx", "multirow", "adjustbox", "enumitem", "qrcode", "tikz", "pgfplots",
       "tikz-cd", "circuitikz", "forest", "chemfig", "quantikz"},
      {}, "ALLPACKAGEPROFILE", {}, false, true});
    if (args.size() == 7) {
      std::vector<ProfileCase> selected;
      for (const auto &test : cases)
        if (test.name == args[6]) selected.push_back(test);
      require(!selected.empty(), "Unknown or unavailable profile case: " + args[6]);
      cases = std::move(selected);
      report.insert("caseFilter", args[6]);
    }
    QString firstProfile;
    for (auto test : cases) {
      config.profileRoot = materialize(context, test, profilesRoot);
      if (test.generatedWarmup) {
        for (const QString &relative : {QStringLiteral("texmf-dist/fonts/tfm/adobe/courier/pcrr8t.tfm"),
                                        QStringLiteral("texmf-dist/fonts/vf/adobe/courier/pcrr8t.vf")}) {
          const QString prerequisite = QDir(config.texmfRoot).filePath(relative);
          require(QFileInfo::exists(prerequisite), "Virtual-font smoke prerequisite is missing: " + prerequisite);
        }
        const QString warmup = QString::fromUtf8(readFile(QDir(config.profileRoot).filePath("warmup.tex")));
        const QString begin = QStringLiteral("\\begin{preview}");
        const QString end = QStringLiteral("\\end{preview}");
        const qsizetype first = warmup.indexOf(begin);
        const qsizetype last = first < 0 ? -1 : warmup.indexOf(end, first + begin.size());
        require(first >= 0 && last > first, "Generated all-package warmup has no preview body");
        test.body = test.marker + ". \\ProfileSmokeUserMacro.\\par\n" +
                    warmup.mid(first + begin.size(), last - first - begin.size());
        test.body += "\\par\\begingroup\\font\\StemTeXVirtual=pcrr8t at 10pt "
                     "\\StemTeXVirtual Virtual font probe.\\par\\endgroup\n";
      }
      if (firstProfile.isEmpty()) firstProfile = config.profileRoot;
      out << test.name << ": materialized " << config.profileRoot << '\n';
      out.flush();
      QJsonObject profile{{"name", test.name}, {"path", config.profileRoot},
                          {"manifest", parseJson(readFile(QDir(config.profileRoot).filePath("profile.json")), "manifest")}};
      const auto drivers = profile.value("manifest").toObject().value("outputDrivers").toObject();
      require(drivers.value("pdf").toString() == "dvipdfmx" && drivers.value("svg").toString() == "dvisvgm",
              "Generated profile does not declare both output drivers");
      ProcessHandles processes;
      auto backend = std::make_unique<stemtex::LuaBackend>(config);
      QString error;
      const bool ready = backend->initialize([&out](const QString &message) {
        out << "  " << message << '\n';
        out.flush();
      }, &error);
      require(ready, test.name + " initialization failed: " + error);
      qint64 pids[2]{backend->workerPid(), 0};
      processes.track(pids[0]);
      QJsonArray outputs;
      QStringList workerRoots;
      int pages[2]{0, 0};
      int index = 0;
      const double widths[]{320.0, 280.0, 300.0, 260.0, 320.0};
      const double fontSizes[]{10.0, 14.0, 12.0, 11.0, 10.0};
      for (bool svg : {false, false, true, true, false}) {
        const int format = svg ? 1 : 0;
        const double width = widths[index];
        const auto result = backend->render(test.body.toUtf8(), width, fontSizes[index], svg);
        require(result.ok, test.name + " render failed: " + result.error);
        if (!pids[format]) {
          pids[format] = result.workerPid;
          processes.track(pids[format]);
        }
        require(result.workerPid == pids[format] && result.pageNumber == pages[format] + 1,
                test.name + " did not retain its per-format worker and advance one page");
        if (svg) require(pids[0] != pids[1], "PDF and SVG did not use distinct driver-specific workers");
        pages[format] = result.pageNumber;
        const QString workerRoot = QFileInfo(result.outputPath).absolutePath();
        if (!workerRoots.contains(workerRoot)) workerRoots.append(workerRoot);
        const auto summary = parseJson(result.summary.toUtf8(), "render summary");
        require(summary.value("converterMode").toString() == "daemon-dll", "Expected resident conversion");
        require(result.issueFlags == 0 && !result.recoverable,
                test.name + " converter reported an issue: " + result.outcomeMessage);
        outputs.append(QJsonObject{{"path", result.outputPath}, {"summary", summary},
                                    {"inspection", inspectOutput(result, test, width, svg)}});
        out << "  " << (svg ? "SVG" : "PDF") << ": PID=" << result.workerPid
            << " page=" << result.pageNumber << '\n';
        out.flush();
        ++index;
      }
      if (test.graphics) {
        const qint64 originalSvgPid = pids[1];
        const auto invalid = backend->render("\\StemTeXProfileSmokeUndefinedCommand\n", 300.0, 10.0, true);
        require(!invalid.ok && !invalid.error.isEmpty(), "Malformed SVG request was not rejected");
        const auto retainedPdf = backend->render(test.body.toUtf8(), 280.0, 10.0, false);
        require(retainedPdf.ok, "PDF failed after an SVG error: " + retainedPdf.error);
        require(retainedPdf.workerPid == pids[0] && retainedPdf.pageNumber == pages[0] + 1,
                "An SVG error restarted or lost the parked PDF worker");
        pages[0] = retainedPdf.pageNumber;
        outputs.append(QJsonObject{{"case", "pdf-after-svg-error"}, {"path", retainedPdf.outputPath},
                                    {"summary", parseJson(retainedPdf.summary.toUtf8(), "render summary")},
                                    {"inspection", inspectOutput(retainedPdf, test, 280.0, false)}});
        const auto recoveredSvg = backend->render(test.body.toUtf8(), 300.0, 12.0, true);
        require(recoveredSvg.ok, "SVG recovery failed: " + recoveredSvg.error);
        require(recoveredSvg.workerPid != originalSvgPid && recoveredSvg.pageNumber == 1,
                "SVG recovery did not start a new independent worker");
        pids[1] = recoveredSvg.workerPid;
        pages[1] = recoveredSvg.pageNumber;
        processes.track(pids[1]);
        const QString recoveredRoot = QFileInfo(recoveredSvg.outputPath).absolutePath();
        if (!workerRoots.contains(recoveredRoot)) workerRoots.append(recoveredRoot);
        outputs.append(QJsonObject{{"case", "recovered-svg"}, {"path", recoveredSvg.outputPath},
                                    {"summary", parseJson(recoveredSvg.summary.toUtf8(), "render summary")},
                                    {"inspection", inspectOutput(recoveredSvg, test, 300.0, true)}});
        profile.insert("svgErrorPreservedPdfWorker", true);
        profile.insert("originalSvgWorkerPid", originalSvgPid);
        out << "  SVG error recovery: PDF PID retained; SVG PID replaced\n";
        out.flush();
      }
      backend.reset();
      processes.verifyStopped();
      for (const auto &workerRoot : workerRoots) {
        const QByteArray transcript = readFile(QDir(workerRoot).filePath("host-transcript.log"));
        require(!transcript.contains("Missing character:"), test.name + " reported missing glyphs");
      }
      profile.insert("pdfWorkerPid", pids[0]);
      profile.insert("svgWorkerPid", pids[1]);
      profile.insert("retainedPdfWorkerAfterSvg", true);
      profile.insert("workersStopped", true);
      profile.insert("outputs", outputs);
      profiles.append(profile);
    }
    report.insert("wrongEngineGuard", verifyWrongEngine(config.texmfRoot, firstProfile, runRoot));
    report.insert("passed", true);
    exitCode = 0;
  } catch (const std::exception &error) {
    report.insert("error", QString::fromUtf8(error.what()));
    out << "FAILED: " << error.what() << '\n';
  }
  report.insert("profiles", profiles);
  report.insert("elapsedMs", timer.elapsed());
  const QString reportPath = QDir(config.stateRoot).filePath("profile-smoke-results.json");
  QFile result(reportPath);
  if (!result.open(QIODevice::WriteOnly) || result.write(QJsonDocument(report).toJson()) < 0) {
    out << "FAILED: cannot save " << reportPath << '\n';
    return 1;
  }
  out << (exitCode == 0 ? "PASS: " : "FAIL: ") << reportPath << '\n';
  return exitCode;
}
