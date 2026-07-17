#include "stemtex_renderer.h"

#include <QApplication>
#include <QByteArray>
#include <QClipboard>
#include <QColor>
#include <QDesktopServices>
#include <QDir>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QHBoxLayout>
#include <QIcon>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMainWindow>
#include <QMetaObject>
#include <QPdfDocument>
#include <QImage>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QComboBox>
#include <QSignalBlocker>
#include <QSlider>
#include <QSpinBox>
#include <QSplitter>
#include <QStatusBar>
#include <QSvgRenderer>
#include <QString>
#include <QStringList>
#include <QTextBrowser>
#include <QTextStream>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QVector>
#include <QWidget>

#include <Qsci/qscilexertex.h>
#include <Qsci/qsciscintilla.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>

namespace {

constexpr double kWidthSliderScale = 10.0;
constexpr double kDefaultFontSizePt = 10.0;

std::mutex gRendererLifecycleMutex;

int widthToSliderValue(double widthPt) {
  return qRound(widthPt * kWidthSliderScale);
}

double sliderValueToWidth(int value) {
  return value / kWidthSliderScale;
}

QString appResourceRoot() {
  return QDir::cleanPath(QCoreApplication::applicationDirPath());
}

QString defaultRuntimeRoot() {
  QDir appDir(QCoreApplication::applicationDirPath());
  QString installedRuntime = appDir.filePath("../runtime");
  return QDir::cleanPath(QDir(installedRuntime).absolutePath());
}

QString normalizeRuntimeRoot(const QString &path) {
  QDir dir(QDir::cleanPath(QDir(path).absolutePath()));
  return QDir::cleanPath(dir.absolutePath());
}

QString normalizeTexmfRoot(const QString &path) {
  return QDir::cleanPath(QDir(path).absolutePath());
}

QString defaultTexmfRoot(const QString &runtimeRoot) {
  return normalizeTexmfRoot(runtimeRoot);
}

struct ProfileEntry {
  QString name;
  QString path;
};

bool rendererProfileInfo(const QString &profileRoot, ProfileEntry *entry, QString *errorText) {
  QByteArray profile = QDir::cleanPath(profileRoot).toUtf8();
  StemTeXErrorCode code = STEMTEX_OK;
  char *error = nullptr;
  char *json = stemtex_renderer_profile_info_json(profile.constData(), &code, &error);
  if (!json) {
    if (errorText) *errorText = error ? QString::fromUtf8(error) : QString("profile scan failed");
    stemtex_renderer_free_string(error);
    return false;
  }
  QJsonParseError parseError{};
  QJsonDocument doc = QJsonDocument::fromJson(QByteArray(json), &parseError);
  stemtex_renderer_free_string(json);
  stemtex_renderer_free_string(error);
  if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
    if (errorText) *errorText = QString("profile parser returned invalid JSON");
    return false;
  }
  QJsonObject obj = doc.object();
  if (!obj.value("valid").toBool()) return false;
  QString path = obj.value("path").toString();
  if (path.isEmpty()) return false;
  QString name = obj.value("name").toString();
  if (name.isEmpty()) name = QFileInfo(path).fileName();
  if (entry) *entry = {name, QDir::cleanPath(path)};
  return true;
}

QVector<ProfileEntry> profileRoots(const QString &, QString *errorText) {
  QVector<ProfileEntry> profiles;
  QDir appDir(QCoreApplication::applicationDirPath());
  QDir dir(appDir.filePath("profiles"));
  if (!dir.exists()) {
    if (errorText) *errorText = QString("profile directory not found: %1").arg(dir.absolutePath());
    return profiles;
  }
  for (const QFileInfo &candidate : dir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
    ProfileEntry profile;
    if (!rendererProfileInfo(candidate.absoluteFilePath(), &profile, errorText)) continue;
    profiles.push_back(profile);
  }
  return profiles;
}

QString defaultSnippet() {
  return QString::fromUtf8(
      "这是一段 StemTeX Renderer GUI 里的中文、数学和化学预览：$E=mc^2$，以及 \\textcolor{blue}{蓝色文字}。\n\n"
      "\\[\n"
      "  \\int_0^1 x^2\\,dx = \\frac{1}{3},\\quad \\langle\\psi,\\phi\\rangle\n"
      "\\]\n\n"
      "\\ce{2H2 + O2 -> 2H2O}\n");
}

void configureRendererDllSearch(const QString &runtimeRoot) {
  QString appDir = QCoreApplication::applicationDirPath();
  QString sdkDir = QDir(runtimeRoot).filePath("bin/sdk");
  std::wstring appDirWide = QDir::toNativeSeparators(QDir::cleanPath(appDir)).toStdWString();
  std::wstring sdkDirWide = QDir::toNativeSeparators(QDir::cleanPath(sdkDir)).toStdWString();
  SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS | LOAD_LIBRARY_SEARCH_USER_DIRS);
  AddDllDirectory(appDirWide.c_str());
  AddDllDirectory(sdkDirWide.c_str());
}

QString oneLineJsonMetric(const QString &summaryJson) {
  QJsonParseError err{};
  QJsonDocument doc = QJsonDocument::fromJson(summaryJson.toUtf8(), &err);
  if (err.error != QJsonParseError::NoError || !doc.isObject()) return summaryJson;
  QJsonObject obj = doc.object();
  QString format = obj.value("outputFormat").toString("pdf").toUpper();
  QString backend = obj.value("backend").toString(format == "SVG" ? "dvisvgmdaemon" : "xdvipdfmxdaemon");
  int totalMs = obj.value("requestToOutputMs").toInt(obj.value("requestToPdfMs").toInt());
  int convertMs = format == "SVG" ? obj.value("dvisvgmMs").toInt(obj.value("convertMs").toInt())
                                  : obj.value("xdvipdfmxMs").toInt(obj.value("convertMs").toInt());
  int finalizeXdvMs = obj.value("finalizeXdvMs").toInt();
  int outputBytes = obj.value("outputBytes").toInt(format == "SVG" ? obj.value("svgBytes").toInt()
                                                                    : obj.value("pdfBytes").toInt());
  int xetexNoPdfMs = qMax(0, totalMs - finalizeXdvMs - convertMs);
  double widthPt = obj.value("widthPt").toDouble();
  double fontSizePt = obj.value("fontSizePt").toDouble(kDefaultFontSizePt);
  return QString("Output: %1 via %2\n"
                 "XeTeX --no-pdf time: %3 ms\n"
                 "conversion time: %4 ms\n"
                 "Total time until %1 complete: %5 ms\n"
                 "Layout: %6 pt width, %7 pt font\n"
                 "%1: %8 bytes, spare: %9/%10")
      .arg(format)
      .arg(backend)
      .arg(xetexNoPdfMs)
      .arg(convertMs)
      .arg(totalMs)
      .arg(widthPt, 0, 'f', 1)
      .arg(fontSizePt, 0, 'f', 1)
      .arg(outputBytes)
      .arg(obj.value("spareReady").toInt())
      .arg(obj.value("spareTarget").toInt());
}

QByteArray encodeWithCodePage(const QString &text, UINT codePage) {
  std::wstring wide = text.toStdWString();
  if (wide.empty()) return QByteArray();
  int size = WideCharToMultiByte(codePage, 0, wide.data(), (int)wide.size(), nullptr, 0, nullptr, nullptr);
  if (size <= 0) return text.toUtf8();
  std::vector<char> bytes((size_t)size);
  WideCharToMultiByte(codePage, 0, wide.data(), (int)wide.size(), bytes.data(), size, nullptr, nullptr);
  return QByteArray(bytes.data(), size);
}

QByteArray encodeSnippetForTeX(const QString &text, const QString &encoding) {
  if (encoding == "GBK") {
    return QByteArray("\\XeTeXinputencoding \"GBK\"\n") + encodeWithCodePage(text, 936);
  }
  if (encoding == "Big5") {
    return QByteArray("\\XeTeXinputencoding \"Big5\"\n") + encodeWithCodePage(text, 950);
  }
  return text.toUtf8();
}

QRect expandRectRightToWidth(QRect rect, int minWidth, const QRect &limit) {
  if (rect.width() >= minWidth) return rect.intersected(limit);
  rect.setRight(rect.left() + minWidth - 1);
  return rect.intersected(limit);
}

struct CroppedPreview {
  QImage image;
  QSize displaySize;
};

QRect contentBounds(const QImage &rgba) {
  QRect bounds;
  const int threshold = 245;
  for (int y = 0; y < rgba.height(); ++y) {
    const QRgb *line = reinterpret_cast<const QRgb *>(rgba.constScanLine(y));
    for (int x = 0; x < rgba.width(); ++x) {
      QRgb pixel = line[x];
      if (qAlpha(pixel) > 8 && (qRed(pixel) < threshold || qGreen(pixel) < threshold || qBlue(pixel) < threshold)) {
        QRect pixel(x, y, 1, 1);
        bounds = bounds.isNull() ? pixel : bounds.united(pixel);
      }
    }
  }
  return bounds;
}

QSize displaySizeForRenderedPixels(const QSize &pixels, double pixelsPerPoint) {
  constexpr double screenPixelsPerPoint = 96.0 / 72.0;
  return QSize(qMax(1, static_cast<int>(std::ceil(pixels.width() / pixelsPerPoint * screenPixelsPerPoint))),
               qMax(1, static_cast<int>(std::ceil(pixels.height() / pixelsPerPoint * screenPixelsPerPoint))));
}

CroppedPreview cropRenderedPreview(const QImage &source, double minWidthPt, int dpi, double paddingPt) {
  if (source.isNull()) return {};
  double pixelsPerPoint = qMax(1.0, static_cast<double>(dpi) / 72.0);
  QImage rgba = source.convertToFormat(QImage::Format_ARGB32);
  QRect bounds = contentBounds(rgba);
  if (bounds.isNull()) return {rgba.convertToFormat(QImage::Format_RGB32), displaySizeForRenderedPixels(rgba.size(), pixelsPerPoint)};

  int pad = qMax(0, qRound(paddingPt * pixelsPerPoint));
  bounds = expandRectRightToWidth(bounds, qMax(1, static_cast<int>(std::ceil(minWidthPt * pixelsPerPoint))), rgba.rect());
  QImage cropped = rgba.copy(bounds);
  QImage white(cropped.width() + pad * 2, cropped.height() + pad * 2, QImage::Format_RGB32);
  white.fill(Qt::white);
  QPainter painter(&white);
  painter.drawImage(pad, pad, cropped);
  painter.end();
  return {white, displaySizeForRenderedPixels(white.size(), pixelsPerPoint)};
}

CroppedPreview renderCroppedPdfPreview(const QString &pdfPath, double minWidthPt, int dpi, double paddingPt) {
  QPdfDocument source;
  QPdfDocument::Error error = source.load(pdfPath);
  if (error != QPdfDocument::Error::None || source.pageCount() <= 0) return {};

  double pixelsPerPoint = qMax(1.0, static_cast<double>(dpi) / 72.0);
  QSizeF points = source.pagePointSize(0);
  QSize imageSize(qMax(1, static_cast<int>(std::ceil(points.width() * pixelsPerPoint))),
                  qMax(1, static_cast<int>(std::ceil(points.height() * pixelsPerPoint))));
  QImage page = source.render(0, imageSize);
  return cropRenderedPreview(page, minWidthPt, dpi, paddingPt);
}

double svgLengthPt(const QString &svg, const QString &attribute) {
  QString single = attribute + "='";
  QString dbl = attribute + "=\"";
  int start = svg.indexOf(single);
  int quoteLen = single.size();
  if (start < 0) {
    start = svg.indexOf(dbl);
    quoteLen = dbl.size();
  }
  if (start < 0) return 0.0;
  start += quoteLen;
  int end = svg.indexOf(svg.at(start - 1), start);
  if (end <= start) return 0.0;
  QString value = svg.mid(start, end - start).trimmed();
  if (value.endsWith("pt", Qt::CaseInsensitive)) value.chop(2);
  bool ok = false;
  double number = value.toDouble(&ok);
  return ok ? number : 0.0;
}

QSizeF svgPointSize(const QString &svgPath) {
  QFile file(svgPath);
  if (!file.open(QIODevice::ReadOnly)) return {};
  QString head = QString::fromUtf8(file.read(4096));
  double widthPt = svgLengthPt(head, "width");
  double heightPt = svgLengthPt(head, "height");
  return widthPt > 0.0 && heightPt > 0.0 ? QSizeF(widthPt, heightPt) : QSizeF();
}

CroppedPreview renderSvgPreview(const QString &svgPath, double minWidthPt, int dpi, double paddingPt) {
  QSvgRenderer renderer(svgPath);
  if (!renderer.isValid()) return {};

  QSizeF points = svgPointSize(svgPath);
  if (!points.isValid() || points.isEmpty()) {
    QSizeF viewBox = renderer.viewBoxF().size();
    if (viewBox.isValid() && !viewBox.isEmpty()) {
      points = viewBox;
    } else {
      QSize defaultSize = renderer.defaultSize();
      points = QSizeF(defaultSize.width() * 72.0 / 96.0, defaultSize.height() * 72.0 / 96.0);
    }
  }
  if (!points.isValid() || points.isEmpty()) return {};

  double pixelsPerPoint = qMax(1.0, static_cast<double>(dpi) / 72.0);
  QSize imageSize(qMax(1, static_cast<int>(std::ceil(points.width() * pixelsPerPoint))),
                  qMax(1, static_cast<int>(std::ceil(points.height() * pixelsPerPoint))));
  QImage image(imageSize, QImage::Format_ARGB32_Premultiplied);
  image.fill(Qt::transparent);
  QPainter painter(&image);
  renderer.render(&painter, QRectF(0, 0, imageSize.width(), imageSize.height()));
  painter.end();

  return cropRenderedPreview(image, minWidthPt, dpi, paddingPt);
}

int runSmoke(const QString &repoRoot, const QString &runtimeRoot, const QString &profileRoot, const QString &texmfRoot) {
  QDir(QDir(repoRoot).filePath("build")).mkpath(".");
  QFile smokeLog(QDir(repoRoot).filePath("build/gui-smoke.log"));
  (void)smokeLog.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Append);
  QTextStream log(&smokeLog);
  auto logLine = [&](const QString &text) {
    if (!smokeLog.isOpen()) return;
    log << text << '\n';
    log.flush();
  };
  QByteArray repo = QDir::cleanPath(repoRoot).toUtf8();
  QByteArray runtime = QDir::cleanPath(runtimeRoot).toUtf8();
  QByteArray texmf = QDir::cleanPath(texmfRoot).toUtf8();
  QByteArray profile = QDir::cleanPath(profileRoot).toUtf8();
  QDir runtimeDir(QString::fromUtf8(runtime));
  printf("repoRoot=%s\nruntimeRoot=%s\ntexmfRoot=%s\nprofileRoot=%s\nruntimeHasWorkerHost=%d runtimeHasXetexdaemon=%d runtimeHasDvipdfmxDll=%d runtimeHasDvisvgmDll=%d profileHasWarmup=%d\n",
         repo.constData(), runtime.constData(), texmf.constData(), profile.constData(),
         QFileInfo::exists(runtimeDir.filePath("bin/windows/stemtex-worker-host.exe")) ? 1 : 0,
         QFileInfo::exists(runtimeDir.filePath("bin/windows/xetexdaemon.exe")) ? 1 : 0,
         QFileInfo::exists(runtimeDir.filePath("bin/windows/dvipdfmxdaemon.dll")) ? 1 : 0,
         QFileInfo::exists(runtimeDir.filePath("bin/windows/dvisvgmdaemon.dll")) ? 1 : 0,
         QFileInfo::exists(QDir(QString::fromUtf8(profile)).filePath("warmup.tex")) ? 1 : 0);
  logLine(QString("repoRoot=%1").arg(QString::fromUtf8(repo)));
  logLine(QString("runtimeRoot=%1").arg(QString::fromUtf8(runtime)));
  logLine(QString("texmfRoot=%1").arg(QString::fromUtf8(texmf)));
  logLine(QString("profileRoot=%1").arg(QString::fromUtf8(profile)));
  StemTeXConfig cfg{};
  cfg.repo_root_utf8 = repo.constData();
  cfg.runtime_root_utf8 = runtime.constData();
  cfg.texmf_root_utf8 = texmf.constData();
  cfg.profile_root_utf8 = profile.constData();
  cfg.request_timeout_ms = 90000;
  cfg.xdvipdfmx_timeout_ms = 90000;
  cfg.spare_worker_count = 0;

  StemTeXErrorCode code = STEMTEX_OK;
  char *error = nullptr;
  StemTeXRenderer *renderer = stemtex_renderer_create(&cfg, &code, &error);
  if (!renderer) {
    fprintf(stderr, "create failed code=%d: %s\n", (int)code, error ? error : "");
    logLine(QString("create failed code=%1: %2").arg((int)code).arg(error ? QString::fromUtf8(error) : QString()));
    stemtex_renderer_free_string(error);
    return 1;
  }
  logLine(QString("renderer version=%1 abi=%2").arg(stemtex_renderer_version(), stemtex_renderer_abi_version()));

  QByteArray snippet = defaultSnippet().toUtf8();
  StemTeXRenderOutputResult pdfResult{};
  int ok = stemtex_renderer_render_output(renderer, snippet.constData(), 360.5, STEMTEX_OUTPUT_PDF, &pdfResult,
                                          &code, &error);
  if (!ok) {
    fprintf(stderr, "PDF render failed code=%d: %s\n", (int)code, error ? error : "");
    logLine(QString("PDF render failed code=%1: %2").arg((int)code).arg(error ? QString::fromUtf8(error) : QString()));
    stemtex_renderer_free_string(error);
    stemtex_renderer_destroy(renderer);
    return 1;
  }

  QString pdfPath = QString::fromUtf8(pdfResult.output_path_utf8);
  QString summary = QString::fromUtf8(pdfResult.summary_json_utf8);
  QPdfDocument pdf;
  QPdfDocument::Error pdfError = pdf.load(pdfPath);
  int pages = pdf.pageCount();
  QSizeF pageSize = pages > 0 ? pdf.pagePointSize(0) : QSizeF();
  CroppedPreview cropped = renderCroppedPdfPreview(pdfPath, 360.5, 300, 8.0);
  printf("pdf=%s\nsummary=%s\nqtPdfError=%d pages=%d pagePoints=%.2fx%.2f croppedPixels=%dx%d\n",
         pdfPath.toUtf8().constData(), summary.toUtf8().constData(),
         (int)pdfError, pages, pageSize.width(), pageSize.height(), cropped.image.width(), cropped.image.height());
  logLine(QString("pdf=%1").arg(pdfPath));
  logLine(QString("summary=%1").arg(summary));
  logLine(QString("qtPdfError=%1 pages=%2 pagePoints=%3x%4 croppedPixels=%5x%6")
              .arg((int)pdfError)
              .arg(pages)
              .arg(pageSize.width())
              .arg(pageSize.height())
              .arg(cropped.image.width())
              .arg(cropped.image.height()));
  if (!summary.contains("\"xdvipdfmxMode\":\"daemon-dll\"") &&
      !summary.contains("\"xdvipdfmxMode\":\"dll\"") &&
      !summary.contains("\"xdvipdfmxMode\":\"process-isolated\"")) {
    fprintf(stderr, "expected a supported xdvipdfmxMode, got summary=%s\n", summary.toUtf8().constData());
    logLine(QString("expected supported xdvipdfmxMode"));
    stemtex_renderer_free_output_result(&pdfResult);
    stemtex_renderer_destroy(renderer);
    return 1;
  }

  StemTeXRenderOutputResult svgResult{};
  ok = stemtex_renderer_render_output(renderer, snippet.constData(), 360.5, STEMTEX_OUTPUT_SVG, &svgResult, &code,
                                      &error);
  if (!ok) {
    fprintf(stderr, "SVG render failed code=%d: %s\n", (int)code, error ? error : "");
    logLine(QString("SVG render failed code=%1: %2").arg((int)code).arg(error ? QString::fromUtf8(error) : QString()));
    stemtex_renderer_free_string(error);
    stemtex_renderer_free_output_result(&pdfResult);
    stemtex_renderer_destroy(renderer);
    return 1;
  }
  QString svgPath = QString::fromUtf8(svgResult.output_path_utf8);
  QString svgSummary = QString::fromUtf8(svgResult.summary_json_utf8);
  QSizeF svgSize = svgPointSize(svgPath);
  CroppedPreview svgPreview = renderSvgPreview(svgPath, 360.5, 300, 8.0);
  printf("svg=%s\nsummary=%s\nsvgPreviewPixels=%dx%d\n", svgPath.toUtf8().constData(),
         svgSummary.toUtf8().constData(), svgPreview.image.width(), svgPreview.image.height());
  logLine(QString("svg=%1").arg(svgPath));
  logLine(QString("svgSummary=%1").arg(svgSummary));
  logLine(QString("svgPoints=%1x%2 svgPreviewPixels=%3x%4")
              .arg(svgSize.width())
              .arg(svgSize.height())
              .arg(svgPreview.image.width())
              .arg(svgPreview.image.height()));
  if (!svgSummary.contains("\"dvisvgmMode\":\"daemon-dll\"") || svgPreview.image.isNull()) {
    fprintf(stderr, "expected dvisvgm SVG output, got summary=%s\n", svgSummary.toUtf8().constData());
    logLine(QString("expected dvisvgm SVG output"));
    stemtex_renderer_free_output_result(&svgResult);
    stemtex_renderer_free_output_result(&pdfResult);
    stemtex_renderer_destroy(renderer);
    return 1;
  }
  if (pageSize.isValid() && svgSize.isValid() &&
      (std::abs(pageSize.width() - svgSize.width()) > 0.25 ||
       std::abs(pageSize.height() - svgSize.height()) > 0.25)) {
    fprintf(stderr, "SVG page size %.3fx%.3f does not match PDF page size %.3fx%.3f\n",
            svgSize.width(), svgSize.height(), pageSize.width(), pageSize.height());
    logLine(QString("SVG page size mismatch svg=%1x%2 pdf=%3x%4")
                .arg(svgSize.width())
                .arg(svgSize.height())
                .arg(pageSize.width())
                .arg(pageSize.height()));
    stemtex_renderer_free_output_result(&svgResult);
    stemtex_renderer_free_output_result(&pdfResult);
    stemtex_renderer_destroy(renderer);
    return 1;
  }
  if (!cropped.image.isNull() && !svgPreview.image.isNull() &&
      (std::abs(cropped.image.width() - svgPreview.image.width()) > 8 ||
       std::abs(cropped.image.height() - svgPreview.image.height()) > 8)) {
    fprintf(stderr, "SVG preview crop %dx%d does not match PDF preview crop %dx%d\n",
            svgPreview.image.width(), svgPreview.image.height(), cropped.image.width(), cropped.image.height());
    logLine(QString("SVG preview crop mismatch svg=%1x%2 pdf=%3x%4")
                .arg(svgPreview.image.width())
                .arg(svgPreview.image.height())
                .arg(cropped.image.width())
                .arg(cropped.image.height()));
    stemtex_renderer_free_output_result(&svgResult);
    stemtex_renderer_free_output_result(&pdfResult);
    stemtex_renderer_destroy(renderer);
    return 1;
  }
  stemtex_renderer_free_output_result(&svgResult);
  stemtex_renderer_free_output_result(&pdfResult);
  stemtex_renderer_destroy(renderer);
  return pdfError == QPdfDocument::Error::None && pages > 0 && !svgPreview.image.isNull() ? 0 : 1;
}

}  // namespace

class MainWindow : public QMainWindow {
 public:
  MainWindow(QString repoRoot, QString runtimeRoot)
      : repo_root_(std::move(repoRoot)),
        runtime_root_(normalizeRuntimeRoot(runtimeRoot)),
        texmf_root_(defaultTexmfRoot(runtime_root_)) {
    setWindowTitle("StemTeX Renderer GUI");
    resize(1180, 760);

    auto *central = new QWidget(this);
    auto *rootLayout = new QVBoxLayout(central);
    rootLayout->setContentsMargins(10, 10, 10, 10);
    rootLayout->setSpacing(8);

    auto *toolbar = new QVBoxLayout();
    toolbar->setSpacing(6);
    auto *layoutRow = new QHBoxLayout();
    layoutRow->setSpacing(8);
    auto *runtimeRow = new QHBoxLayout();
    runtimeRow->setSpacing(8);
    auto *widthLabel = new QLabel("版心宽度", central);
    widthSlider_ = new QSlider(Qt::Horizontal, central);
    widthSlider_->setRange(widthToSliderValue(30.0), widthToSliderValue(450.0));
    widthSlider_->setSingleStep(widthToSliderValue(0.5));
    widthSlider_->setPageStep(widthToSliderValue(10.0));
    widthSlider_->setValue(widthToSliderValue(360.0));
    widthSpin_ = new QDoubleSpinBox(central);
    widthSpin_->setRange(30.0, 450.0);
    widthSpin_->setSingleStep(0.5);
    widthSpin_->setDecimals(1);
    widthSpin_->setSuffix(" pt");
    widthSpin_->setValue(360.0);
    fontSizeSpin_ = new QDoubleSpinBox(central);
    fontSizeSpin_->setRange(1.0, 200.0);
    fontSizeSpin_->setSingleStep(0.5);
    fontSizeSpin_->setDecimals(1);
    fontSizeSpin_->setSuffix(" pt");
    fontSizeSpin_->setValue(kDefaultFontSizePt);
    dpiSpin_ = new QSpinBox(central);
    dpiSpin_->setRange(72, 1152);
    dpiSpin_->setSingleStep(24);
    dpiSpin_->setSuffix(" dpi");
    dpiSpin_->setValue(300);
    paddingSpin_ = new QDoubleSpinBox(central);
    paddingSpin_->setRange(0.0, 24.0);
    paddingSpin_->setSingleStep(0.5);
    paddingSpin_->setDecimals(1);
    paddingSpin_->setSuffix(" pt");
    paddingSpin_->setValue(8.0);
    texmfLabel_ = new QLabel(QFileInfo(texmf_root_).fileName(), central);
    texmfLabel_->setToolTip(texmf_root_);
    texmfButton_ = new QPushButton("TeXLive...", central);
    profileCombo_ = new QComboBox(central);
    reloadProfiles();
    encodingCombo_ = new QComboBox(central);
    encodingCombo_->addItems({"UTF-8", "GBK", "Big5"});
    encodingCombo_->setCurrentText("UTF-8");
    outputCombo_ = new QComboBox(central);
    outputCombo_->addItem("PDF", STEMTEX_OUTPUT_PDF);
    outputCombo_->addItem("SVG", STEMTEX_OUTPUT_SVG);
    outputCombo_->setCurrentIndex(0);
    renderButton_ = new QPushButton("排版", central);
    copyImageButton_ = new QPushButton("复制图像", central);
    copyImageButton_->setEnabled(false);
    saveImageButton_ = new QPushButton("保存图像", central);
    saveImageButton_->setEnabled(false);
    openButton_ = new QPushButton("打开 PDF", central);
    openButton_->setEnabled(false);
    layoutRow->addWidget(widthLabel);
    layoutRow->addWidget(widthSlider_, 1);
    layoutRow->addWidget(widthSpin_);
    layoutRow->addWidget(new QLabel("字号", central));
    layoutRow->addWidget(fontSizeSpin_);
    layoutRow->addWidget(new QLabel("DPI", central));
    layoutRow->addWidget(dpiSpin_);
    layoutRow->addWidget(new QLabel("裁切余量", central));
    layoutRow->addWidget(paddingSpin_);
    toolbar->addLayout(layoutRow);

    runtimeRow->addWidget(new QLabel("TeXLive", central));
    runtimeRow->addWidget(texmfLabel_);
    runtimeRow->addWidget(texmfButton_);
    runtimeRow->addWidget(new QLabel("Profile", central));
    runtimeRow->addWidget(profileCombo_);
    runtimeRow->addWidget(new QLabel("输入编码", central));
    runtimeRow->addWidget(encodingCombo_);
    runtimeRow->addWidget(new QLabel("Output", central));
    runtimeRow->addWidget(outputCombo_);
    runtimeRow->addStretch(1);
    runtimeRow->addWidget(renderButton_);
    runtimeRow->addWidget(copyImageButton_);
    runtimeRow->addWidget(saveImageButton_);
    runtimeRow->addWidget(openButton_);
    toolbar->addLayout(runtimeRow);
    rootLayout->addLayout(toolbar);

    auto *splitter = new QSplitter(Qt::Horizontal, central);
    editor_ = new QsciScintilla(splitter);
    editor_->setUtf8(true);
    editor_->setText(defaultSnippet());
    editor_->setWrapMode(QsciScintilla::WrapWord);
    editor_->setMarginLineNumbers(0, true);
    editor_->setMarginWidth(0, "0000");
    editor_->setBraceMatching(QsciScintilla::SloppyBraceMatch);
    editor_->setCaretLineVisible(true);
    editor_->setCaretLineBackgroundColor(QColor(245, 248, 255));
    editor_->setAutoIndent(true);
    editor_->setIndentationsUseTabs(false);
    editor_->setIndentationWidth(2);
    editor_->setTabWidth(2);
    editor_->setFolding(QsciScintilla::BoxedTreeFoldStyle);
    QFont editorFont("Consolas", 11);
    editor_->setFont(editorFont);
    auto *lexer = new QsciLexerTeX(editor_);
    lexer->setDefaultFont(editorFont);
    lexer->setColor(QColor(34, 34, 34), QsciLexerTeX::Default);
    lexer->setColor(QColor(0, 92, 175), QsciLexerTeX::Command);
    lexer->setColor(QColor(105, 58, 8), QsciLexerTeX::Special);
    lexer->setColor(QColor(100, 70, 160), QsciLexerTeX::Group);
    lexer->setColor(QColor(20, 120, 70), QsciLexerTeX::Symbol);
    lexer->setColor(QColor(34, 34, 34), QsciLexerTeX::Text);
    editor_->setLexer(lexer);
    observedEditorText_ = editor_->text();

    auto *previewShell = new QWidget(splitter);
    auto *previewLayout = new QVBoxLayout(previewShell);
    previewLayout->setContentsMargins(0, 0, 0, 0);
    previewLayout->setSpacing(6);
    croppedPreview_ = new QLabel(previewShell);
    croppedPreview_->setAlignment(Qt::AlignHCenter | Qt::AlignTop);
    croppedPreview_->setMinimumSize(240, 240);
    croppedPreview_->setStyleSheet("QLabel { background: #808080; border: 1px solid #606060; padding: 12px; }");
    previewWarning_ = new QLabel("!", croppedPreview_);
    previewWarning_->setAlignment(Qt::AlignCenter);
    previewWarning_->setFixedSize(28, 28);
    previewWarning_->setStyleSheet(
        "QLabel { background: #b3261e; color: white; border: 1px solid #7f1d1d; border-radius: 14px; "
        "font-weight: 700; padding: 0; }");
    previewWarning_->hide();
    details_ = new QTextBrowser(previewShell);
    details_->setMaximumHeight(110);
    previewLayout->addWidget(croppedPreview_, 1);
    previewLayout->addWidget(details_);

    splitter->addWidget(editor_);
    splitter->addWidget(previewShell);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 1);
    rootLayout->addWidget(splitter, 1);
    setCentralWidget(central);
    engineStatusLabel_ = new QLabel(this);
    engineStatusLabel_->setTextFormat(Qt::RichText);
    engineStatusLabel_->setMinimumWidth(180);
    statusBar()->addWidget(engineStatusLabel_, 1);
    enginePollTimer_ = new QTimer(this);
    enginePollTimer_->setInterval(500);
    connect(enginePollTimer_, &QTimer::timeout, this, [this]() { refreshEngineStatus(); });
    editorPollTimer_ = new QTimer(this);
    editorPollTimer_->setInterval(200);
    connect(editorPollTimer_, &QTimer::timeout, this, [this]() {
      if (!editor_) return;
      QString current = editor_->text();
      if (current == observedEditorText_) return;
      observedEditorText_ = current;
      scheduleAutoRender();
    });
    autoRenderTimer_ = new QTimer(this);
    autoRenderTimer_->setSingleShot(true);
    autoRenderTimer_->setInterval(250);
    connect(autoRenderTimer_, &QTimer::timeout, this, [this]() { renderSnippet(); });

    connect(widthSlider_, &QSlider::valueChanged, this, [this](int value) {
      QSignalBlocker blocker(widthSpin_);
      widthSpin_->setValue(sliderValueToWidth(value));
      updatePreviewMinimumWidth(widthSpin_->value());
      scheduleAutoRender();
    });
    connect(widthSpin_, &QDoubleSpinBox::valueChanged, this, [this](double value) {
      QSignalBlocker blocker(widthSlider_);
      widthSlider_->setValue(widthToSliderValue(value));
      updatePreviewMinimumWidth(value);
      scheduleAutoRender();
    });
    connect(fontSizeSpin_, &QDoubleSpinBox::valueChanged, this, [this](double) { scheduleAutoRender(); });
    connect(dpiSpin_, &QSpinBox::valueChanged, this, [this](int) { rerenderLastPreview(); });
    connect(paddingSpin_, &QDoubleSpinBox::valueChanged, this, [this](double) { rerenderLastPreview(); });
    connect(encodingCombo_, &QComboBox::currentTextChanged, this, [this](const QString &) { scheduleAutoRender(); });
    connect(outputCombo_, &QComboBox::currentIndexChanged, this, [this](int) {
      updateOpenButtonText();
      scheduleAutoRender();
    });
    connect(texmfButton_, &QPushButton::clicked, this, [this]() { chooseTexmfRoot(); });
    connect(profileCombo_, &QComboBox::currentIndexChanged, this, [this](int) { switchProfile(); });
    connect(renderButton_, &QPushButton::clicked, this, [this]() {
      if (autoRenderTimer_) autoRenderTimer_->stop();
      renderSnippet();
    });
    connect(copyImageButton_, &QPushButton::clicked, this, [this]() { copyPreviewImage(); });
    connect(saveImageButton_, &QPushButton::clicked, this, [this]() { savePreviewImage(); });
    connect(openButton_, &QPushButton::clicked, this, [this]() {
      if (!lastOutputPath_.isEmpty()) QDesktopServices::openUrl(QUrl::fromLocalFile(lastOutputPath_));
    });

    updateEngineStatus(false, spareReady_, spareTarget_);
    setUiReady(true);
    updatePreviewMinimumWidth(widthSpin_->value());
    editorPollTimer_->start();
    startBackgroundWorker();
    initializeRenderer(true);
  }

  ~MainWindow() override {
    shuttingDown_.store(true);
    stopRenderer(false);
    stopBackgroundWorker();
  }

 private:
  void startBackgroundWorker() {
    backgroundWorker_ = std::thread([this]() { backgroundLoop(); });
  }

  bool postBackground(std::function<void()> task) {
    {
      std::lock_guard<std::mutex> lock(backgroundMutex_);
      if (backgroundStop_) return false;
      backgroundQueue_.push_back(std::move(task));
    }
    backgroundCv_.notify_one();
    return true;
  }

  void backgroundLoop() {
    while (true) {
      std::function<void()> task;
      {
        std::unique_lock<std::mutex> lock(backgroundMutex_);
        backgroundCv_.wait(lock, [this]() { return backgroundStop_ || !backgroundQueue_.empty(); });
        if (backgroundStop_ && backgroundQueue_.empty()) return;
        task = std::move(backgroundQueue_.front());
        backgroundQueue_.pop_front();
      }
      task();
    }
  }

  void stopBackgroundWorker() {
    {
      std::lock_guard<std::mutex> lock(backgroundMutex_);
      backgroundStop_ = true;
    }
    backgroundCv_.notify_all();
    if (backgroundWorker_.joinable()) backgroundWorker_.join();
  }

  void setUiReady(bool ready) {
    (void)ready;
    bool hasProfile = profileCombo_ && profileCombo_->currentIndex() >= 0;
    renderButton_->setEnabled(hasProfile);
    if (profileCombo_) profileCombo_->setEnabled(profileCombo_->count() > 0);
    if (outputCombo_) outputCombo_->setEnabled(true);
  }

  void setPreviewImageReady(bool ready) {
    copyImageButton_->setEnabled(ready);
    saveImageButton_->setEnabled(ready);
  }

  void scheduleAutoRender() {
    if (shuttingDown_.load() || !autoRenderTimer_) return;
    if (!profileCombo_ || profileCombo_->currentIndex() < 0) return;
    if (!hasRenderer()) {
      pendingStartupRender_ = true;
      return;
    }
    autoRenderTimer_->start();
  }

  StemTeXOutputFormat selectedOutputFormat() const {
    int value = outputCombo_ ? outputCombo_->currentData().toInt() : STEMTEX_OUTPUT_PDF;
    return value == STEMTEX_OUTPUT_SVG ? STEMTEX_OUTPUT_SVG : STEMTEX_OUTPUT_PDF;
  }

  QString selectedOutputFormatText() const {
    return selectedOutputFormat() == STEMTEX_OUTPUT_SVG ? "SVG" : "PDF";
  }

  void updateOpenButtonText() {
    if (!openButton_) return;
    QString label = lastOutputFormat_.isEmpty() ? selectedOutputFormatText() : lastOutputFormat_.toUpper();
    openButton_->setText(QString("打开 %1").arg(label));
  }

  QString lightHtml(bool ok) const {
    return QString("<span style=\"color:%1;font-size:14px;\">&#9679;</span>").arg(ok ? "#179c48" : "#c62828");
  }

  void updateEngineStatus(bool primaryOk, int spareReady, int spareTarget, const QString &note = QString()) {
    spareReady_ = qMax(0, spareReady);
    spareTarget_ = qMax(0, spareTarget);
    QString engineText = primaryOk ? "primary ready" : "preparing primary ...";
    QString text = lightHtml(primaryOk);
    text += QString(" <span style=\"color:#333;\">%1</span>").arg(engineText);
    text += QString(" <span style=\"color:#777;\">spares %1/%2</span>").arg(spareReady_).arg(spareTarget_);
    text += " ";
    for (int i = 0; i < spareTarget_; ++i) {
      text += lightHtml(i < spareReady_);
    }
    QString shownNote = note;
    if (!shownNote.isEmpty()) {
      engineNote_ = shownNote;
      text += QString(" <span style=\"color:#555;\">%1</span>").arg(shownNote.toHtmlEscaped());
    } else {
      engineNote_.clear();
    }
    engineStatusLabel_->setText(text);
  }

  QString snapshotNote(const StemTeXEngineSnapshot &snapshot, const QString &overrideNote = QString()) const {
    if (!overrideNote.isEmpty()) return overrideNote;
    QString note;
    switch (snapshot.stage) {
      case STEMTEX_STAGE_QUEUED: note = "latest request queued"; break;
      case STEMTEX_STAGE_TYPESETTING: note = "XeTeX typesetting"; break;
      case STEMTEX_STAGE_CONVERTING: note = "converting output"; break;
      case STEMTEX_STAGE_REBUILDING: note = "worker rebuilding"; break;
      case STEMTEX_STAGE_STOPPING: note = "renderer stopping"; break;
      case STEMTEX_STAGE_IDLE:
      default: note = "idle"; break;
    }
    if (snapshot.async_running) {
      note += QString(", running job %1").arg(QString::number(static_cast<qulonglong>(snapshot.running_job_id)));
    }
    if (snapshot.async_pending) {
      note += QString(", pending job %1").arg(QString::number(static_cast<qulonglong>(snapshot.pending_job_id)));
    }
    if (snapshot.spare_rebuilding) {
      note += ", rebuilding spare";
    }
    return note;
  }

  void refreshEngineStatus(const QString &overrideNote = QString()) {
    std::lock_guard<std::mutex> lock(rendererMutex_);
    if (!renderer_) return;
    StemTeXEngineSnapshot snapshot{};
    if (!stemtex_renderer_engine_snapshot(renderer_, &snapshot)) return;
    updateEngineStatus(snapshot.primary_ready != 0, snapshot.spare_ready, snapshot.spare_target,
                       snapshotNote(snapshot, overrideNote));
  }

  QString selectedProfileRoot() const {
    return profileCombo_ && profileCombo_->currentIndex() >= 0 ? profileCombo_->currentData().toString() : QString();
  }

  void reloadProfiles() {
    QString profileError;
    bool oldSignals = profileCombo_->blockSignals(true);
    profileCombo_->clear();
    for (const ProfileEntry &profile : profileRoots(repo_root_, &profileError)) {
      profileCombo_->addItem(profile.name, profile.path);
    }
    profileCombo_->blockSignals(oldSignals);
    profileCombo_->setToolTip(profileCombo_->count() == 0 ? profileError : QString());
    QString shown = QFileInfo(texmf_root_).fileName();
    if (shown.isEmpty()) shown = texmf_root_;
    texmfLabel_->setText(shown);
    texmfLabel_->setToolTip(QString("TeXLive package/font tree: %1\nDaemon runtime: %2").arg(texmf_root_, runtime_root_));
  }

  void destroyRendererLater(StemTeXRenderer *renderer) {
    if (!renderer) return;
    if (postBackground([renderer]() {
      std::lock_guard<std::mutex> lock(gRendererLifecycleMutex);
      stemtex_renderer_destroy(renderer);
    })) {
      return;
    }
    std::lock_guard<std::mutex> lock(gRendererLifecycleMutex);
    stemtex_renderer_destroy(renderer);
  }

  void stopRenderer(bool asyncDestroy = true) {
    ++rendererGeneration_;
    ++latestUiRequestId_;
    enginePollTimer_->stop();
    if (autoRenderTimer_) autoRenderTimer_->stop();
    pendingStartupRender_ = false;
    StemTeXRenderer *renderer = nullptr;
    {
      std::lock_guard<std::mutex> lock(rendererMutex_);
      renderer = renderer_;
      renderer_ = nullptr;
    }
    if (asyncDestroy) {
      destroyRendererLater(renderer);
    } else if (renderer) {
      std::lock_guard<std::mutex> lifecycleLock(gRendererLifecycleMutex);
      stemtex_renderer_destroy(renderer);
    }
  }

  void clearProfileOutput() {
    lastOutputPath_.clear();
    lastOutputFormat_.clear();
    lastPreview_ = QImage();
    lastPreviewDisplaySize_ = QSize();
    lastSummaryText_.clear();
    lastOutcomeMessage_.clear();
    lastOutcomeCode_ = STEMTEX_RENDER_OUTCOME_OK;
    lastIssueFlags_ = 0;
    croppedPreview_->clear();
    croppedPreview_->setText(QString());
    setPreviewWarning(STEMTEX_RENDER_OUTCOME_OK, 0, QString());
    details_->clear();
    openButton_->setEnabled(false);
    updateOpenButtonText();
    setPreviewImageReady(false);
  }

  bool hasRenderer() const {
    std::lock_guard<std::mutex> lock(rendererMutex_);
    return renderer_ != nullptr;
  }

  StemTeXRenderer *currentRenderer() const {
    std::lock_guard<std::mutex> lock(rendererMutex_);
    return renderer_;
  }

  void installRenderer(StemTeXRenderer *renderer) {
    std::lock_guard<std::mutex> lock(rendererMutex_);
    if (renderer_) {
      std::lock_guard<std::mutex> lifecycleLock(gRendererLifecycleMutex);
      stemtex_renderer_destroy(renderer_);
    }
    renderer_ = renderer;
  }

  void chooseTexmfRoot() {
    QString selected = QFileDialog::getExistingDirectory(this, "Select TeXLive package/font tree", texmf_root_);
    if (selected.isEmpty()) return;
    QString normalized = normalizeTexmfRoot(selected);
    if (normalized == texmf_root_) return;
    stopRenderer();
    clearProfileOutput();
    texmf_root_ = normalized;
    reloadProfiles();
    setUiReady(true);
    updateEngineStatus(false, 0, spareTarget_, QString("texmf: %1").arg(texmf_root_));
    initializeRenderer(true);
  }

  void switchProfile() {
    QString profileName = profileCombo_->currentText();
    stopRenderer(true);
    clearProfileOutput();
    updateEngineStatus(false, 0, spareTarget_, QString("profile: %1").arg(profileName));
    if (!profileCombo_ || profileCombo_->currentIndex() < 0) {
      setUiReady(true);
      return;
    }
    initializeRenderer(false);
  }

  void initializeRenderer(bool renderAfterInit) {
    QString profileRoot = selectedProfileRoot();
    if (profileRoot.isEmpty()) {
      updateEngineStatus(false, 0, spareTarget_, "choose a profile");
      setUiReady(true);
      return;
    }
    setUiReady(false);
    updateEngineStatus(false, 0, spareTarget_, QString("starting profile: %1").arg(QFileInfo(profileRoot).fileName()));
    uint64_t generation = ++rendererGeneration_;
    QString repoRoot = repo_root_;
    QString runtimeRoot = runtime_root_;
    QString texmfRoot = texmf_root_;
    postBackground([this, repoRoot, runtimeRoot, texmfRoot, profileRoot, generation, renderAfterInit]() {
      if (shuttingDown_.load() || generation != rendererGeneration_.load()) return;
      auto start = std::chrono::steady_clock::now();
      QByteArray repo = QDir::cleanPath(repoRoot).toUtf8();
      QByteArray runtime = QDir::cleanPath(runtimeRoot).toUtf8();
      QByteArray texmf = QDir::cleanPath(texmfRoot).toUtf8();
      QByteArray profile = QDir::cleanPath(profileRoot).toUtf8();
      StemTeXConfig cfg{};
      cfg.repo_root_utf8 = repo.constData();
      cfg.runtime_root_utf8 = runtime.constData();
      cfg.texmf_root_utf8 = texmf.constData();
      cfg.profile_root_utf8 = profile.constData();
      cfg.request_timeout_ms = 90000;
      cfg.xdvipdfmx_timeout_ms = 90000;
      cfg.spare_worker_count = 0;
      StemTeXErrorCode code = STEMTEX_OK;
      char *error = nullptr;
      StemTeXRenderer *renderer = nullptr;
      bool installed = false;
      bool stale = false;
      {
        std::lock_guard<std::mutex> lifecycleLock(gRendererLifecycleMutex);
        renderer = stemtex_renderer_create(&cfg, &code, &error);
        stale = shuttingDown_.load() || generation != rendererGeneration_.load();
        if (renderer && !stale) {
          std::lock_guard<std::mutex> rendererLock(rendererMutex_);
          if (!renderer_) {
            renderer_ = renderer;
            renderer = nullptr;
            installed = true;
          } else {
            stale = true;
          }
        }
        if (renderer && stale) {
          stemtex_renderer_destroy(renderer);
          renderer = nullptr;
        }
      }
      auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
      QString errorText = error ? QString::fromUtf8(error) : QString();
      stemtex_renderer_free_string(error);
      if (stale && !installed) return;
      QMetaObject::invokeMethod(this, [this, installed, code, errorText, elapsed, generation, renderAfterInit]() {
        if (shuttingDown_.load() || generation != rendererGeneration_.load()) {
          return;
        }
        if (!installed) {
          setUiReady(false);
          updateEngineStatus(false, 0, spareTarget_, QString("renderer init failed, code %1").arg((int)code));
          details_->setPlainText(errorText);
          return;
        }
        setUiReady(true);
        refreshEngineStatus(QString("renderer initialized in %1 ms").arg(elapsed));
        enginePollTimer_->start();
        if (pendingStartupRender_) {
          pendingStartupRender_ = false;
          renderSnippet();
        } else if (renderAfterInit) {
          scheduleAutoRender();
        }
      }, Qt::QueuedConnection);
    });
  }

  void renderSnippet() {
    if (autoRenderTimer_) autoRenderTimer_->stop();
    uint64_t generation = rendererGeneration_.load();
    if (!hasRenderer()) {
      pendingStartupRender_ = true;
      updateEngineStatus(false, spareReady_, spareTarget_, "renderer is still starting; this request will run after init");
      setPreviewImageReady(false);
      details_->clear();
      return;
    }
    QString snippet = editor_->text();
    QString encoding = encodingCombo_->currentText();
    double width = widthSpin_->value();
    double fontSize = fontSizeSpin_->value();
    StemTeXOutputFormat outputFormat = selectedOutputFormat();
    QString outputLabel = outputFormat == STEMTEX_OUTPUT_SVG ? "SVG" : "PDF";
    refreshEngineStatus(QString("render %1 request submitted").arg(outputLabel));
    setPreviewImageReady(false);
    details_->clear();
    QByteArray text = encodeSnippetForTeX(snippet, encoding);
    uint64_t uiRequestId = ++latestUiRequestId_;
    if (!postBackground([this, text, width, fontSize, outputFormat, outputLabel, uiRequestId, generation]() {
      StemTeXErrorCode code = STEMTEX_OK;
      char *error = nullptr;
      StemTeXRenderOutputResult result{};
      int ok = 0;
      if (!shuttingDown_.load() && generation == rendererGeneration_.load() &&
          uiRequestId == latestUiRequestId_.load()) {
        std::lock_guard<std::mutex> lifecycleLock(gRendererLifecycleMutex);
        StemTeXRenderer *renderer = nullptr;
        {
          std::lock_guard<std::mutex> rendererLock(rendererMutex_);
          renderer = renderer_;
        }
        if (renderer && !shuttingDown_.load() && generation == rendererGeneration_.load() &&
            uiRequestId == latestUiRequestId_.load()) {
          ok = stemtex_renderer_render_output_with_font_size(renderer, text.constData(), width, fontSize, outputFormat,
                                                             &result, &code, &error);
        } else {
          code = STEMTEX_ERROR_CANCELLED;
        }
      } else {
        code = STEMTEX_ERROR_CANCELLED;
      }
      QString outputPath = ok && result.output_path_utf8 ? QString::fromUtf8(result.output_path_utf8) : QString();
      QString resultFormat = ok && result.output_format_utf8 ? QString::fromUtf8(result.output_format_utf8)
                                                             : outputLabel.toLower();
      QString summary = ok && result.summary_json_utf8 ? QString::fromUtf8(result.summary_json_utf8) : QString();
      StemTeXRenderOutcomeCode outcomeCode = ok ? result.outcome_code : STEMTEX_RENDER_OUTCOME_INTERNAL;
      int issueFlags = ok ? result.issue_flags : 0;
      QString outcomeMessage =
          ok && result.outcome_message_utf8 ? QString::fromUtf8(result.outcome_message_utf8) : QString();
      QString errorText = error ? QString::fromUtf8(error) : QString();
      if (!ok && code == STEMTEX_ERROR_CANCELLED && errorText.isEmpty()) {
        errorText = "older request skipped because a newer request was submitted";
      }
      stemtex_renderer_free_string(error);
      stemtex_renderer_free_output_result(&result);
      QMetaObject::invokeMethod(this, [this, uiRequestId, generation, ok, code, outputPath, resultFormat, summary,
                                       outcomeCode, issueFlags, outcomeMessage, errorText]() {
        if (shuttingDown_.load() || generation != rendererGeneration_.load() ||
            uiRequestId != latestUiRequestId_.load()) return;
        if (!ok) {
          setPreviewWarning(STEMTEX_RENDER_OUTCOME_INTERNAL, 0, errorText);
          refreshEngineStatus(code == STEMTEX_ERROR_CANCELLED
                                  ? QString("older request skipped because a newer request was submitted")
                                  : QString("render failed, code %1").arg((int)code));
          if (code != STEMTEX_ERROR_CANCELLED) details_->setPlainText(errorText);
          return;
        }
        refreshEngineStatus();
        lastOutputPath_ = outputPath;
        lastOutputFormat_ = resultFormat;
        updateOpenButtonText();
        openButton_->setEnabled(!lastOutputPath_.isEmpty());
        showOutputPreview(outputPath, resultFormat, widthSpin_->value());
        setPreviewImageReady(!lastPreview_.isNull());
        lastOutcomeCode_ = outcomeCode;
        lastIssueFlags_ = issueFlags;
        lastOutcomeMessage_ = outcomeMessage;
        setPreviewWarning(outcomeCode, issueFlags, outcomeMessage);
        lastSummaryText_ = QString("renderer request: %1\n").arg(uiRequestId) + oneLineJsonMetric(summary) +
                           "\n\n" + summary;
        updateDetailsText();
      }, Qt::QueuedConnection);
    })) {}
  }

  void showCroppedPreview(const QString &pdfPath, double widthPt) {
    CroppedPreview cropped = renderCroppedPdfPreview(pdfPath, widthPt, dpiSpin_->value(), paddingSpin_->value());
    if (cropped.image.isNull()) {
      lastPreview_ = QImage();
      lastPreviewDisplaySize_ = QSize();
      croppedPreview_->clear();
      croppedPreview_->setText("PDF preview failed");
      return;
    }
    lastPreview_ = cropped.image;
    lastPreviewDisplaySize_ = cropped.displaySize;
    updatePreviewPixmap();
  }

  void showSvgPreview(const QString &svgPath, double widthPt) {
    CroppedPreview preview = renderSvgPreview(svgPath, widthPt, dpiSpin_->value(), paddingSpin_->value());
    if (preview.image.isNull()) {
      lastPreview_ = QImage();
      lastPreviewDisplaySize_ = QSize();
      croppedPreview_->clear();
      croppedPreview_->setText("SVG preview failed");
      return;
    }
    lastPreview_ = preview.image;
    lastPreviewDisplaySize_ = preview.displaySize;
    updatePreviewPixmap();
  }

  void showOutputPreview(const QString &outputPath, const QString &format, double widthPt) {
    if (format.compare("svg", Qt::CaseInsensitive) == 0) {
      showSvgPreview(outputPath, widthPt);
    } else {
      showCroppedPreview(outputPath, widthPt);
    }
  }

  void rerenderLastPreview() {
    if (lastOutputPath_.isEmpty()) {
      updatePreviewPixmap();
      return;
    }
    showOutputPreview(lastOutputPath_, lastOutputFormat_, widthSpin_->value());
    setPreviewImageReady(!lastPreview_.isNull());
    updateDetailsText();
  }

  void copyPreviewImage() {
    if (lastPreview_.isNull()) return;
    QApplication::clipboard()->setImage(lastPreview_);
    updateEngineStatus(hasRenderer(), spareReady_, spareTarget_);
  }

  void savePreviewImage() {
    if (lastPreview_.isNull()) return;
    QString path = QFileDialog::getSaveFileName(this, "Save image", QDir::home().filePath("stemtex-snippet.png"),
                                                "PNG image (*.png);;JPEG image (*.jpg *.jpeg);;BMP image (*.bmp)");
    if (path.isEmpty()) return;
    if (!lastPreview_.save(path)) {
      updateEngineStatus(hasRenderer(), spareReady_, spareTarget_, "save failed");
      return;
    }
    updateEngineStatus(hasRenderer(), spareReady_, spareTarget_);
  }

  void updatePreviewPixmap() {
    if (lastPreview_.isNull()) return;
    QSize target(qMax(1, croppedPreview_->width() - 24), qMax(1, croppedPreview_->height() - 24));
    QSize logicalSize = lastPreviewDisplaySize_.isValid() ? lastPreviewDisplaySize_ : lastPreview_.size();
    QSize displaySize = logicalSize;
    if (displaySize.width() > target.width() || displaySize.height() > target.height()) {
      displaySize.scale(target, Qt::KeepAspectRatio);
    }
    double dpr = croppedPreview_->devicePixelRatioF();
    QSize physicalSize(qMax(1, qRound(displaySize.width() * dpr)), qMax(1, qRound(displaySize.height() * dpr)));
    QImage displayImage = lastPreview_.scaled(physicalSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    QPixmap pixmap = QPixmap::fromImage(displayImage);
    pixmap.setDevicePixelRatio(dpr);
    croppedPreview_->setPixmap(pixmap);
    positionPreviewWarning();
  }

  void positionPreviewWarning() {
    if (!previewWarning_) return;
    previewWarning_->move(qMax(8, croppedPreview_->width() - previewWarning_->width() - 18), 18);
    previewWarning_->raise();
  }

  void setPreviewWarning(StemTeXRenderOutcomeCode outcome, int issueFlags, const QString &message) {
    if (!previewWarning_) return;
    bool warning = outcome != STEMTEX_RENDER_OUTCOME_OK || issueFlags != 0;
    previewWarning_->setVisible(warning);
    previewWarning_->setToolTip(message);
    positionPreviewWarning();
  }

  void updateDetailsText() {
    if (!details_ || lastPreview_.isNull()) return;
    QString previewLabel = lastOutputFormat_.compare("svg", Qt::CaseInsensitive) == 0 ? "SVG preview raster"
                                                                                      : "cropped PDF preview";
    QString outcomeText;
    if (lastOutcomeCode_ != STEMTEX_RENDER_OUTCOME_OK || lastIssueFlags_ != 0) {
      outcomeText = QString("outcome: %1, issue flags: %2, %3\n")
                        .arg((int)lastOutcomeCode_)
                        .arg(lastIssueFlags_)
                        .arg(lastOutcomeMessage_);
    }
    details_->setPlainText(QString("%1: %2 x %3 px, displayed %4 x %5 px, %6 dpi, %7 pt padding\n")
                               .arg(previewLabel)
                               .arg(lastPreview_.width())
                               .arg(lastPreview_.height())
                               .arg(lastPreviewDisplaySize_.width())
                               .arg(lastPreviewDisplaySize_.height())
                               .arg(dpiSpin_->value())
                               .arg(paddingSpin_->value(), 0, 'f', 1) +
                           outcomeText + lastSummaryText_);
  }

  void updatePreviewMinimumWidth(double widthPt) {
    constexpr double screenPixelsPerPoint = 96.0 / 72.0;
    croppedPreview_->setMinimumWidth(qMax(240, static_cast<int>(std::ceil(widthPt * screenPixelsPerPoint)) + 48));
  }

  void resizeEvent(QResizeEvent *event) override {
    QMainWindow::resizeEvent(event);
    updatePreviewPixmap();
    positionPreviewWarning();
  }

  QString repo_root_;
  QString runtime_root_;
  QString texmf_root_;
  StemTeXRenderer *renderer_ = nullptr;
  mutable std::mutex rendererMutex_;
  std::atomic<bool> shuttingDown_{false};
  std::atomic<uint64_t> rendererGeneration_{0};
  QString lastOutputPath_;
  QString lastOutputFormat_;
  QsciScintilla *editor_ = nullptr;
  QSlider *widthSlider_ = nullptr;
  QDoubleSpinBox *widthSpin_ = nullptr;
  QDoubleSpinBox *fontSizeSpin_ = nullptr;
  QSpinBox *dpiSpin_ = nullptr;
  QDoubleSpinBox *paddingSpin_ = nullptr;
  QLabel *texmfLabel_ = nullptr;
  QPushButton *texmfButton_ = nullptr;
  QComboBox *profileCombo_ = nullptr;
  QComboBox *encodingCombo_ = nullptr;
  QComboBox *outputCombo_ = nullptr;
  QPushButton *renderButton_ = nullptr;
  QPushButton *copyImageButton_ = nullptr;
  QPushButton *saveImageButton_ = nullptr;
  QPushButton *openButton_ = nullptr;
  QLabel *croppedPreview_ = nullptr;
  QLabel *previewWarning_ = nullptr;
  QImage lastPreview_;
  QSize lastPreviewDisplaySize_;
  QString lastSummaryText_;
  StemTeXRenderOutcomeCode lastOutcomeCode_ = STEMTEX_RENDER_OUTCOME_OK;
  int lastIssueFlags_ = 0;
  QString lastOutcomeMessage_;
  QTextBrowser *details_ = nullptr;
  QLabel *engineStatusLabel_ = nullptr;
  QTimer *enginePollTimer_ = nullptr;
  QTimer *editorPollTimer_ = nullptr;
  QTimer *autoRenderTimer_ = nullptr;
  std::mutex backgroundMutex_;
  std::condition_variable backgroundCv_;
  std::deque<std::function<void()>> backgroundQueue_;
  std::thread backgroundWorker_;
  bool backgroundStop_ = false;
  QString observedEditorText_;
  int spareReady_ = 0;
  int spareTarget_ = 0;
  QString engineNote_;
  bool pendingStartupRender_ = false;
  std::atomic<uint64_t> latestUiRequestId_{0};
};

int main(int argc, char **argv) {
  qputenv("QT_QPA_PLATFORM", "windows");

  QApplication app(argc, argv);
  QApplication::setWindowIcon(QIcon(":/icons/stemtex-renderer-gui.png"));
  QStringList args = app.arguments();
  bool smoke = args.contains("--smoke");
  args.removeAll("--smoke");
  QString repoRoot = args.size() > 1 ? args.at(1) : appResourceRoot();
  QString runtimeRoot = args.size() > 2 ? args.at(2) : defaultRuntimeRoot();
  configureRendererDllSearch(runtimeRoot);
  QString profileError;
  QVector<ProfileEntry> profiles = profileRoots(repoRoot, &profileError);
  QString profileRoot = args.size() > 3 ? args.at(3) : (profiles.isEmpty() ? QString() : profiles.first().path);
  QString texmfRoot = args.size() > 4 ? args.at(4) : defaultTexmfRoot(runtimeRoot);
  if (smoke) return runSmoke(repoRoot, runtimeRoot, profileRoot, texmfRoot);
  MainWindow w(repoRoot, runtimeRoot);
  w.show();
  return app.exec();
}
