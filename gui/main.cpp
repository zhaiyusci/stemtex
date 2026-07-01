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
#include <QSlider>
#include <QSpinBox>
#include <QSplitter>
#include <QStatusBar>
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
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>

namespace {

std::mutex gRendererLifecycleMutex;

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
  int totalMs = obj.value("requestToPdfMs").toInt();
  int xdvipdfmxMs = obj.value("xdvipdfmxMs").toInt();
  int finalizeXdvMs = obj.value("finalizeXdvMs").toInt();
  int xetexNoPdfMs = qMax(0, totalMs - finalizeXdvMs - xdvipdfmxMs);
  return QString("XeTeX --no-pdf time: %1 ms\n"
                 "xdvipdfmx time: %2 ms\n"
                 "Total time until PDF complete: %3 ms\n"
                 "PDF: %4 bytes, spare: %5/%6")
      .arg(xetexNoPdfMs)
      .arg(xdvipdfmxMs)
      .arg(totalMs)
      .arg(obj.value("pdfBytes").toInt())
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

CroppedPreview renderCroppedPdfPreview(const QString &pdfPath, int minWidthPt, int dpi, double paddingPt) {
  QPdfDocument source;
  QPdfDocument::Error error = source.load(pdfPath);
  if (error != QPdfDocument::Error::None || source.pageCount() <= 0) return {};

  double pixelsPerPoint = qBound(1.0, dpi / 72.0, 16.0);
  QSizeF points = source.pagePointSize(0);
  QSize imageSize(qMax(1, int(points.width() * pixelsPerPoint)), qMax(1, int(points.height() * pixelsPerPoint)));
  QImage page = source.render(0, imageSize);
  if (page.isNull()) return {};

  QImage rgba = page.convertToFormat(QImage::Format_ARGB32);
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
  if (bounds.isNull()) return {rgba, rgba.size()};

  int pad = qMax(0, qRound(paddingPt * pixelsPerPoint));
  bounds = expandRectRightToWidth(bounds, qMax(1, int(minWidthPt * pixelsPerPoint)), rgba.rect());
  QImage cropped = rgba.copy(bounds);
  QImage white(cropped.width() + pad * 2, cropped.height() + pad * 2, QImage::Format_RGB32);
  white.fill(Qt::white);
  QPainter painter(&white);
  painter.drawImage(pad, pad, cropped);
  painter.end();
  constexpr double screenPixelsPerPoint = 96.0 / 72.0;
  QSize displaySize(qMax(1, int(white.width() / pixelsPerPoint * screenPixelsPerPoint)),
                    qMax(1, int(white.height() / pixelsPerPoint * screenPixelsPerPoint)));
  return {white, displaySize};
}

int runSmoke(const QString &repoRoot, const QString &runtimeRoot, const QString &profileRoot, const QString &texmfRoot) {
  QDir(QDir(repoRoot).filePath("build")).mkpath(".");
  QFile smokeLog(QDir(repoRoot).filePath("build/gui-smoke.log"));
  smokeLog.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Append);
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
  printf("repoRoot=%s\nruntimeRoot=%s\ntexmfRoot=%s\nprofileRoot=%s\nruntimeHasXetexdaemon=%d runtimeHasDvipdfmxDll=%d profileHasWarmup=%d\n",
         repo.constData(), runtime.constData(), texmf.constData(), profile.constData(),
         QFileInfo::exists(runtimeDir.filePath("bin/windows/xetexdaemon.exe")) ? 1 : 0,
         QFileInfo::exists(runtimeDir.filePath("bin/windows/dvipdfmxdaemon.dll")) ? 1 : 0,
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

  StemTeXRenderResult result{};
  QByteArray snippet = defaultSnippet().toUtf8();
  int ok = stemtex_renderer_render(renderer, snippet.constData(), 360, &result, &code, &error);
  if (!ok) {
    fprintf(stderr, "render failed code=%d: %s\n", (int)code, error ? error : "");
    logLine(QString("render failed code=%1: %2").arg((int)code).arg(error ? QString::fromUtf8(error) : QString()));
    stemtex_renderer_free_string(error);
    stemtex_renderer_destroy(renderer);
    return 1;
  }

  QString pdfPath = QString::fromUtf8(result.pdf_path_utf8);
  QString summary = QString::fromUtf8(result.summary_json_utf8);
  QPdfDocument pdf;
  QPdfDocument::Error pdfError = pdf.load(pdfPath);
  int pages = pdf.pageCount();
  QSizeF pageSize = pages > 0 ? pdf.pagePointSize(0) : QSizeF();
  CroppedPreview cropped = renderCroppedPdfPreview(pdfPath, 360, 300, 8.0);
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
    stemtex_renderer_free_result(&result);
    stemtex_renderer_destroy(renderer);
    return 1;
  }
  stemtex_renderer_free_result(&result);
  stemtex_renderer_destroy(renderer);
  return pdfError == QPdfDocument::Error::None && pages > 0 ? 0 : 1;
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
    widthSlider_->setRange(30, 450);
    widthSlider_->setSingleStep(10);
    widthSlider_->setPageStep(20);
    widthSlider_->setValue(360);
    widthSpin_ = new QSpinBox(central);
    widthSpin_->setRange(30, 450);
    widthSpin_->setSingleStep(10);
    widthSpin_->setSuffix(" pt");
    widthSpin_->setValue(360);
    dpiSpin_ = new QSpinBox(central);
    dpiSpin_->setRange(72, 9600);
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

    connect(widthSlider_, &QSlider::valueChanged, widthSpin_, &QSpinBox::setValue);
    connect(widthSpin_, &QSpinBox::valueChanged, widthSlider_, &QSlider::setValue);
    connect(widthSpin_, &QSpinBox::valueChanged, this, [this](int value) {
      updatePreviewMinimumWidth(value);
      scheduleAutoRender();
    });
    connect(dpiSpin_, &QSpinBox::valueChanged, this, [this](int) { rerenderLastPdfPreview(); });
    connect(paddingSpin_, &QDoubleSpinBox::valueChanged, this, [this](double) { rerenderLastPdfPreview(); });
    connect(encodingCombo_, &QComboBox::currentTextChanged, this, [this](const QString &) { scheduleAutoRender(); });
    connect(texmfButton_, &QPushButton::clicked, this, [this]() { chooseTexmfRoot(); });
    connect(profileCombo_, &QComboBox::currentIndexChanged, this, [this](int) { switchProfile(); });
    connect(renderButton_, &QPushButton::clicked, this, [this]() {
      if (autoRenderTimer_) autoRenderTimer_->stop();
      renderSnippet();
    });
    connect(copyImageButton_, &QPushButton::clicked, this, [this]() { copyPreviewImage(); });
    connect(saveImageButton_, &QPushButton::clicked, this, [this]() { savePreviewImage(); });
    connect(openButton_, &QPushButton::clicked, this, [this]() {
      if (!lastPdf_.isEmpty()) QDesktopServices::openUrl(QUrl::fromLocalFile(lastPdf_));
    });

    updateEngineStatus(false, spareReady_, spareTarget_);
    setUiReady(true);
    updatePreviewMinimumWidth(widthSpin_->value());
    editorPollTimer_->start();
    initializeRenderer(true);
  }

  ~MainWindow() override {
    shuttingDown_.store(true);
    stopRenderer(false);
  }

 private:
  void setUiReady(bool ready) {
    (void)ready;
    bool hasProfile = profileCombo_ && profileCombo_->currentIndex() >= 0;
    renderButton_->setEnabled(hasProfile);
    if (profileCombo_) profileCombo_->setEnabled(profileCombo_->count() > 0);
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
      case STEMTEX_STAGE_CONVERTING: note = "xdvipdfmx converting PDF"; break;
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
    std::thread([renderer]() {
      std::lock_guard<std::mutex> lock(gRendererLifecycleMutex);
      stemtex_renderer_destroy(renderer);
    }).detach();
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
    lastPdf_.clear();
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
    std::thread([this, profileRoot, generation, renderAfterInit]() {
      auto start = std::chrono::steady_clock::now();
      QByteArray repo = QDir::cleanPath(repo_root_).toUtf8();
      QByteArray runtime = QDir::cleanPath(runtime_root_).toUtf8();
      QByteArray texmf = QDir::cleanPath(texmf_root_).toUtf8();
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
    }).detach();
  }

  void renderSnippet() {
    if (autoRenderTimer_) autoRenderTimer_->stop();
    uint64_t generation = rendererGeneration_.load();
    StemTeXRenderer *renderer = currentRenderer();
    if (!renderer) {
      pendingStartupRender_ = true;
      updateEngineStatus(false, spareReady_, spareTarget_, "renderer is still starting; this request will run after init");
      setPreviewImageReady(false);
      details_->clear();
      return;
    }
    QString snippet = editor_->text();
    QString encoding = encodingCombo_->currentText();
    int width = widthSpin_->value();
    refreshEngineStatus("render request submitted; waiting for renderer scheduler");
    setPreviewImageReady(false);
    details_->clear();
    QByteArray text = encodeSnippetForTeX(snippet, encoding);
    uint64_t uiRequestId = ++latestUiRequestId_;
    struct CallbackContext {
      MainWindow *self = nullptr;
      uint64_t uiRequestId = 0;
      uint64_t generation = 0;
    };
    auto *context = new CallbackContext{this, uiRequestId, generation};
    auto callback = [](uint64_t rendererJobId, int ok, const StemTeXRenderResult *result, StemTeXErrorCode code,
                       const char *error, void *userData) {
      std::unique_ptr<CallbackContext> context(static_cast<CallbackContext *>(userData));
      MainWindow *self = context->self;
      uint64_t uiRequestId = context->uiRequestId;
      uint64_t generation = context->generation;
      QString pdfPath = result && result->pdf_path_utf8 ? QString::fromUtf8(result->pdf_path_utf8) : QString();
      QString summary = result && result->summary_json_utf8 ? QString::fromUtf8(result->summary_json_utf8) : QString();
      StemTeXRenderOutcomeCode outcomeCode = result ? result->outcome_code : STEMTEX_RENDER_OUTCOME_INTERNAL;
      int issueFlags = result ? result->issue_flags : 0;
      QString outcomeMessage =
          result && result->outcome_message_utf8 ? QString::fromUtf8(result->outcome_message_utf8) : QString();
      QString errorText = error ? QString::fromUtf8(error) : QString();
      QMetaObject::invokeMethod(self, [self, uiRequestId, generation, rendererJobId, ok, code, pdfPath, summary,
                                       outcomeCode, issueFlags, outcomeMessage, errorText]() {
        if (self->shuttingDown_.load() || generation != self->rendererGeneration_.load() ||
            uiRequestId != self->latestUiRequestId_) return;
        if (!ok) {
          self->setPreviewWarning(STEMTEX_RENDER_OUTCOME_INTERNAL, 0, errorText);
          self->refreshEngineStatus(code == STEMTEX_ERROR_CANCELLED
                                        ? QString("older request skipped because a newer request was submitted")
                                        : QString("render failed, code %1").arg((int)code));
          if (code != STEMTEX_ERROR_CANCELLED) self->details_->setPlainText(errorText);
          return;
        }
        self->refreshEngineStatus();
        self->lastPdf_ = pdfPath;
        self->openButton_->setEnabled(true);
        self->showCroppedPreview(pdfPath, self->widthSpin_->value());
        self->setPreviewImageReady(!self->lastPreview_.isNull());
        self->lastOutcomeCode_ = outcomeCode;
        self->lastIssueFlags_ = issueFlags;
        self->lastOutcomeMessage_ = outcomeMessage;
        self->setPreviewWarning(outcomeCode, issueFlags, outcomeMessage);
        self->lastSummaryText_ = QString("renderer job: %1\n").arg(rendererJobId) + oneLineJsonMetric(summary) +
                                 "\n\n" + summary;
        self->updateDetailsText();
      }, Qt::QueuedConnection);
    };
    std::thread([this, renderer, text, width, callback, context, uiRequestId, generation]() {
      StemTeXErrorCode code = STEMTEX_OK;
      char *error = nullptr;
      uint64_t rendererJobId = 0;
      int submitted = 0;
      if (!shuttingDown_.load() && generation == rendererGeneration_.load()) {
        std::lock_guard<std::mutex> lock(rendererMutex_);
        if (renderer == renderer_) {
          submitted = stemtex_renderer_render_async(renderer, text.constData(), width, &rendererJobId, callback, context,
                                                    &code, &error);
        }
      }
      QString errorText = error ? QString::fromUtf8(error) : QString();
      stemtex_renderer_free_string(error);
      if (!submitted) {
        delete context;
        QMetaObject::invokeMethod(this, [this, uiRequestId, generation, code, errorText]() {
          if (shuttingDown_.load() || generation != rendererGeneration_.load() || uiRequestId != latestUiRequestId_) return;
          refreshEngineStatus(QString("failed to submit request, code %1").arg((int)code));
          details_->setPlainText(errorText);
        }, Qt::QueuedConnection);
      }
    }).detach();
  }

  void showCroppedPreview(const QString &pdfPath, int widthPt) {
    CroppedPreview cropped = renderCroppedPdfPreview(pdfPath, widthPt, dpiSpin_->value(), paddingSpin_->value());
    if (cropped.image.isNull()) {
      croppedPreview_->setText("PDF preview failed");
      return;
    }
    lastPreview_ = cropped.image;
    lastPreviewDisplaySize_ = cropped.displaySize;
    updatePreviewPixmap();
  }

  void rerenderLastPdfPreview() {
    if (lastPdf_.isEmpty()) {
      updatePreviewPixmap();
      return;
    }
    showCroppedPreview(lastPdf_, widthSpin_->value());
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
    QString outcomeText;
    if (lastOutcomeCode_ != STEMTEX_RENDER_OUTCOME_OK || lastIssueFlags_ != 0) {
      outcomeText = QString("outcome: %1, issue flags: %2, %3\n")
                        .arg((int)lastOutcomeCode_)
                        .arg(lastIssueFlags_)
                        .arg(lastOutcomeMessage_);
    }
    details_->setPlainText(QString("cropped preview: %1 x %2 px, displayed %3 x %4 px, %5 dpi, %6 pt padding\n")
                               .arg(lastPreview_.width())
                               .arg(lastPreview_.height())
                               .arg(lastPreviewDisplaySize_.width())
                               .arg(lastPreviewDisplaySize_.height())
                               .arg(dpiSpin_->value())
                               .arg(paddingSpin_->value(), 0, 'f', 1) +
                           outcomeText + lastSummaryText_);
  }

  void updatePreviewMinimumWidth(int widthPt) {
    constexpr double screenPixelsPerPoint = 96.0 / 72.0;
    croppedPreview_->setMinimumWidth(qMax(240, int(widthPt * screenPixelsPerPoint) + 48));
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
  QString lastPdf_;
  QsciScintilla *editor_ = nullptr;
  QSlider *widthSlider_ = nullptr;
  QSpinBox *widthSpin_ = nullptr;
  QSpinBox *dpiSpin_ = nullptr;
  QDoubleSpinBox *paddingSpin_ = nullptr;
  QLabel *texmfLabel_ = nullptr;
  QPushButton *texmfButton_ = nullptr;
  QComboBox *profileCombo_ = nullptr;
  QComboBox *encodingCombo_ = nullptr;
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
  QString observedEditorText_;
  int spareReady_ = 0;
  int spareTarget_ = 0;
  QString engineNote_;
  bool pendingStartupRender_ = false;
  uint64_t latestUiRequestId_ = 0;
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
