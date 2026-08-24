#include "stemtex_profile.h"

#include <QApplication>
#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QIcon>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QStandardItem>
#include <QStandardItemModel>
#include <QStandardPaths>
#include <QStatusBar>
#include <QStringList>
#include <QTextCursor>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>

#include <string>
#include <utility>

#include <windows.h>

namespace {

QString cleanAbsolutePath(const QString &path) {
  return QDir::cleanPath(QDir(path).absolutePath());
}

QString defaultRuntimeRoot() {
  QDir appDir(QCoreApplication::applicationDirPath());
  return cleanAbsolutePath(appDir.filePath("../runtime"));
}

QString defaultProfilesRoot() {
  QString data = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
  return cleanAbsolutePath(QDir(data).filePath("StemTeX/profiles"));
}

void configureSdkDllSearch(const QString &runtimeRoot) {
  QString appDir = QCoreApplication::applicationDirPath();
  QString sdkDir = QDir(runtimeRoot).filePath("bin/sdk");
  std::wstring appDirWide = QDir::toNativeSeparators(cleanAbsolutePath(appDir)).toStdWString();
  std::wstring sdkDirWide = QDir::toNativeSeparators(cleanAbsolutePath(sdkDir)).toStdWString();
  SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS | LOAD_LIBRARY_SEARCH_USER_DIRS);
  AddDllDirectory(appDirWide.c_str());
  AddDllDirectory(sdkDirWide.c_str());
}

QString argumentValue(const QStringList &args, const QString &name, const QString &fallback) {
  int index = args.indexOf(name);
  return index >= 0 && index + 1 < args.size() ? cleanAbsolutePath(args.at(index + 1)) : fallback;
}

QString profileErrorText(char *error) {
  QString text = error ? QString::fromUtf8(error) : QStringLiteral("Unknown Profile Creator error");
  stemtex_profile_free_string(error);
  return text;
}

class WaitCursor {
 public:
  WaitCursor() { QApplication::setOverrideCursor(Qt::WaitCursor); }
  ~WaitCursor() { QApplication::restoreOverrideCursor(); }
};

class CreatorWindow : public QMainWindow {
 public:
  CreatorWindow(QString texmfRoot, QString profilesRoot)
      : texmf_root_(std::move(texmfRoot)),
        profiles_root_(std::move(profilesRoot)) {
    setWindowTitle(QStringLiteral("StemTeX Profile Creator"));
    resize(820, 680);

    auto *central = new QWidget(this);
    auto *root = new QVBoxLayout(central);
    root->setContentsMargins(14, 14, 14, 14);
    root->setSpacing(12);

    auto *title = new QLabel(QStringLiteral("字体 Profile"), central);
    QFont titleFont = title->font();
    titleFont.setPointSize(titleFont.pointSize() + 5);
    titleFont.setBold(true);
    title->setFont(titleFont);
    root->addWidget(title);
    auto *intro = new QLabel(
        QStringLiteral("独立选择正文字体、数学字体和 CJK 字体。StemTeX 会隐藏具体的宏包、字体文件与加载顺序。"),
        central);
    intro->setWordWrap(true);
    root->addWidget(intro);

    auto *environmentBox = new QGroupBox(QStringLiteral("环境"), central);
    auto *environmentForm = new QFormLayout(environmentBox);
    auto *texmfRow = new QWidget(environmentBox);
    auto *texmfLayout = new QHBoxLayout(texmfRow);
    texmfLayout->setContentsMargins(0, 0, 0, 0);
    texmfEdit_ = new QLineEdit(texmf_root_, texmfRow);
    texmfEdit_->setReadOnly(true);
    auto *chooseTexmf = new QPushButton(QStringLiteral("选择 TeX Live..."), texmfRow);
    texmfLayout->addWidget(texmfEdit_, 1);
    texmfLayout->addWidget(chooseTexmf);
    environmentForm->addRow(QStringLiteral("字体树"), texmfRow);

    profilesEdit_ = new QLineEdit(profiles_root_, environmentBox);
    profilesEdit_->setReadOnly(true);
    profilesEdit_->setToolTip(QStringLiteral("Renderer GUI 会自动扫描这个用户 Profile 目录"));
    environmentForm->addRow(QStringLiteral("保存到"), profilesEdit_);
    root->addWidget(environmentBox);

    auto *fontBox = new QGroupBox(QStringLiteral("字体组合"), central);
    auto *fontForm = new QFormLayout(fontBox);
    nameEdit_ = new QLineEdit(fontBox);
    textCombo_ = new QComboBox(fontBox);
    mathCombo_ = new QComboBox(fontBox);
    cjkCombo_ = new QComboBox(fontBox);
    fontForm->addRow(QStringLiteral("Profile 名称"), nameEdit_);
    fontForm->addRow(QStringLiteral("正文字体"), textCombo_);
    fontForm->addRow(QStringLiteral("数学字体"), mathCombo_);
    fontForm->addRow(QStringLiteral("CJK 字体"), cjkCombo_);
    selectionInfo_ = new QLabel(fontBox);
    selectionInfo_->setWordWrap(true);
    selectionInfo_->setStyleSheet(QStringLiteral("QLabel { color: #555; padding-top: 4px; }"));
    fontForm->addRow(QString(), selectionInfo_);
    root->addWidget(fontBox);

    auto *preambleBox = new QGroupBox(QStringLiteral("生成的 preamble.tex"), central);
    auto *preambleLayout = new QVBoxLayout(preambleBox);
    preambleEdit_ = new QPlainTextEdit(preambleBox);
    preambleEdit_->setReadOnly(true);
    preambleEdit_->setLineWrapMode(QPlainTextEdit::NoWrap);
    preambleEdit_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    preambleEdit_->setPlaceholderText(QStringLiteral("选择有效字体后将在这里生成 preamble.tex"));
    preambleLayout->addWidget(preambleEdit_);
    root->addWidget(preambleBox, 1);

    auto *buttons = new QHBoxLayout();
    auto *openProfiles = new QPushButton(QStringLiteral("打开 Profile 目录"), central);
    createButton_ = new QPushButton(QStringLiteral("创建 Profile"), central);
    createButton_->setDefault(true);
    buttons->addWidget(openProfiles);
    buttons->addStretch(1);
    buttons->addWidget(createButton_);
    root->addLayout(buttons);
    setCentralWidget(central);

    connect(chooseTexmf, &QPushButton::clicked, this, [this]() { chooseTexmfRoot(); });
    connect(openProfiles, &QPushButton::clicked, this, [this]() {
      QDir().mkpath(profiles_root_);
      QDesktopServices::openUrl(QUrl::fromLocalFile(profiles_root_));
    });
    connect(nameEdit_, &QLineEdit::textEdited, this, [this](const QString &) { name_touched_ = true; });
    connect(nameEdit_, &QLineEdit::textChanged, this, [this](const QString &text) {
      createButton_->setEnabled(selectionsValid() && !text.trimmed().isEmpty());
    });
    for (QComboBox *combo : {textCombo_, mathCombo_, cjkCombo_}) {
      connect(combo, &QComboBox::currentIndexChanged, this, [this](int) { selectionChanged(); });
    }
    connect(createButton_, &QPushButton::clicked, this, [this]() { createProfile(); });

    reloadCatalog("latin-modern", "latin-modern-math", "simsun");
  }

 private:
  struct SpecStorage {
    QByteArray name;
    QByteArray text;
    QByteArray math;
    QByteArray cjk;
    StemTeXProfileSpec spec{};

    SpecStorage(const QString &profileName, const QString &textId,
                const QString &mathId, const QString &cjkId)
        : name(profileName.toUtf8()), text(textId.toUtf8()), math(mathId.toUtf8()), cjk(cjkId.toUtf8()) {
      spec.name_utf8 = name.constData();
      spec.text_font_id_utf8 = text.constData();
      spec.math_font_id_utf8 = math.constData();
      spec.cjk_font_id_utf8 = cjk.constData();
    }
  };

  QString selectedId(QComboBox *combo) const {
    return combo && combo->currentIndex() >= 0 ? combo->currentData().toString() : QString();
  }

  bool selectionsValid() const {
    return !selectedId(textCombo_).isEmpty() && !selectedId(mathCombo_).isEmpty() && !selectedId(cjkCombo_).isEmpty();
  }

  void chooseTexmfRoot() {
    QString selected = QFileDialog::getExistingDirectory(this, QStringLiteral("选择 TeX Live 根目录"), texmf_root_);
    if (selected.isEmpty()) return;
    QString normalized = cleanAbsolutePath(selected);
    if (normalized == texmf_root_) return;
    QString oldText = selectedId(textCombo_);
    QString oldMath = selectedId(mathCombo_);
    QString oldCjk = selectedId(cjkCombo_);
    texmf_root_ = normalized;
    texmfEdit_->setText(texmf_root_);
    reloadCatalog(oldText, oldMath, oldCjk);
  }

  void addCatalogItems(QComboBox *combo, const QJsonArray &fonts, const QString &role, const QString &wantedId) {
    combo->blockSignals(true);
    combo->clear();
    int wantedIndex = -1;
    int firstAvailable = -1;
    for (const QJsonValue &value : fonts) {
      QJsonObject font = value.toObject();
      if (font.value("role").toString() != role) continue;
      QString id = font.value("id").toString();
      QString source = font.value("source").toString();
      bool available = font.value("available").toBool();
      QString sourceLabel = source == "system" ? QStringLiteral("Windows")
                            : source == "texlive" ? QStringLiteral("TeX Live")
                            : QStringLiteral("关闭");
      QString label = QStringLiteral("%1  ·  %2").arg(font.value("displayName").toString(), sourceLabel);
      if (!available) label += QStringLiteral("  （当前树缺失）");
      combo->addItem(label, id);
      int index = combo->count() - 1;
      combo->setItemData(index, font.value("description").toString(), Qt::ToolTipRole);
      combo->setItemData(index, font, Qt::UserRole + 1);
      if (auto *model = qobject_cast<QStandardItemModel *>(combo->model())) {
        if (QStandardItem *item = model->item(index)) item->setEnabled(available);
      }
      if (available && firstAvailable < 0) firstAvailable = index;
      if (available && id == wantedId) wantedIndex = index;
    }
    combo->setCurrentIndex(wantedIndex >= 0 ? wantedIndex : firstAvailable);
    combo->blockSignals(false);
  }

  void reloadCatalog(const QString &wantedText, const QString &wantedMath, const QString &wantedCjk) {
    QByteArray texmf = texmf_root_.toUtf8();
    StemTeXProfileContext context{texmf.constData()};
    StemTeXProfileErrorCode code = STEMTEX_PROFILE_OK;
    char *error = nullptr;
    char *json = stemtex_profile_font_catalog_json(&context, &code, &error);
    if (!json) {
      QString message = profileErrorText(error);
      statusBar()->showMessage(message);
      QMessageBox::critical(this, QStringLiteral("无法读取字体目录"), message);
      for (QComboBox *combo : {textCombo_, mathCombo_, cjkCombo_}) combo->clear();
      selectionChanged();
      return;
    }
    QJsonParseError parseError{};
    QJsonDocument document = QJsonDocument::fromJson(QByteArray(json), &parseError);
    stemtex_profile_free_string(json);
    stemtex_profile_free_string(error);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
      QMessageBox::critical(this, QStringLiteral("字体目录错误"), parseError.errorString());
      return;
    }
    QJsonArray fonts = document.object().value("fonts").toArray();
    addCatalogItems(textCombo_, fonts, "text", wantedText);
    addCatalogItems(mathCombo_, fonts, "math", wantedMath);
    addCatalogItems(cjkCombo_, fonts, "cjk", wantedCjk);
    statusBar()->showMessage(QStringLiteral("已读取当前 TeX Live 字体目录"), 4000);
    selectionChanged();
  }

  QString suggestedName() const {
    QStringList ids{selectedId(textCombo_), selectedId(mathCombo_), selectedId(cjkCombo_)};
    ids.removeAll("none");
    QString base = ids.join('-');
    if (base.isEmpty()) base = QStringLiteral("font-profile");
    QString candidate = base;
    int suffix = 2;
    while (QFileInfo::exists(QDir(profiles_root_).filePath(candidate))) {
      candidate = QStringLiteral("%1-%2").arg(base).arg(suffix++);
    }
    return candidate;
  }

  void selectionChanged() {
    if (!name_touched_) nameEdit_->setText(suggestedName());
    QStringList descriptions;
    for (QComboBox *combo : {textCombo_, mathCombo_, cjkCombo_}) {
      if (combo->currentIndex() >= 0) descriptions << combo->currentData(Qt::ToolTipRole).toString();
    }
    selectionInfo_->setText(descriptions.join(QStringLiteral(" · ")));
    bool valid = selectionsValid();
    createButton_->setEnabled(valid && !nameEdit_->text().trimmed().isEmpty());
    updatePreamble();
  }

  SpecStorage currentSpec(const QString &nameOverride = QString()) const {
    QString name = nameOverride.isEmpty() ? nameEdit_->text().trimmed() : nameOverride;
    return SpecStorage(name, selectedId(textCombo_), selectedId(mathCombo_), selectedId(cjkCombo_));
  }

  bool generateTemporaryProfile(const QString &root, const SpecStorage &storage, QString *profilePath, QString *errorText) {
    QByteArray texmf = texmf_root_.toUtf8();
    QByteArray profiles = root.toUtf8();
    StemTeXProfileContext context{texmf.constData()};
    StemTeXProfileErrorCode code = STEMTEX_PROFILE_OK;
    char *result = nullptr;
    char *error = nullptr;
    int ok = stemtex_profile_materialize(&context, &storage.spec, profiles.constData(), &result, &code, &error);
    if (!ok) {
      if (errorText) *errorText = profileErrorText(error);
      stemtex_profile_free_string(result);
      return false;
    }
    QJsonDocument resultDoc = QJsonDocument::fromJson(QByteArray(result ? result : "{}"));
    QString path = resultDoc.object().value("profilePath").toString();
    stemtex_profile_free_string(result);
    stemtex_profile_free_string(error);
    if (path.isEmpty()) path = QDir(root).filePath(QString::fromUtf8(storage.name));
    if (profilePath) *profilePath = path;
    return true;
  }

  void updatePreamble() {
    if (!selectionsValid()) {
      preambleEdit_->clear();
      return;
    }
    QByteArray texmf = texmf_root_.toUtf8();
    StemTeXProfileContext context{texmf.constData()};
    SpecStorage storage = currentSpec(QStringLiteral("preamble-preview"));
    StemTeXProfileErrorCode code = STEMTEX_PROFILE_OK;
    char *error = nullptr;
    char *preamble = stemtex_profile_preamble_utf8(&context, &storage.spec, &code, &error);
    if (!preamble) {
      QString message = profileErrorText(error);
      preambleEdit_->setPlainText(QStringLiteral("% 无法生成 preamble.tex\n% %1").arg(message));
      statusBar()->showMessage(message);
      return;
    }
    preambleEdit_->setPlainText(QString::fromUtf8(preamble));
    stemtex_profile_free_string(preamble);
    stemtex_profile_free_string(error);
    preambleEdit_->moveCursor(QTextCursor::Start);
    statusBar()->showMessage(QStringLiteral("preamble.tex 已按当前字体组合更新"), 3000);
  }

  void createProfile() {
    if (!selectionsValid()) return;
    QString name = nameEdit_->text().trimmed();
    if (name.isEmpty()) return;
    WaitCursor wait;
    QDir().mkpath(profiles_root_);
    SpecStorage storage = currentSpec();
    QString path;
    QString error;
    if (!generateTemporaryProfile(profiles_root_, storage, &path, &error)) {
      QMessageBox::critical(this, QStringLiteral("创建失败"), error);
      statusBar()->showMessage(error);
      return;
    }
    QMessageBox::information(
        this, QStringLiteral("Profile 已创建"),
        QStringLiteral("已创建：\n%1\n\nRenderer GUI 将把它作为普通 Profile 使用。").arg(path));
    QApplication::exit(0);
  }

  QString texmf_root_;
  QString profiles_root_;
  QLineEdit *texmfEdit_ = nullptr;
  QLineEdit *profilesEdit_ = nullptr;
  QLineEdit *nameEdit_ = nullptr;
  QComboBox *textCombo_ = nullptr;
  QComboBox *mathCombo_ = nullptr;
  QComboBox *cjkCombo_ = nullptr;
  QLabel *selectionInfo_ = nullptr;
  QPlainTextEdit *preambleEdit_ = nullptr;
  QPushButton *createButton_ = nullptr;
  bool name_touched_ = false;
};

}  // namespace

int main(int argc, char **argv) {
  qputenv("QT_QPA_PLATFORM", "windows");
  QApplication app(argc, argv);
  QCoreApplication::setApplicationName("StemTeX");
  QApplication::setWindowIcon(QIcon(":/icons/stemtex-renderer-gui.png"));
  QStringList args = app.arguments();
  bool smoke = args.contains("--smoke");
  QString runtimeRoot = argumentValue(args, "--runtime", defaultRuntimeRoot());
  QString texmfRoot = argumentValue(args, "--texmf", runtimeRoot);
  QString profilesRoot = argumentValue(args, "--profiles", defaultProfilesRoot());
  configureSdkDllSearch(runtimeRoot);
  CreatorWindow window(texmfRoot, profilesRoot);
  if (smoke) return 0;
  window.show();
  return app.exec();
}
