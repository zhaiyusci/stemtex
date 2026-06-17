#include "stemtex_renderer.h"

#include <QApplication>
#include <QByteArray>
#include <QClipboard>
#include <QDesktopServices>
#include <QDir>
#include <QElapsedTimer>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMainWindow>
#include <QMetaObject>
#include <QPdfDocument>
#include <QImage>
#include <QPainter>
#include <QPixmap>
#include <QPlainTextEdit>
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
#include <QWidget>

#include <atomic>
#include <chrono>
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
    if (QFileInfo::exists(dir.filePath("webapp/worker-webapp.tex")) && QFileInfo::exists(dir.filePath("test/preamble.tex"))) {
      return dir.absolutePath();
    }
    if (!dir.cdUp()) break;
  }
  return QDir::currentPath();
}

QString defaultRuntimeRoot() {
  QString env = QProcessEnvironment::systemEnvironment().value("STEMTEX_RUNTIME");
  if (!env.isEmpty()) return env;
  QDir repo(defaultRepoRoot());
  QString sideTree = repo.filePath("dist/stemtex-texlive-daemon");
  if (QFileInfo::exists(QDir(sideTree).filePath("bin/windows/xetexdaemon.exe")) &&
      QFileInfo::exists(QDir(sideTree).filePath("texmf-var/cache-warmup/warmup.xdv"))) {
    return QDir::cleanPath(sideTree);
  }
  return "C:/StemTeX";
}

QString defaultSnippet() {
  return QString::fromUtf8(
      "这是一段 Qt demo 里的中文、数学和化学预览：$E=mc^2$，以及 \\textcolor{blue}{蓝色文字}。\n\n"
      "\\[\n"
      "  \\int_0^1 x^2\\,dx = \\frac{1}{3},\\quad \\ip{\\psi}{\\phi}\n"
      "\\]\n\n"
      "\\ce{2H2 + O2 -> 2H2O}\n");
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

int jsonIntValue(const QString &summaryJson, const QString &key, int fallback) {
  QJsonParseError err{};
  QJsonDocument doc = QJsonDocument::fromJson(summaryJson.toUtf8(), &err);
  if (err.error != QJsonParseError::NoError || !doc.isObject()) return fallback;
  QJsonValue value = doc.object().value(key);
  return value.isDouble() ? value.toInt() : fallback;
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

int runSmoke(const QString &repoRoot, const QString &runtimeRoot) {
  QByteArray repo = QDir::cleanPath(repoRoot).toUtf8();
  QByteArray runtime = QDir::cleanPath(runtimeRoot).toUtf8();
  QDir runtimeDir(QString::fromUtf8(runtime));
  printf("repoRoot=%s\nruntimeRoot=%s\nruntimeHasXetexdaemon=%d runtimeHasDvipdfmxDll=%d runtimeHasWarmup=%d\n",
         repo.constData(), runtime.constData(),
         QFileInfo::exists(runtimeDir.filePath("bin/windows/xetexdaemon.exe")) ? 1 : 0,
         QFileInfo::exists(runtimeDir.filePath("bin/windows/dvipdfmx.dll")) ? 1 : 0,
         QFileInfo::exists(runtimeDir.filePath("texmf-var/cache-warmup/warmup.xdv")) ? 1 : 0);
  StemTeXConfig cfg{};
  cfg.repo_root_utf8 = repo.constData();
  cfg.runtime_root_utf8 = runtime.constData();
  cfg.request_timeout_ms = 90000;
  cfg.xdvipdfmx_timeout_ms = 90000;
  cfg.spare_worker_count = 1;

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
  if (!summary.contains("\"xdvipdfmxMode\":\"dll\"")) {
    fprintf(stderr, "expected xdvipdfmxMode=dll, got summary=%s\n", summary.toUtf8().constData());
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
  MainWindow(QString repoRoot, QString runtimeRoot) : repo_root_(std::move(repoRoot)), runtime_root_(std::move(runtimeRoot)) {
    setWindowTitle("StemTeX Qt Preview");
    resize(1180, 760);

    auto *central = new QWidget(this);
    auto *rootLayout = new QVBoxLayout(central);
    rootLayout->setContentsMargins(10, 10, 10, 10);
    rootLayout->setSpacing(8);

    auto *toolbar = new QHBoxLayout();
    toolbar->setSpacing(8);
    auto *widthLabel = new QLabel("版心宽度", central);
    widthSlider_ = new QSlider(Qt::Horizontal, central);
    widthSlider_->setRange(180, 430);
    widthSlider_->setSingleStep(10);
    widthSlider_->setPageStep(20);
    widthSlider_->setValue(360);
    widthSpin_ = new QSpinBox(central);
    widthSpin_->setRange(180, 430);
    widthSpin_->setSingleStep(10);
    widthSpin_->setSuffix(" pt");
    widthSpin_->setValue(360);
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
    toolbar->addWidget(new QLabel("输入编码", central));
    toolbar->addWidget(encodingCombo_);
    toolbar->addWidget(renderButton_);
    toolbar->addWidget(copyImageButton_);
    toolbar->addWidget(saveImageButton_);
    toolbar->addWidget(openButton_);
    rootLayout->addLayout(toolbar);

    auto *splitter = new QSplitter(Qt::Horizontal, central);
    editor_ = new QPlainTextEdit(splitter);
    editor_->setPlainText(defaultSnippet());
    editor_->setLineWrapMode(QPlainTextEdit::WidgetWidth);

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
    connect(renderButton_, &QPushButton::clicked, this, [this]() { renderSnippet(); });
    connect(copyImageButton_, &QPushButton::clicked, this, [this]() { copyPreviewImage(); });
    connect(saveImageButton_, &QPushButton::clicked, this, [this]() { savePreviewImage(); });
    connect(openButton_, &QPushButton::clicked, this, [this]() {
      if (!lastPdf_.isEmpty()) QDesktopServices::openUrl(QUrl::fromLocalFile(lastPdf_));
    });

    updateEngineStatus(false, spareReady_, spareTarget_);
    setUiReady(false);
    updatePreviewMinimumWidth(widthSpin_->value());
    initializeRenderer();
  }

  ~MainWindow() override {
    shuttingDown_.store(true);
    if (renderer_) stemtex_renderer_destroy(renderer_);
  }

 private:
  void setUiReady(bool ready) {
    renderButton_->setEnabled(ready);
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
    QString text = lightHtml(primaryOk);
    for (int i = 0; i < spareTarget_; ++i) {
      text += lightHtml(i < spareReady_);
    }
    QString shownNote = note.isEmpty() ? engineNote_ : note;
    if (!shownNote.isEmpty()) {
      engineNote_ = shownNote;
      text += "<span style=\"color:transparent;font-size:14px;\">&#9679;</span>";
      text += shownNote.toHtmlEscaped();
    }
    engineStatusLabel_->setText(text);
  }

  void refreshEngineStatus() {
    if (!renderer_) return;
    StemTeXEngineSnapshot snapshot{};
    if (!stemtex_renderer_engine_snapshot(renderer_, &snapshot)) return;
    updateEngineStatus(snapshot.primary_ready != 0, snapshot.spare_ready, snapshot.spare_target);
  }

  void initializeRenderer() {
    std::thread([this]() {
      auto start = std::chrono::steady_clock::now();
      QByteArray repo = QDir::cleanPath(repo_root_).toUtf8();
      QByteArray runtime = QDir::cleanPath(runtime_root_).toUtf8();
      StemTeXConfig cfg{};
      cfg.repo_root_utf8 = repo.constData();
      cfg.runtime_root_utf8 = runtime.constData();
      cfg.request_timeout_ms = 90000;
      cfg.xdvipdfmx_timeout_ms = 90000;
      cfg.spare_worker_count = 1;
      StemTeXErrorCode code = STEMTEX_OK;
      char *error = nullptr;
      StemTeXRenderer *renderer = stemtex_renderer_create(&cfg, &code, &error);
      auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
      QString errorText = error ? QString::fromUtf8(error) : QString();
      stemtex_renderer_free_string(error);
      QMetaObject::invokeMethod(this, [this, renderer, code, errorText, elapsed]() {
        if (shuttingDown_.load()) {
          if (renderer) stemtex_renderer_destroy(renderer);
          return;
        }
        renderer_ = renderer;
        if (!renderer_) {
          setUiReady(false);
          updateEngineStatus(false, 0, spareTarget_, QString("init failed code=%1").arg((int)code));
          details_->setPlainText(errorText);
          return;
        }
        setUiReady(true);
        updateEngineStatus(true, 0, spareTarget_, QString("init %1 ms").arg(elapsed));
        enginePollTimer_->start();
      }, Qt::QueuedConnection);
    }).detach();
  }

  void renderSnippet() {
    if (!renderer_) return;
    QString snippet = editor_->toPlainText();
    QString encoding = encodingCombo_->currentText();
    int width = widthSpin_->value();
    setUiReady(false);
    updateEngineStatus(true, spareReady_, spareTarget_);
    setPreviewImageReady(false);
    details_->clear();
    auto start = std::chrono::steady_clock::now();
    std::thread([this, snippet, encoding, width, start]() {
      QByteArray text = encodeSnippetForTeX(snippet, encoding);
      StemTeXRenderResult result{};
      StemTeXErrorCode code = STEMTEX_OK;
      char *error = nullptr;
      int ok = stemtex_renderer_render(renderer_, text.constData(), width, &result, &code, &error);
      QString pdfPath = result.pdf_path_utf8 ? QString::fromUtf8(result.pdf_path_utf8) : QString();
      QString summary = result.summary_json_utf8 ? QString::fromUtf8(result.summary_json_utf8) : QString();
      QString errorText = error ? QString::fromUtf8(error) : QString();
      stemtex_renderer_free_string(error);
      stemtex_renderer_free_result(&result);
      auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
      QMetaObject::invokeMethod(this, [this, ok, code, pdfPath, summary, errorText, elapsed]() {
        if (shuttingDown_.load()) return;
        setUiReady(true);
        if (!ok) {
          engineNote_ = code == STEMTEX_ERROR_WORKER_RESTARTING
                            ? QString("recovering")
                            : QString("render failed code=%1").arg((int)code);
          refreshEngineStatus();
          details_->setPlainText(errorText);
          return;
        }
        int spareReady = jsonIntValue(summary, "spareReady", spareReady_);
        int spareTarget = jsonIntValue(summary, "spareTarget", spareTarget_);
        updateEngineStatus(true, spareReady, spareTarget);
        lastPdf_ = pdfPath;
        openButton_->setEnabled(true);
        showCroppedPreview(pdfPath, widthSpin_->value());
        setPreviewImageReady(!lastPreview_.isNull());
        details_->setPlainText(QString("cropped preview: %1 x %2 px\n").arg(lastPreview_.width()).arg(lastPreview_.height()) +
                               oneLineJsonMetric(summary) + "\n\n" + summary);
      }, Qt::QueuedConnection);
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
  StemTeXRenderer *renderer_ = nullptr;
  std::atomic<bool> shuttingDown_{false};
  QString lastPdf_;
  QPlainTextEdit *editor_ = nullptr;
  QSlider *widthSlider_ = nullptr;
  QSpinBox *widthSpin_ = nullptr;
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
  int spareTarget_ = 1;
  QString engineNote_;
};

int main(int argc, char **argv) {
  qputenv("QT_QPA_PLATFORM", "windows");

  QApplication app(argc, argv);
  QStringList args = app.arguments();
  bool smoke = args.contains("--smoke");
  args.removeAll("--smoke");
  QString repoRoot = args.size() > 1 ? args.at(1) : defaultRepoRoot();
  QString runtimeRoot = args.size() > 2 ? args.at(2) : defaultRuntimeRoot();
  if (smoke) return runSmoke(repoRoot, runtimeRoot);
  MainWindow w(repoRoot, runtimeRoot);
  w.show();
  return app.exec();
}
