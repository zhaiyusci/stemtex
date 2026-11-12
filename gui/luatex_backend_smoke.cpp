#include "luatex_backend.h"
#include "../cpp-daemon/stemtex_renderer.h"

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
#include <QProcessEnvironment>
#include <QMap>
#include <QSvgRenderer>
#include <QTextStream>
#include <QXmlStreamReader>
#include <QtMath>

#include <chrono>
#include <cmath>
#include <memory>
#include <stdexcept>
#include <thread>
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

QByteArray readOutput(const QString &path) {
  QFile file(path);
  require(file.open(QIODevice::ReadOnly), QStringLiteral("Cannot read output: ") + path);
  return file.readAll();
}

bool hasInk(const QImage &source) {
  const QImage image = source.convertToFormat(QImage::Format_ARGB32);
  for (int y = 0; y < image.height(); ++y) {
    const auto *row = reinterpret_cast<const QRgb *>(image.constScanLine(y));
    for (int x = 0; x < image.width(); ++x) {
      if (qAlpha(row[x]) > 32 && qGray(row[x]) < 220) return true;
    }
  }
  return false;
}

QSize rasterSize(const QSizeF &points) {
  require(points.isValid() && points.width() > 0 && points.height() > 0,
          QStringLiteral("Output has invalid page dimensions"));
  QSize result(qMax(1, qCeil(points.width() * 2.0)), qMax(1, qCeil(points.height() * 2.0)));
  if (result.width() > 2048 || result.height() > 2048) result.scale(2048, 2048, Qt::KeepAspectRatio);
  return result;
}

QJsonObject inspectPdf(const QString &path, const QString &marker, const QString &previousMarker = {}) {
  QPdfDocument pdf;
  require(pdf.load(path) == QPdfDocument::Error::None, QStringLiteral("Qt could not load PDF: ") + path);
  require(pdf.pageCount() == 1, QStringLiteral("Expected exactly one selected PDF page: ") + path);
  const QString text = pdf.getAllText(0).text();
  require(text.contains(marker), QStringLiteral("Selected PDF page is missing marker %1; text: %2")
                                    .arg(marker, text.left(2000)));
  require(previousMarker.isEmpty() || !text.contains(previousMarker),
          QStringLiteral("Selected PDF contains a previous request's content: ") + previousMarker);
  const QSizeF points = pdf.pagePointSize(0);
  const QImage raster = pdf.render(0, rasterSize(points));
  require(!raster.isNull() && hasInk(raster), QStringLiteral("Qt rendered a blank PDF page: ") + path);
  return {{"pages", pdf.pageCount()}, {"widthPt", points.width()}, {"heightPt", points.height()},
          {"text", text.left(4096)}, {"visibleInk", true}};
}

QJsonObject inspectSvg(const QString &path, bool allowEmpty = false) {
  QSvgRenderer svg(path);
  require(svg.isValid(), QStringLiteral("Qt could not load SVG: ") + path);
  const QSizeF points = svg.viewBoxF().size();
  QImage raster(rasterSize(points), QImage::Format_ARGB32_Premultiplied);
  raster.fill(Qt::white);
  QPainter painter(&raster);
  svg.render(&painter);
  painter.end();
  const bool ink = hasInk(raster);
  require(allowEmpty || ink, QStringLiteral("Qt rendered a blank SVG page: ") + path);
  return {{"viewBoxWidth", points.width()}, {"viewBoxHeight", points.height()}, {"visibleInk", ink}};
}

void requireSvgRule(const QString &path, const QSizeF &rulePt) {
  QFile file(path);
  require(file.open(QIODevice::ReadOnly), QStringLiteral("Cannot inspect SVG rules: ") + path);
  QXmlStreamReader xml(file.readAll());
  bool found = false;
  while (!xml.atEnd()) {
    xml.readNext();
    if (xml.isStartElement() && xml.name() == QStringLiteral("rect")) {
      const double width = xml.attributes().value("width").toDouble();
      const double height = xml.attributes().value("height").toDouble();
      if (std::abs(width - rulePt.width() * 72.0 / 72.27) < 0.01 &&
          std::abs(height - rulePt.height() * 72.0 / 72.27) < 0.01) found = true;
    }
  }
  require(!xml.hasError() && found,
          QStringLiteral("Selected SVG is missing this request's %1 x %2 pt rule")
              .arg(rulePt.width()).arg(rulePt.height()));
}

QJsonObject inspectMetrics(const stemtex::LuaRenderResult &result, bool svg,
                           double width, double fontSize) {
  QJsonParseError error;
  const auto document = QJsonDocument::fromJson(result.summary.toUtf8(), &error);
  require(error.error == QJsonParseError::NoError && document.isObject(),
          QStringLiteral("Summary is not a JSON object: ") + result.summary);
  const auto metrics = document.object();
  const QString format = svg ? "svg" : "pdf";
  require(metrics.value("outputFormat").toString() == format && result.outputFormat == format,
          QStringLiteral("Summary has the wrong outputFormat"));
  require(metrics.value("backend").toString() == (svg ? "dvisvgmdaemon" : "xdvipdfmxdaemon"),
          QStringLiteral("Summary did not name the native converter backend"));
  require(metrics.value("converterMode").toString() == "daemon-dll" &&
              metrics.value(svg ? "dvisvgmMode" : "xdvipdfmxMode").toString() == "daemon-dll",
          QStringLiteral("The request did not use a resident converter DLL"));
  const QStringList timingKeys{"requestToOutputMs", "typesetMs", "finalizeDviMs", "convertMs",
                               "xdvipdfmxMs", "dvisvgmMs"};
  for (const auto &key : timingKeys) {
    const auto value = metrics.value(key);
    require(value.isDouble() && std::isfinite(value.toDouble()) && value.toDouble() >= 0,
            QStringLiteral("Missing or invalid timing metric: ") + key);
  }
  require(metrics.value("requestToOutputMs").toDouble() >= metrics.value("convertMs").toDouble() &&
              metrics.value("requestToOutputMs").toDouble() >= metrics.value("typesetMs").toDouble(),
          QStringLiteral("Stage timing exceeds the complete request timing"));
  require(std::abs(metrics.value("convertMs").toDouble() -
                   metrics.value(svg ? "dvisvgmMs" : "xdvipdfmxMs").toDouble()) < 0.01,
          QStringLiteral("Selected converter timing differs from convertMs"));
  require(metrics.value(svg ? "xdvipdfmxMs" : "dvisvgmMs").toDouble() == 0,
          QStringLiteral("Unused converter reports conversion time"));
  require(metrics.value("outputBytes").toDouble() == QFileInfo(result.outputPath).size() &&
              metrics.value("outputPath").toString() == result.outputPath,
          QStringLiteral("Summary does not describe the produced file"));
  require(std::abs(metrics.value("widthPt").toDouble() - width) < 0.0001 &&
              std::abs(metrics.value("fontSizePt").toDouble() - fontSize) < 0.0001,
          QStringLiteral("Summary dimensions differ from the request"));
  require(metrics.value("workerPid").toDouble() == result.workerPid &&
              metrics.value("workerPage").toInt() == result.pageNumber,
          QStringLiteral("Summary worker identity differs from the render result"));
  require(metrics.value("checkpoint").toBool(),
          QStringLiteral("The initialized worker did not report its checkpoint"));
  require(metrics.value("issueFlags").isDouble() && metrics.value("issueFlags").toInt() == result.issueFlags &&
              metrics.value("outcomeMessage").toString() == result.outcomeMessage &&
              metrics.value(svg ? "dvisvgmReturnCode" : "xdvipdfmxReturnCode").isDouble(),
          QStringLiteral("Summary did not preserve native converter outcomes"));
  return metrics;
}

struct EnvironmentSnapshot {
  QProcessEnvironment windows = QProcessEnvironment::systemEnvironment();
  QStringList names{"TEXMFROOT", "TEXMFCNF", "TEXMFVAR", "TEXMFCACHE", "TEXMFCONFIG", "TEXFORMATS",
                    "TEXINPUTS", "TEXFONTMAPS", "OPENTYPEFONTS", "TTFONTS", "FONTCONFIG_PATH",
                    "FONTCONFIG_FILE", "PATH", "max_print_line"};
  QMap<QString, QByteArray> crt;
  EnvironmentSnapshot() {
    for (const auto &name : names) crt.insert(name, qgetenv(name.toLatin1().constData()));
  }
  void verifyRestored(const QString &phase) const {
    const auto current = QProcessEnvironment::systemEnvironment();
    for (const auto &name : names) {
      require(current.contains(name) == windows.contains(name) && current.value(name) == windows.value(name) &&
                  qgetenv(name.toLatin1().constData()) == crt.value(name),
              QStringLiteral("%1 leaked process environment variable %2").arg(phase, name));
    }
  }
};

QJsonObject renderXeTransition(const stemtex::LuaBackendConfig &config, const QString &marker) {
  const QByteArray repo = config.repoRoot.toUtf8(), runtime = config.runtimeRoot.toUtf8();
  const QByteArray texmf = config.texmfRoot.toUtf8(), profile = config.profileRoot.toUtf8();
  const QByteArray state = QDir(config.stateRoot).filePath("xe-state").toUtf8();
  const QByteArray renders = QDir(config.stateRoot).filePath("xe-renders").toUtf8();
  StemTeXConfig native{};
  native.repo_root_utf8 = repo.constData();
  native.runtime_root_utf8 = runtime.constData();
  native.texmf_root_utf8 = texmf.constData();
  native.profile_root_utf8 = profile.constData();
  native.state_root_utf8 = state.constData();
  native.renders_root_utf8 = renders.constData();
  native.request_timeout_ms = 90000;
  native.xdvipdfmx_timeout_ms = 90000;
  native.spare_worker_count = 0;
  StemTeXErrorCode code = STEMTEX_OK;
  char *error = nullptr;
  std::unique_ptr<StemTeXRenderer, decltype(&stemtex_renderer_destroy)> renderer(
      stemtex_renderer_create(&native, &code, &error), &stemtex_renderer_destroy);
  QString message = error ? QString::fromUtf8(error) : QString();
  stemtex_renderer_free_string(error);
  error = nullptr;
  require(renderer != nullptr, QStringLiteral("XeTeX transition initialization failed: ") + message);
  struct OutputGuard {
    StemTeXRenderOutputResult value{};
    ~OutputGuard() { stemtex_renderer_free_output_result(&value); }
  } output;
  const QByteArray snippet = (marker + ".\\par\\[E=mc^2\\]\n").toUtf8();
  const bool ok = stemtex_renderer_render_block(renderer.get(), snippet.constData(), 280.0, 10.0,
                                                STEMTEX_OUTPUT_PDF, &output.value, &code, &error) != 0;
  message = error ? QString::fromUtf8(error) : QString();
  stemtex_renderer_free_string(error);
  require(ok, QStringLiteral("XeTeX transition render failed: ") + message);
  const QString path = QString::fromUtf8(output.value.output_path_utf8);
  const QString summary = QString::fromUtf8(output.value.summary_json_utf8);
  const auto metrics = QJsonDocument::fromJson(summary.toUtf8()).object();
  require(metrics.value("converterMode").toString() == "daemon-dll",
          QStringLiteral("XeTeX transition did not use the resident converter"));
  return {{"marker", marker}, {"output", path}, {"inspection", inspectPdf(path, marker)},
          {"summary", metrics}};
}

struct ProcessTracker {
  std::vector<HANDLE> handles;
  ~ProcessTracker() {
    for (HANDLE handle : handles) CloseHandle(handle);
  }
  void track(qint64 pid) {
    HANDLE handle = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
    require(handle != nullptr, QStringLiteral("Cannot inspect live worker PID %1").arg(pid));
    handles.push_back(handle);
  }
  void verifyStopped() const {
    for (HANDLE handle : handles) {
      require(WaitForSingleObject(handle, 3000) == WAIT_OBJECT_0,
              QStringLiteral("A LuaHBTeX worker remained alive after backend destruction"));
    }
  }
};

}  // namespace

int main(int argc, char **argv) {
  QGuiApplication app(argc, argv);
  const QStringList args = app.arguments();
  QTextStream out(stdout);
  if (args.size() != 6 && args.size() != 7) {
    out << "Usage: stemtex-luatex-smoke REPO_ROOT TEXLIVE_ROOT ENGINE_EXE PROFILE_ROOT STATE_ROOT [RUNTIME_ROOT]\n";
    return 2;
  }

  stemtex::LuaBackendConfig config;
  config.repoRoot = QDir(args.at(1)).absolutePath();
  config.runtimeRoot = args.size() == 7 ? QDir(args.at(6)).absolutePath()
                                      : QDir(config.repoRoot).filePath("staging/runtime");
  config.texmfRoot = QDir(args.at(2)).absolutePath();
  config.enginePath = QDir::cleanPath(args.at(3));
  config.profileRoot = QDir(args.at(4)).absolutePath();
  config.stateRoot = QDir(args.at(5)).absolutePath();
  config.timeoutMs = 90000;
  QDir().mkpath(config.stateRoot);

  QJsonObject report{{"passed", false}, {"engine", config.enginePath}, {"texmfRoot", config.texmfRoot},
                     {"runtimeRoot", config.runtimeRoot}};
  QJsonArray requests;
  QElapsedTimer total;
  total.start();
  int exitCode = 1;
  try {
    const EnvironmentSnapshot originalEnvironment;
    report.insert("xeBeforeLua", renderXeTransition(config, QStringLiteral("XEBEFORELUA")));
    originalEnvironment.verifyRestored("XeTeX before LuaHBTeX");
    out << "xe-before-lua: resident PDF converter passed\n";
    out.flush();
    ProcessTracker workers;
    auto backend = std::make_unique<stemtex::LuaBackend>(config);
    QString error;
    const bool initialized = backend->initialize([&out](const QString &message) {
              out << "startup: " << message << '\n';
              out.flush();
            }, &error);
    require(initialized, QStringLiteral("Initialization failed: ") + error);
    require(backend->ready(), QStringLiteral("Backend did not become ready"));
    const qint64 initialPid = backend->workerPid();
    require(initialPid > 0, QStringLiteral("Initialized backend has no worker PID"));
    workers.track(initialPid);
    HMODULE pdfModule = GetModuleHandleW(L"dvipdfmxdaemon.dll");
    HMODULE svgModule = GetModuleHandleW(L"dvisvgmdaemon.dll");
    auto requireResidentConverters = [&]() {
      require(pdfModule && svgModule && GetModuleHandleW(L"dvipdfmxdaemon.dll") == pdfModule &&
                  GetModuleHandleW(L"dvisvgmdaemon.dll") == svgModule,
              QStringLiteral("Resident converter DLLs were unloaded during a live backend lifetime"));
    };
    requireResidentConverters();
    int previousPage = 1;  // Internal warmup is the checkpoint baseline page.

    auto render = [&](const char *label, const QByteArray &snippet, double width, double fontSize,
                      bool svg, const QString &marker = {}, const QString &previousMarker = {},
                      const QSizeF &rulePt = {}) {
      QElapsedTimer timer;
      timer.start();
      const auto result = backend->render(snippet, width, fontSize, svg);
      const qint64 elapsed = timer.elapsed();
      require(result.ok, QStringLiteral("%1 failed: %2").arg(QString::fromLatin1(label), result.error));
      require(result.workerPid > 0 && result.workerPid == backend->workerPid(),
              QStringLiteral("Render result did not identify the live worker"));
      require(backend->ready(), QStringLiteral("Worker was not ready after rendering"));
      requireResidentConverters();
      const auto metrics = inspectMetrics(result, svg, width, fontSize);
      const auto inspection = svg ? inspectSvg(result.outputPath)
                                  : inspectPdf(result.outputPath, marker, previousMarker);
      if (svg && rulePt.isValid()) requireSvgRule(result.outputPath, rulePt);
      // This smoke uses the bundled profile's 1 pt PreviewBorder. Both
      // converters must retain the requested minipage width and that border.
      const double expectedWidth = (width + 2.0) * 72.0 / 72.27;
      const double actualWidth = inspection.value(svg ? "viewBoxWidth" : "widthPt").toDouble();
      require(std::abs(actualWidth - expectedWidth) < 0.3,
              QStringLiteral("%1 page width is %2 bp; expected %3 bp for the requested width and border")
                  .arg(QString::fromLatin1(label)).arg(actualWidth).arg(expectedWidth));
      requests.append(QJsonObject{{"name", QString::fromLatin1(label)}, {"elapsedMs", elapsed},
                                  {"pid", result.workerPid}, {"page", result.pageNumber},
                                  {"output", result.outputPath}, {"inspection", inspection},
                                  {"summary", metrics}, {"recoverable", result.recoverable},
                                  {"issueFlags", result.issueFlags}, {"outcomeMessage", result.outcomeMessage}});
      out << label << ": PID=" << result.workerPid << " page=" << result.pageNumber
          << " elapsed=" << elapsed << " ms convert=" << metrics.value("convertMs").toDouble()
          << " ms mode=" << metrics.value("converterMode").toString()
          << " return=" << metrics.value(svg ? "dvisvgmReturnCode" : "xdvipdfmxReturnCode").toInt()
          << " issueFlags=" << result.issueFlags << " output=" << result.outputPath << '\n';
      out.flush();
      return result;
    };

    const auto first = render("first-pdf", "PDFALPHA. Inline math $E=mc^2$.\\par\n"
                              "\\[\\int_0^1 x^2\\,dx=\\frac{1}{3}\\]\n",
                              220.25, 10.0, false, QStringLiteral("PDFALPHA"));
    require(first.workerPid == initialPid && first.pageNumber == previousPage + 1,
            QStringLiteral("First request did not follow the initialized worker's warmup page"));
    const QByteArray firstPdfBytes = readOutput(first.outputPath);
    previousPage = first.pageNumber;

    const auto second = render("second-svg", "SVGSECOND.\\par\\rule{37pt}{9pt}\\par\n"
                               "\\[\\sum_{k=1}^{n} k=\\frac{n(n+1)}{2}\\]\n",
                               360.0, 12.0, true, {}, {}, QSizeF(37.0, 9.0));
    require(second.workerPid == initialPid && second.pageNumber == previousPage + 1,
            QStringLiteral("Second request did not advance one page in the same live worker"));
    previousPage = second.pageNumber;

    const auto third = render("third-pdf", "PDFOMEGA. Different request, width, and font size.\\par\n"
                              "\\[\\sqrt{a^2+b^2}\\ne a+b\\]\n",
                              300.5, 14.0, false, QStringLiteral("PDFOMEGA"), QStringLiteral("PDFALPHA"));
    require(third.workerPid == initialPid && third.pageNumber == previousPage + 1,
            QStringLiteral("Third request did not advance one page in the same live worker"));
    previousPage = third.pageNumber;

    const auto hotSvg = render("hot-latest-svg", "SVGHOTLATEST.\\par\\rule{49pt}{6pt}\\par\n"
                               "\\[\\alpha^2+\\beta^2=\\gamma^2\\]\n",
                               240.0, 11.5, true, {}, {}, QSizeF(49.0, 6.0));
    require(hotSvg.workerPid == initialPid && hotSvg.pageNumber == previousPage + 1,
            QStringLiteral("Repeated SVG conversion did not advance in the same worker"));
    previousPage = hotSvg.pageNumber;

    // Many overlapping boxes keep the visual page small while forcing the
    // native snapshot parser past the engine's 16 KB DVI buffer boundary.
    QByteArray largeSnippet("PDFBOUNDARY.\\par\\leavevmode\n");
    for (int i = 0; i < 400; ++i)
      largeSnippet += "\\rlap{abcdefghijklmnopqrstuvwxyz0123456789}\\relax\n";
    largeSnippet += "\\strut\\par\n";
    const auto boundary = render("buffer-boundary-pdf", largeSnippet, 300.0, 9.0, false,
                                 QStringLiteral("PDFBOUNDARY"), QStringLiteral("PDFOMEGA"));
    require(boundary.workerPid == initialPid && boundary.pageNumber == previousPage + 1,
            QStringLiteral("Large request did not advance one page in the same live worker"));
    QFileInfo boundaryDvi(boundary.outputPath.left(boundary.outputPath.size() - 4) + ".dvi");
    require(boundaryDvi.size() > 16384, QStringLiteral("Boundary case did not exceed the DVI buffer size"));
    report.insert("boundaryDviBytes", boundaryDvi.size());
    previousPage = boundary.pageNumber;

    for (bool svg : {false, true}) {
      QElapsedTimer blankTimer;
      blankTimer.start();
      const auto blank = backend->render({}, 300.0, 10.0, svg);
      require(blank.ok, QStringLiteral("Empty editor input failed: ") + blank.error);
      require(blank.workerPid == initialPid && blank.pageNumber == previousPage + 1 && backend->ready(),
              QStringLiteral("Empty input disrupted the live worker"));
      requireResidentConverters();
      const auto metrics = inspectMetrics(blank, svg, 300.0, 10.0);
      QJsonObject inspection;
      if (svg) {
        inspection = inspectSvg(blank.outputPath, true);
      } else {
        QPdfDocument pdf;
        require(pdf.load(blank.outputPath) == QPdfDocument::Error::None && pdf.pageCount() == 1,
                QStringLiteral("Empty input did not produce one valid PDF page"));
        const auto raster = pdf.render(0, rasterSize(pdf.pagePointSize(0)));
        require(!raster.isNull(), QStringLiteral("Empty PDF page could not be rasterized"));
        inspection = {{"pages", pdf.pageCount()}, {"visibleInk", hasInk(raster)},
                      {"widthPt", pdf.pagePointSize(0).width()}};
      }
      const double actualWidth = inspection.value(svg ? "viewBoxWidth" : "widthPt").toDouble();
      require(std::abs(actualWidth - 302.0 * 72.0 / 72.27) < 0.3,
              QStringLiteral("Empty input did not preserve the requested page width and border"));
      previousPage = blank.pageNumber;
      requests.append(QJsonObject{{"name", svg ? "empty-svg" : "empty-pdf"},
                                  {"elapsedMs", blankTimer.elapsed()}, {"pid", blank.workerPid},
                                  {"page", blank.pageNumber}, {"output", blank.outputPath},
                                  {"inspection", inspection}, {"summary", metrics},
                                  {"recoverable", blank.recoverable}, {"issueFlags", blank.issueFlags},
                                  {"outcomeMessage", blank.outcomeMessage}});
      out << (svg ? "empty-svg" : "empty-pdf") << ": PID=" << blank.workerPid
          << " page=" << blank.pageNumber << " elapsed=" << blankTimer.elapsed() << " ms\n";
      out.flush();
    }
    report.insert("sameWorkerAcrossSuccesses", true);

    QElapsedTimer failureTimer;
    failureTimer.start();
    const auto invalid = backend->render("\\StemTeXDeliberatelyUndefinedCommand\n", 300.0, 10.0, false);
    require(!invalid.ok && !invalid.error.isEmpty(), QStringLiteral("Invalid TeX was not reported as an error"));
    QJsonParseError failureParseError;
    const auto failureDocument = QJsonDocument::fromJson(invalid.summary.toUtf8(), &failureParseError);
    require(failureParseError.error == QJsonParseError::NoError && failureDocument.isObject(),
            QStringLiteral("Invalid TeX did not return structured recovery metrics"));
    const auto failureMetrics = failureDocument.object();
    require(failureMetrics.value("checkpoint").toBool() && failureMetrics.value("checkpointRestored").toBool() &&
                !failureMetrics.value("workerRestarted").toBool() &&
                failureMetrics.value("recoveryMode").toString() == "checkpoint" &&
                failureMetrics.value("readyAfterError").toBool() && backend->ready() &&
                failureMetrics.value("failedWorkerPid").toDouble() == initialPid &&
                failureMetrics.value("workerPid").toDouble() == initialPid && backend->workerPid() == initialPid,
            QStringLiteral("Invalid TeX did not restore the checkpoint in the same ready worker"));
    requireResidentConverters();
    report.insert("texError", QJsonObject{{"elapsedMs", failureTimer.elapsed()}, {"message", invalid.error},
                                         {"summary", failureMetrics}});
    out << "invalid-tex: checkpoint restored in the same PID\n";
    out.flush();

    const auto invalidSize = backend->render("not sent to TeX", -1.0, 10.0, false);
    const auto validationMetrics = QJsonDocument::fromJson(invalidSize.summary.toUtf8()).object();
    require(!invalidSize.ok && backend->ready() && backend->workerPid() == initialPid &&
                validationMetrics.value("failureStage").toString() == "validation" &&
                validationMetrics.value("recoveryMode").toString() == "none" &&
                !validationMetrics.value("checkpointRestored").toBool() &&
                !validationMetrics.value("workerRestarted").toBool() &&
                validationMetrics.value("failedWorkerPid").toInteger() == 0 &&
                validationMetrics.value("restoreMs").toInteger(-1) == 0,
            QStringLiteral("Invalid dimensions reused old recovery metrics or disrupted the worker"));
    report.insert("argumentValidationPreservesWorker", true);

    const auto recovered = render("recovered-pdf", "PDFRECOVERED.\\par\n"
                                  "\\[\\frac{x+1}{x-1}\\]\n",
                                  280.0, 11.0, false, QStringLiteral("PDFRECOVERED"), QStringLiteral("PDFOMEGA"));
    require(recovered.workerPid == initialPid && recovered.pageNumber == 2,
            QStringLiteral("TeX error recovery did not resume after the baseline in the same worker"));
    require(recovered.outputPath != first.outputPath && readOutput(first.outputPath) == firstPdfBytes,
            QStringLiteral("Checkpoint recovery overwrote a previously published PDF"));
    report.insert("recoveredWithSameWorker", true);
    report.insert("publishedPdfPreserved", true);
    const auto recoveredSvg = render("recovered-svg", "SVGRECOVERED.\\par\\rule{23pt}{14pt}\\par\n"
                                      "\\[\\frac{1}{1+x^2}\\]\n",
                                      260.0, 13.0, true, {}, {}, QSizeF(23.0, 14.0));
    require(recoveredSvg.workerPid == recovered.workerPid && recoveredSvg.pageNumber == recovered.pageNumber + 1,
            QStringLiteral("Recovered SVG did not advance in the restored TeX worker"));
    report.insert("residentConvertersSurvivedCheckpointRestore", true);

    QElapsedTimer cancelTimer;
    cancelTimer.start();
    std::thread canceller([&backend]() {
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
      backend->cancel();
    });
    stemtex::LuaRenderResult cancelled;
    try {
      cancelled = backend->render("\\loop\\iftrue\\repeat\n", 300.0, 10.0, false);
    } catch (...) {
      canceller.join();
      throw;
    }
    canceller.join();
    require(!cancelled.ok, QStringLiteral("Cancelled infinite TeX loop unexpectedly succeeded"));
    require(!backend->ready() && backend->workerPid() == 0,
            QStringLiteral("Terminal cancellation retained a ready or running worker"));
    require(cancelTimer.elapsed() < 5000,
            QStringLiteral("Cancellation took %1 ms, expected less than 5000 ms").arg(cancelTimer.elapsed()));
    report.insert("cancel", QJsonObject{{"elapsedMs", cancelTimer.elapsed()}, {"message", cancelled.error}});
    out << "cancel: returned in " << cancelTimer.elapsed() << " ms\n";
    backend.reset();
    workers.verifyStopped();
    originalEnvironment.verifyRestored("LuaHBTeX destruction");
    require(GetModuleHandleW(L"dvipdfmxdaemon.dll") == nullptr &&
                GetModuleHandleW(L"dvisvgmdaemon.dll") == nullptr,
            QStringLiteral("LuaHBTeX destruction did not unload its converter DLLs"));

    backend = std::make_unique<stemtex::LuaBackend>(config);
    error.clear();
    const bool reinitialized = backend->initialize([&out](const QString &message) {
      out << "reinitialize: " << message << '\n';
      out.flush();
    }, &error);
    require(reinitialized, QStringLiteral("Second backend initialization failed: ") + error);
    workers.track(backend->workerPid());
    pdfModule = GetModuleHandleW(L"dvipdfmxdaemon.dll");
    svgModule = GetModuleHandleW(L"dvisvgmdaemon.dll");
    requireResidentConverters();
    const auto reinitializedPdf = render("reinitialized-pdf", "PDFREINITIALIZED.\\par\\[a+b=c\\]\n",
                                         310.0, 10.5, false, QStringLiteral("PDFREINITIALIZED"),
                                         QStringLiteral("PDFRECOVERED"));
    require(reinitializedPdf.pageNumber == 2,
            QStringLiteral("Reinitialized backend did not render after its own warmup baseline"));
    const auto reinitializedSvg = render("reinitialized-svg", "SVGREINITIALIZED.\\par\\rule{41pt}{7pt}\\par\n"
                                         "\\[\\sqrt{x+y}\\]\n",
                                         290.0, 15.0, true, {}, {}, QSizeF(41.0, 7.0));
    require(reinitializedSvg.workerPid == reinitializedPdf.workerPid &&
                reinitializedSvg.pageNumber == reinitializedPdf.pageNumber + 1,
            QStringLiteral("Reinitialized converters did not handle repeated requests"));
    backend.reset();
    workers.verifyStopped();
    originalEnvironment.verifyRestored("LuaHBTeX reinitialization and destruction");
    report.insert("converterShutdownAndReinitialize", true);

    report.insert("xeAfterLua", renderXeTransition(config, QStringLiteral("XEAFTERLUA")));
    originalEnvironment.verifyRestored("XeTeX after LuaHBTeX");
    out << "xe-after-lua: resident PDF converter passed\n";
    report.insert("environmentRestored", true);
    report.insert("xeLuaXeTransitions", true);
    report.insert("workersStopped", true);
    report.insert("passed", true);
    exitCode = 0;
  } catch (const std::exception &error) {
    report.insert("error", QString::fromUtf8(error.what()));
    out << "FAILED: " << error.what() << '\n';
  }
  report.insert("requests", requests);
  report.insert("elapsedMs", total.elapsed());
  const QString reportPath = QDir(config.stateRoot).filePath("smoke-results.json");
  QFile reportFile(reportPath);
  if (!reportFile.open(QIODevice::WriteOnly) || reportFile.write(QJsonDocument(report).toJson()) < 0) {
    out << "FAILED: could not write " << reportPath << '\n';
    return 1;
  }
  out << (exitCode == 0 ? "PASS: " : "FAIL: ") << reportPath << '\n';
  return exitCode;
}
