#include "stemtex_renderer.h"

#include <QApplication>
#include <QByteArray>
#include <QClipboard>
#include <QColor>
#include <QDesktopServices>
#include <QDir>
#include <QElapsedTimer>
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
#include <QProcessEnvironment>
#include <QPushButton>
#include <QComboBox>
#include <QSlider>
#include <QSpinBox>
#include <QSplitter>
#include <QStatusBar>
#include <QString>
#include <QStringList>
#include <QTextBrowser>
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

QString defaultRepoRoot() {
  QDir dir(QCoreApplication::applicationDirPath());
  for (int i = 0; i < 8; ++i) {
    if (QFileInfo::exists(dir.filePath("cpp-daemon/worker-template.tex")) && QFileInfo::exists(dir.filePath("gui/profiles"))) {
      return dir.absolutePath();
    }
    if (!dir.cdUp()) break;
  }
  return QDir::currentPath();
}

QString defaultRuntimeRoot() {
  QString env = QProcessEnvironment::systemEnvironment().value("STEMTEX_RUNTIME");
  if (!env.isEmpty()) return QDir::cleanPath(QDir(env).absolutePath());
  QDir appDir(QCoreApplication::applicationDirPath());
  QString installedRuntime = appDir.filePath("../runtime");
  if (QFileInfo::exists(QDir(installedRuntime).filePath("bin/windows/xetexdaemon.exe"))) {
    return QDir::cleanPath(installedRuntime);
  }
  QString portableRuntime = appDir.filePath("runtime");
  if (QFileInfo::exists(QDir(portableRuntime).filePath("bin/windows/xetexdaemon.exe"))) {
    return QDir::cleanPath(portableRuntime);
  }
  return QString();
}

QString normalizeRuntimeRoot(const QString &path) {
  QDir dir(QDir::cleanPath(QDir(path).absolutePath()));
  QString nestedRuntime = dir.filePath("runtime");
  if (QFileInfo::exists(QDir(nestedRuntime).filePath("bin/windows/xetexdaemon.exe"))) {
    return QDir::cleanPath(nestedRuntime);
  }
  return QDir::cleanPath(dir.absolutePath());
}

QString normalizeTexmfRoot(const QString &path) {
  return QDir::cleanPath(QDir(path).absolutePath());
}

QString defaultTexmfRoot(const QString &runtimeRoot) {
  QString env = QProcessEnvironment::systemEnvironment().value("STEMTEX_TEXMF_ROOT");
  if (!env.isEmpty()) return normalizeTexmfRoot(env);
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

QVector<ProfileEntry> profileRoots(const QString &repoRoot, QString *errorText) {
  QVector<ProfileEntry> profiles;
  QStringList seen;
  QDir appDir(QCoreApplication::applicationDirPath());
  for (const QString &base : {appDir.filePath("profiles"), QDir(repoRoot).filePath("gui/profiles")}) {
    QDir dir(base);
    if (!dir.exists()) continue;
    for (const QFileInfo &candidate : dir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
      ProfileEntry profile;
      if (!rendererProfileInfo(candidate.absoluteFilePath(), &profile, errorText)) continue;
      if (seen.contains(profile.path)) continue;
      seen << profile.path;
      profiles.push_back(profile);
    }
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

QImage renderCroppedPdfPreview(const QString &pdfPath, int minWidthPt) {
  QPdfDocument source;
  QPdfDocument::Error error = source.load(pdfPath);
  if (error != QPdfDocument::Error::None || source.pageCount() <= 0) return QImage();

  constexpr double pixelsPerPoint = 3.0;
  QSizeF points = source.pagePointSize(0);
  QSize imageSize(qMax(1, int(points.width() * pixelsPerPoint)), qMax(1, int(points.height() * pixelsPerPoint)));
  QImage page = source.render(0, imageSize);
  if (page.isNull()) return QImage();

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
  if (bounds.isNull()) return rgba;

  int pad = 24;
  bounds = expandRectRightToWidth(bounds, qMax(1, int(minWidthPt * pixelsPerPoint)), rgba.rect());
  bounds = bounds.adjusted(-pad, -pad, pad, pad).intersected(rgba.rect());
  QImage cropped = rgba.copy(bounds);
  QImage white(cropped.size(), QImage::Format_RGB32);
  white.fill(Qt::white);
  QPainter painter(&white);
  painter.drawImage(0, 0, cropped);
  painter.end();
  return white;
}

int runSmoke(const QString &repoRoot, const QString &runtimeRoot, const QString &profileRoot, const QString &texmfRoot) {
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
  StemTeXConfig cfg{};
  cfg.repo_root_utf8 = repo.constData();
  cfg.runtime_root_utf8 = runtime.constData();
  cfg.texmf_root_utf8 = texmf.constData();
  cfg.profile_root_utf8 = profile.constData();
  cfg.request_timeout_ms = 90000;
  cfg.xdvipdfmx_timeout_ms = 90000;
  cfg.spare_worker_count = 2;

  StemTeXErrorCode code = STEMTEX_OK;
  char *error = nullptr;
  StemTeXRenderer *renderer = stemtex_renderer_create(&cfg, &code, &error);
  if (!renderer) {
    fprintf(stderr, "create failed code=%d: %s\n", (int)code, error ? error : "");
    stemtex_renderer_free_string(error);
    return 1;
  }

  StemTeXRenderResult result{};
  QByteArray snippet = defaultSnippet().toUtf8();
  int ok = stemtex_renderer_render(renderer, snippet.constData(), 360, &result, &code, &error);
  if (!ok) {
    fprintf(stderr, "render failed code=%d: %s\n", (int)code, error ? error : "");
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
  QImage cropped = renderCroppedPdfPreview(pdfPath, 360);
  printf("pdf=%s\nsummary=%s\nqtPdfError=%d pages=%d pagePoints=%.2fx%.2f croppedPixels=%dx%d\n",
         pdfPath.toUtf8().constData(), summary.toUtf8().constData(),
         (int)pdfError, pages, pageSize.width(), pageSize.height(), cropped.width(), cropped.height());
  if (!summary.contains("\"xdvipdfmxMode\":\"daemon-dll\"") && !summary.contains("\"xdvipdfmxMode\":\"dll\"")) {
    fprintf(stderr, "expected xdvipdfmxMode=daemon-dll, got summary=%s\n", summary.toUtf8().constData());
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

    auto *toolbar = new QHBoxLayout();
    toolbar->setSpacing(8);
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
    toolbar->addWidget(widthLabel);
    toolbar->addWidget(widthSlider_, 1);
    toolbar->addWidget(widthSpin_);
    toolbar->addWidget(new QLabel("TeXLive", central));
    toolbar->addWidget(texmfLabel_);
    toolbar->addWidget(texmfButton_);
    toolbar->addWidget(new QLabel("Profile", central));
    toolbar->addWidget(profileCombo_);
    toolbar->addWidget(new QLabel("输入编码", central));
    toolbar->addWidget(encodingCombo_);
    toolbar->addWidget(renderButton_);
    toolbar->addWidget(copyImageButton_);
    toolbar->addWidget(saveImageButton_);
    toolbar->addWidget(openButton_);
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

    auto *previewShell = new QWidget(splitter);
    auto *previewLayout = new QVBoxLayout(previewShell);
    previewLayout->setContentsMargins(0, 0, 0, 0);
    previewLayout->setSpacing(6);
    croppedPreview_ = new QLabel(previewShell);
    croppedPreview_->setAlignment(Qt::AlignHCenter | Qt::AlignTop);
    croppedPreview_->setMinimumSize(240, 240);
    croppedPreview_->setStyleSheet("QLabel { background: #808080; border: 1px solid #606060; padding: 12px; }");
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

    connect(widthSlider_, &QSlider::valueChanged, widthSpin_, &QSpinBox::setValue);
    connect(widthSpin_, &QSpinBox::valueChanged, widthSlider_, &QSlider::setValue);
    connect(widthSpin_, &QSpinBox::valueChanged, this, [this](int value) {
      updatePreviewMinimumWidth(value);
      updatePreviewPixmap();
    });
    connect(texmfButton_, &QPushButton::clicked, this, [this]() { chooseTexmfRoot(); });
    connect(profileCombo_, &QComboBox::currentIndexChanged, this, [this](int) { switchProfile(); });
    connect(renderButton_, &QPushButton::clicked, this, [this]() { renderSnippet(); });
    connect(copyImageButton_, &QPushButton::clicked, this, [this]() { copyPreviewImage(); });
    connect(saveImageButton_, &QPushButton::clicked, this, [this]() { savePreviewImage(); });
    connect(openButton_, &QPushButton::clicked, this, [this]() {
      if (!lastPdf_.isEmpty()) QDesktopServices::openUrl(QUrl::fromLocalFile(lastPdf_));
    });

    updateEngineStatus(false, spareReady_, spareTarget_);
    setUiReady(true);
    updatePreviewMinimumWidth(widthSpin_->value());
    initializeRenderer();
  }

  ~MainWindow() override {
    shuttingDown_.store(true);
    if (renderer_) stemtex_renderer_destroy(renderer_);
  }

 private:
  void setUiReady(bool ready) {
    bool hasProfile = profileCombo_ && profileCombo_->currentIndex() >= 0;
    renderButton_->setEnabled(ready && hasProfile);
    if (profileCombo_) profileCombo_->setEnabled(ready);
  }

  void setPreviewImageReady(bool ready) {
    copyImageButton_->setEnabled(ready);
    saveImageButton_->setEnabled(ready);
  }

  QString lightHtml(bool ok) const {
    return QString("<span style=\"color:%1;font-size:14px;\">&#9679;</span>").arg(ok ? "#179c48" : "#c62828");
  }

  void updateEngineStatus(bool primaryOk, int spareReady, int spareTarget, const QString &note = QString()) {
    spareReady_ = qMax(0, spareReady);
    spareTarget_ = qMax(0, spareTarget);
    QString engineText = primaryOk ? "primary ready" : "primary unavailable";
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

  void stopRenderer() {
    ++rendererGeneration_;
    enginePollTimer_->stop();
    if (renderer_) {
      stemtex_renderer_destroy(renderer_);
      renderer_ = nullptr;
    }
    pendingStartupRender_ = false;
  }

  void chooseTexmfRoot() {
    QString selected = QFileDialog::getExistingDirectory(this, "Select TeXLive package/font tree", texmf_root_);
    if (selected.isEmpty()) return;
    QString normalized = normalizeTexmfRoot(selected);
    if (normalized == texmf_root_) return;
    stopRenderer();
    texmf_root_ = normalized;
    reloadProfiles();
    setUiReady(true);
    updateEngineStatus(false, 0, spareTarget_, QString("texmf: %1").arg(texmf_root_));
    initializeRenderer();
  }

  void switchProfile() {
    if (!renderer_) {
      initializeRenderer();
      return;
    }
    stopRenderer();
    refreshEngineStatus(QString("profile: %1").arg(profileCombo_->currentText()));
    initializeRenderer();
  }

  void initializeRenderer() {
    QString profileRoot = selectedProfileRoot();
    if (profileRoot.isEmpty()) {
      refreshEngineStatus("choose a profile");
      setUiReady(true);
      return;
    }
    uint64_t generation = ++rendererGeneration_;
    std::thread([this, profileRoot, generation]() {
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
      cfg.spare_worker_count = 2;
      StemTeXErrorCode code = STEMTEX_OK;
      char *error = nullptr;
      StemTeXRenderer *renderer = stemtex_renderer_create(&cfg, &code, &error);
      auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
      QString errorText = error ? QString::fromUtf8(error) : QString();
      stemtex_renderer_free_string(error);
      QMetaObject::invokeMethod(this, [this, renderer, code, errorText, elapsed, generation]() {
        if (shuttingDown_.load() || generation != rendererGeneration_.load()) {
          if (renderer) stemtex_renderer_destroy(renderer);
          return;
        }
        renderer_ = renderer;
        if (!renderer_) {
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
        }
      }, Qt::QueuedConnection);
    }).detach();
  }

  void renderSnippet() {
    if (!renderer_) {
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
    };
    auto *context = new CallbackContext{this, uiRequestId};
    auto callback = [](uint64_t rendererJobId, int ok, const StemTeXRenderResult *result, StemTeXErrorCode code,
                       const char *error, void *userData) {
      std::unique_ptr<CallbackContext> context(static_cast<CallbackContext *>(userData));
      MainWindow *self = context->self;
      uint64_t uiRequestId = context->uiRequestId;
      QString pdfPath = result && result->pdf_path_utf8 ? QString::fromUtf8(result->pdf_path_utf8) : QString();
      QString summary = result && result->summary_json_utf8 ? QString::fromUtf8(result->summary_json_utf8) : QString();
      QString errorText = error ? QString::fromUtf8(error) : QString();
      QMetaObject::invokeMethod(self, [self, uiRequestId, rendererJobId, ok, code, pdfPath, summary, errorText]() {
        if (self->shuttingDown_.load() || uiRequestId != self->latestUiRequestId_) return;
        if (!ok) {
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
        self->details_->setPlainText(
            QString("cropped preview: %1 x %2 px\n").arg(self->lastPreview_.width()).arg(self->lastPreview_.height()) +
            QString("renderer job: %1\n").arg(rendererJobId) + oneLineJsonMetric(summary) + "\n\n" + summary);
      }, Qt::QueuedConnection);
    };
    std::thread([this, text, width, callback, context, uiRequestId]() {
      StemTeXErrorCode code = STEMTEX_OK;
      char *error = nullptr;
      uint64_t rendererJobId = 0;
      int submitted = 0;
      if (!shuttingDown_.load()) {
        submitted = stemtex_renderer_render_async(renderer_, text.constData(), width, &rendererJobId, callback, context,
                                                  &code, &error);
      }
      QString errorText = error ? QString::fromUtf8(error) : QString();
      stemtex_renderer_free_string(error);
      if (!submitted) {
        delete context;
        QMetaObject::invokeMethod(this, [this, uiRequestId, code, errorText]() {
          if (shuttingDown_.load() || uiRequestId != latestUiRequestId_) return;
          refreshEngineStatus(QString("failed to submit request, code %1").arg((int)code));
          details_->setPlainText(errorText);
        }, Qt::QueuedConnection);
      }
    }).detach();
  }

  void showCroppedPreview(const QString &pdfPath, int widthPt) {
    QImage cropped = renderCroppedPdfPreview(pdfPath, widthPt);
    if (cropped.isNull()) {
      croppedPreview_->setText("PDF preview failed");
      return;
    }
    lastPreview_ = cropped;
    updatePreviewPixmap();
  }

  void copyPreviewImage() {
    if (lastPreview_.isNull()) return;
    QApplication::clipboard()->setImage(lastPreview_);
    updateEngineStatus(renderer_ != nullptr, spareReady_, spareTarget_);
  }

  void savePreviewImage() {
    if (lastPreview_.isNull()) return;
    QString path = QFileDialog::getSaveFileName(this, "Save image", QDir::home().filePath("stemtex-snippet.png"),
                                                "PNG image (*.png);;JPEG image (*.jpg *.jpeg);;BMP image (*.bmp)");
    if (path.isEmpty()) return;
    if (!lastPreview_.save(path)) {
      updateEngineStatus(renderer_ != nullptr, spareReady_, spareTarget_, "save failed");
      return;
    }
    updateEngineStatus(renderer_ != nullptr, spareReady_, spareTarget_);
  }

  void updatePreviewPixmap() {
    if (lastPreview_.isNull()) return;
    QSize target(qMax(1, croppedPreview_->width() - 24), qMax(1, croppedPreview_->height() - 24));
    croppedPreview_->setPixmap(QPixmap::fromImage(lastPreview_).scaled(target, Qt::KeepAspectRatio,
                                                                       Qt::SmoothTransformation));
  }

  void updatePreviewMinimumWidth(int widthPt) {
    constexpr double screenPixelsPerPoint = 96.0 / 72.0;
    croppedPreview_->setMinimumWidth(qMax(240, int(widthPt * screenPixelsPerPoint) + 48));
  }

  void resizeEvent(QResizeEvent *event) override {
    QMainWindow::resizeEvent(event);
    updatePreviewPixmap();
  }

  QString repo_root_;
  QString runtime_root_;
  QString texmf_root_;
  StemTeXRenderer *renderer_ = nullptr;
  std::atomic<bool> shuttingDown_{false};
  std::atomic<uint64_t> rendererGeneration_{0};
  QString lastPdf_;
  QsciScintilla *editor_ = nullptr;
  QSlider *widthSlider_ = nullptr;
  QSpinBox *widthSpin_ = nullptr;
  QLabel *texmfLabel_ = nullptr;
  QPushButton *texmfButton_ = nullptr;
  QComboBox *profileCombo_ = nullptr;
  QComboBox *encodingCombo_ = nullptr;
  QPushButton *renderButton_ = nullptr;
  QPushButton *copyImageButton_ = nullptr;
  QPushButton *saveImageButton_ = nullptr;
  QPushButton *openButton_ = nullptr;
  QLabel *croppedPreview_ = nullptr;
  QImage lastPreview_;
  QTextBrowser *details_ = nullptr;
  QLabel *engineStatusLabel_ = nullptr;
  QTimer *enginePollTimer_ = nullptr;
  int spareReady_ = 0;
  int spareTarget_ = 2;
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
  QString repoRoot = args.size() > 1 ? args.at(1) : defaultRepoRoot();
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
