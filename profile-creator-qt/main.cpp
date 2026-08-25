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
#include <QHash>
#include <QHeaderView>
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
#include <QSet>
#include <QStandardItem>
#include <QStandardItemModel>
#include <QStandardPaths>
#include <QStatusBar>
#include <QStringList>
#include <QTabWidget>
#include <QTextCursor>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>

#include <string>
#include <utility>
#include <vector>

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
    resize(980, 720);

    auto *central = new QWidget(this);
    auto *root = new QVBoxLayout(central);
    root->setContentsMargins(14, 14, 14, 14);
    root->setSpacing(12);

    auto *title = new QLabel(QStringLiteral("StemTeX Profile Creator"), central);
    QFont titleFont = title->font();
    titleFont.setPointSize(titleFont.pointSize() + 5);
    titleFont.setBold(true);
    title->setFont(titleFont);
    root->addWidget(title);
    auto *intro = new QLabel(
        QStringLiteral("选择字体和受支持的常用宏包。StemTeX 负责解析依赖，并生成顺序确定的 preamble.tex。"),
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
    environmentForm->addRow(QStringLiteral("TeX Live 树"), texmfRow);

    profilesEdit_ = new QLineEdit(profiles_root_, environmentBox);
    profilesEdit_->setReadOnly(true);
    profilesEdit_->setToolTip(QStringLiteral("Renderer GUI 会自动扫描这个用户 Profile 目录"));
    environmentForm->addRow(QStringLiteral("保存到"), profilesEdit_);
    nameEdit_ = new QLineEdit(environmentBox);
    environmentForm->addRow(QStringLiteral("Profile 名称"), nameEdit_);
    root->addWidget(environmentBox);

    auto *tabs = new QTabWidget(central);
    tabs->setDocumentMode(true);

    auto *fontPage = new QWidget(tabs);
    auto *fontPageLayout = new QVBoxLayout(fontPage);
    fontPageLayout->setContentsMargins(12, 12, 12, 12);
    auto *fontBox = new QGroupBox(QStringLiteral("字体组合"), fontPage);
    auto *fontForm = new QFormLayout(fontBox);
    textCombo_ = new QComboBox(fontBox);
    mathCombo_ = new QComboBox(fontBox);
    cjkCombo_ = new QComboBox(fontBox);
    fontForm->addRow(QStringLiteral("正文字体"), textCombo_);
    fontForm->addRow(QStringLiteral("数学字体"), mathCombo_);
    fontForm->addRow(QStringLiteral("CJK 字体"), cjkCombo_);
    selectionInfo_ = new QLabel(fontBox);
    selectionInfo_->setWordWrap(true);
    selectionInfo_->setStyleSheet(QStringLiteral("QLabel { color: #555; padding-top: 4px; }"));
    fontForm->addRow(QString(), selectionInfo_);
    fontPageLayout->addWidget(fontBox);
    fontPageLayout->addStretch(1);
    tabs->addTab(fontPage, QStringLiteral("字体"));

    auto *packagePage = new QWidget(tabs);
    auto *packagePageLayout = new QVBoxLayout(packagePage);
    packagePageLayout->setContentsMargins(12, 12, 12, 12);
    auto *packageBox = new QGroupBox(QStringLiteral("常用宏包（StemTeX 白名单）"), packagePage);
    auto *packageLayout = new QVBoxLayout(packageBox);
    auto *packageHint = new QLabel(
        QStringLiteral("勾选需要的功能；依赖项会自动加入。字体配置和 preview 由 StemTeX 管理，不在此重复显示。"),
        packageBox);
    packageHint->setWordWrap(true);
    packageLayout->addWidget(packageHint);
    packageTree_ = new QTreeWidget(packageBox);
    packageTree_->setColumnCount(4);
    packageTree_->setHeaderLabels({QStringLiteral("宏包"), QStringLiteral("类别"),
                                   QStringLiteral("加载位置"), QStringLiteral("用途")});
    packageTree_->setRootIsDecorated(false);
    packageTree_->setAlternatingRowColors(true);
    packageTree_->setSelectionMode(QAbstractItemView::SingleSelection);
    packageTree_->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    packageTree_->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    packageTree_->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    packageTree_->header()->setSectionResizeMode(3, QHeaderView::Stretch);
    packageTree_->setMinimumHeight(260);
    packagePlanLabel_ = new QLabel(packageBox);
    packagePlanLabel_->setWordWrap(true);
    packagePlanLabel_->setStyleSheet(QStringLiteral("QLabel { color: #555; padding-top: 3px; }"));
    packageNoticeLabel_ = new QLabel(packageBox);
    packageNoticeLabel_->setWordWrap(true);
    packageNoticeLabel_->setStyleSheet(QStringLiteral(
        "QLabel { color: #6b4b00; background: #fff4cc; border: 1px solid #e0bd55; "
        "border-radius: 3px; padding: 6px; }"));
    packageNoticeLabel_->hide();
    packageLayout->addWidget(packageTree_, 1);
    packageLayout->addWidget(packagePlanLabel_);
    packageLayout->addWidget(packageNoticeLabel_);
    packagePageLayout->addWidget(packageBox, 1);
    tabs->addTab(packagePage, QStringLiteral("宏包"));

    auto *customPage = new QWidget(tabs);
    auto *customPageLayout = new QVBoxLayout(customPage);
    customPageLayout->setContentsMargins(12, 12, 12, 12);
    auto *customBox = new QGroupBox(QStringLiteral("用户 preamble"), customPage);
    auto *customLayout = new QVBoxLayout(customBox);
    auto *customHint = new QLabel(
        QStringLiteral("这里的内容会原样追加在 StemTeX 管理部分之后。可以直接写 \\usepackage、"
                       "\\newcommand 和其他 preamble 命令；不要写 \\documentclass 或 document 环境。"),
        customBox);
    customHint->setWordWrap(true);
    userPreambleEdit_ = new QPlainTextEdit(customBox);
    userPreambleEdit_->setLineWrapMode(QPlainTextEdit::NoWrap);
    userPreambleEdit_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    userPreambleEdit_->setPlaceholderText(
        QStringLiteral("% 例如：\n\\usepackage{hyperref}\n\\newcommand{\\R}{\\mathbb{R}}"));
    customLayout->addWidget(customHint);
    customLayout->addWidget(userPreambleEdit_, 1);
    customPageLayout->addWidget(customBox, 1);
    tabs->addTab(customPage, QStringLiteral("自定义"));

    auto *preamblePage = new QWidget(tabs);
    auto *preamblePageLayout = new QVBoxLayout(preamblePage);
    preamblePageLayout->setContentsMargins(12, 12, 12, 12);
    auto *preambleBox = new QGroupBox(QStringLiteral("生成的 preamble.tex"), preamblePage);
    auto *preambleLayout = new QVBoxLayout(preambleBox);
    preambleEdit_ = new QPlainTextEdit(preambleBox);
    preambleEdit_->setReadOnly(true);
    preambleEdit_->setLineWrapMode(QPlainTextEdit::NoWrap);
    preambleEdit_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    preambleEdit_->setPlaceholderText(QStringLiteral("选择有效字体后将在这里生成 preamble.tex"));
    preambleLayout->addWidget(preambleEdit_);
    preamblePageLayout->addWidget(preambleBox, 1);
    tabs->addTab(preamblePage, QStringLiteral("Preamble"));
    root->addWidget(tabs, 1);

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
    connect(packageTree_, &QTreeWidget::itemChanged, this,
            [this](QTreeWidgetItem *item, int column) { packageSelectionChanged(item, column); });
    connect(userPreambleEdit_, &QPlainTextEdit::textChanged, this, [this]() { selectionChanged(); });
    connect(createButton_, &QPushButton::clicked, this, [this]() { createProfile(); });

    reloadCatalog("latin-modern", "latin-modern-math", "simsun");
  }

 private:
  struct SpecStorage {
    QByteArray name;
    QByteArray text;
    QByteArray math;
    QByteArray cjk;
    std::vector<QByteArray> packageIds;
    std::vector<const char *> packagePointers;
    QByteArray userPreamble;
    StemTeXProfileSpecV3 spec{};

    SpecStorage(const QString &profileName, const QString &textId,
                const QString &mathId, const QString &cjkId, const QStringList &packages,
                const QString &userPreambleText)
        : name(profileName.toUtf8()), text(textId.toUtf8()), math(mathId.toUtf8()), cjk(cjkId.toUtf8()),
          userPreamble(userPreambleText.toUtf8()) {
      packageIds.reserve(static_cast<size_t>(packages.size()));
      packagePointers.reserve(static_cast<size_t>(packages.size()));
      for (const QString &id : packages) packageIds.push_back(id.toUtf8());
      for (const QByteArray &id : packageIds) packagePointers.push_back(id.constData());
      spec.name_utf8 = name.constData();
      spec.text_font_id_utf8 = text.constData();
      spec.math_font_id_utf8 = math.constData();
      spec.cjk_font_id_utf8 = cjk.constData();
      spec.package_ids_utf8 = packagePointers.empty() ? nullptr : packagePointers.data();
      spec.package_count = packagePointers.size();
      spec.user_preamble_utf8 = userPreamble.isEmpty() ? nullptr : userPreamble.constData();
    }
  };

  QString selectedId(QComboBox *combo) const {
    return combo && combo->currentIndex() >= 0 ? combo->currentData().toString() : QString();
  }

  bool selectionsValid() const {
    return !selectedId(textCombo_).isEmpty() && !selectedId(mathCombo_).isEmpty() &&
           !selectedId(cjkCombo_).isEmpty() && package_plan_valid_;
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

  QString packageCategoryLabel(const QString &category) const {
    if (category == "math") return QStringLiteral("数学");
    if (category == "chemistry") return QStringLiteral("化学");
    if (category == "physics") return QStringLiteral("物理");
    if (category == "units") return QStringLiteral("数值与单位");
    if (category == "text") return QStringLiteral("文字/颜色");
    if (category == "images") return QStringLiteral("图片");
    if (category == "tables") return QStringLiteral("表格");
    if (category == "layout") return QStringLiteral("版面工具");
    if (category == "lists") return QStringLiteral("列表");
    if (category == "graphics") return QStringLiteral("绘图基础");
    if (category == "plots") return QStringLiteral("函数图");
    if (category == "diagrams") return QStringLiteral("专业图表");
    return category;
  }

  QString packagePhaseLabel(const QString &phase) const {
    return phase == "before-fonts" ? QStringLiteral("字体前") : QStringLiteral("字体后");
  }

  QStringList explicitPackageIds() const {
    QStringList result;
    for (const QString &id : package_catalog_order_) {
      if (explicit_package_ids_.contains(id)) result.push_back(id);
    }
    return result;
  }

  bool reloadPackageCatalog() {
    QByteArray texmf = texmf_root_.toUtf8();
    StemTeXProfileContext context{texmf.constData()};
    StemTeXProfileErrorCode code = STEMTEX_PROFILE_OK;
    char *error = nullptr;
    char *json = stemtex_profile_package_catalog_json(&context, &code, &error);
    if (!json) {
      QString message = profileErrorText(error);
      packageTree_->clear();
      packagePlanLabel_->setText(message);
      packageNoticeLabel_->hide();
      package_plan_valid_ = false;
      QMessageBox::critical(this, QStringLiteral("无法读取宏包白名单"), message);
      return false;
    }
    QJsonParseError parseError{};
    QJsonDocument document = QJsonDocument::fromJson(QByteArray(json), &parseError);
    stemtex_profile_free_string(json);
    stemtex_profile_free_string(error);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
      packageTree_->clear();
      packagePlanLabel_->setText(parseError.errorString());
      packageNoticeLabel_->hide();
      package_plan_valid_ = false;
      QMessageBox::critical(this, QStringLiteral("宏包白名单错误"), parseError.errorString());
      return false;
    }

    const QSet<QString> previousExplicit = explicit_package_ids_;
    updating_packages_ = true;
    packageTree_->clear();
    package_items_.clear();
    package_catalog_order_.clear();
    explicit_package_ids_.clear();
    for (const QJsonValue &value : document.object().value("packages").toArray()) {
      const QJsonObject package = value.toObject();
      const QString id = package.value("id").toString();
      const bool available = package.value("available").toBool();
      const bool selected = available && (package_selection_initialized_
                                               ? previousExplicit.contains(id)
                                               : package.value("defaultEnabled").toBool());
      auto *item = new QTreeWidgetItem(packageTree_);
      item->setText(0, package.value("displayName").toString());
      item->setText(1, packageCategoryLabel(package.value("category").toString()));
      item->setText(2, packagePhaseLabel(package.value("phase").toString()));
      QString purpose = package.value("description").toString();
      QStringList requires;
      for (const QJsonValue &dependency : package.value("requires").toArray()) requires.push_back(dependency.toString());
      QStringList after;
      for (const QJsonValue &dependency : package.value("after").toArray()) after.push_back(dependency.toString());
      if (!requires.isEmpty()) purpose += QStringLiteral("；自动依赖：%1").arg(requires.join(QStringLiteral("、")));
      if (!after.isEmpty()) purpose += QStringLiteral("；若启用则晚于：%1").arg(after.join(QStringLiteral("、")));
      item->setText(3, purpose);
      item->setData(0, Qt::UserRole, id);
      item->setData(0, Qt::UserRole + 1, package);
      Qt::ItemFlags flags = item->flags() | Qt::ItemIsUserCheckable;
      if (!available) flags &= ~(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable);
      item->setFlags(flags);
      item->setCheckState(0, selected ? Qt::Checked : Qt::Unchecked);
      QString tooltip = purpose;
      const QString relative = package.value("relativePath").toString();
      if (!relative.isEmpty()) tooltip += QStringLiteral("\nTeX Live: texmf-dist/%1").arg(relative);
      if (!available) {
        item->setText(0, item->text(0) + QStringLiteral("  （当前树缺失）"));
        const QJsonArray missing = package.value("missing").toArray();
        if (!missing.isEmpty()) tooltip += QStringLiteral("\n缺失：%1").arg(missing.first().toString());
      }
      for (int column = 0; column < 4; ++column) item->setToolTip(column, tooltip);
      package_items_.insert(id, item);
      package_catalog_order_.push_back(id);
      if (selected) explicit_package_ids_.insert(id);
    }
    package_selection_initialized_ = true;
    updating_packages_ = false;
    return refreshPackagePlan();
  }

  bool refreshPackagePlan() {
    const QStringList ids = explicitPackageIds();
    std::vector<QByteArray> storage;
    std::vector<const char *> pointers;
    storage.reserve(static_cast<size_t>(ids.size()));
    pointers.reserve(static_cast<size_t>(ids.size()));
    for (const QString &id : ids) storage.push_back(id.toUtf8());
    for (const QByteArray &id : storage) pointers.push_back(id.constData());

    QByteArray texmf = texmf_root_.toUtf8();
    StemTeXProfileContext context{texmf.constData()};
    StemTeXProfileErrorCode code = STEMTEX_PROFILE_OK;
    char *error = nullptr;
    char *json = stemtex_profile_package_plan_json(
        &context, pointers.empty() ? nullptr : pointers.data(), pointers.size(), &code, &error);
    if (!json) {
      const QString message = profileErrorText(error);
      packagePlanLabel_->setText(QStringLiteral("无法解析宏包顺序：%1").arg(message));
      packageNoticeLabel_->hide();
      package_plan_valid_ = false;
      return false;
    }
    QJsonParseError parseError{};
    QJsonDocument document = QJsonDocument::fromJson(QByteArray(json), &parseError);
    stemtex_profile_free_string(json);
    stemtex_profile_free_string(error);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
      packagePlanLabel_->setText(QStringLiteral("宏包顺序数据错误：%1").arg(parseError.errorString()));
      packageNoticeLabel_->hide();
      package_plan_valid_ = false;
      return false;
    }

    QSet<QString> resolved;
    QStringList dependencyNotes;
    for (const QJsonValue &value : document.object().value("packages").toArray()) {
      const QJsonObject package = value.toObject();
      const QString id = package.value("id").toString();
      resolved.insert(id);
      if (!package.value("explicit").toBool()) {
        QStringList parents;
        for (const QJsonValue &parent : package.value("requiredBy").toArray()) parents.push_back(parent.toString());
        dependencyNotes.push_back(QStringLiteral("%1（由 %2 自动加入）")
                                      .arg(package.value("displayName").toString(), parents.join(QStringLiteral("、"))));
      }
    }

    updating_packages_ = true;
    for (const QString &id : package_catalog_order_) {
      QTreeWidgetItem *item = package_items_.value(id, nullptr);
      if (!item || !(item->flags() & Qt::ItemIsEnabled)) continue;
      item->setCheckState(0, explicit_package_ids_.contains(id) ? Qt::Checked
                             : resolved.contains(id) ? Qt::PartiallyChecked
                                                     : Qt::Unchecked);
    }
    updating_packages_ = false;

    QStringList order;
    for (const QJsonValue &value : document.object().value("loadOrder").toArray()) {
      QJsonObject item = value.toObject();
      if (item.value("kind").toString() == "managed") {
        order.push_back(item.value("id").toString() == "fonts"
                            ? QStringLiteral("字体配置")
                            : QStringLiteral("preview（StemTeX）"));
      } else {
        order.push_back(item.value("displayName").toString());
      }
    }
    QString text = QStringLiteral("实际加载顺序：%1").arg(order.join(QStringLiteral("  →  ")));
    if (!dependencyNotes.isEmpty()) text += QStringLiteral("\n依赖：%1").arg(dependencyNotes.join(QStringLiteral("；")));
    packagePlanLabel_->setText(text);
    QStringList notices;
    for (const QJsonValue &value : document.object().value("notices").toArray()) {
      const QJsonObject notice = value.toObject();
      if (notice.value("id").toString() == "physics-siunitx-qty") {
        notices.push_back(QStringLiteral(
            "同时使用 physics 与 siunitx：依照 siunitx 的兼容策略，\\qty 仍由 physics 提供；"
            "请用 \\SI、\\num 或 \\unit 输入 siunitx 内容。StemTeX 不会重定义这些命令。"));
      } else {
        notices.push_back(notice.value("message").toString());
      }
    }
    packageNoticeLabel_->setText(QStringLiteral("兼容提示：%1").arg(notices.join(QStringLiteral("\n"))));
    packageNoticeLabel_->setVisible(!notices.isEmpty());
    package_plan_valid_ = true;
    return true;
  }

  void packageSelectionChanged(QTreeWidgetItem *item, int column) {
    if (updating_packages_ || !item || column != 0 || !(item->flags() & Qt::ItemIsEnabled)) return;
    const QString id = item->data(0, Qt::UserRole).toString();
    if (item->checkState(0) == Qt::Checked) explicit_package_ids_.insert(id);
    else explicit_package_ids_.remove(id);
    refreshPackagePlan();
    selectionChanged();
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
      packageTree_->clear();
      packagePlanLabel_->clear();
      packageNoticeLabel_->hide();
      package_plan_valid_ = false;
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
    reloadPackageCatalog();
    statusBar()->showMessage(QStringLiteral("已读取当前 TeX Live 字体与宏包目录"), 4000);
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
    return SpecStorage(name, selectedId(textCombo_), selectedId(mathCombo_), selectedId(cjkCombo_),
                       explicitPackageIds(), userPreambleEdit_->toPlainText());
  }

  bool generateTemporaryProfile(const QString &root, const SpecStorage &storage, QString *profilePath, QString *errorText) {
    QByteArray texmf = texmf_root_.toUtf8();
    QByteArray profiles = root.toUtf8();
    StemTeXProfileContext context{texmf.constData()};
    StemTeXProfileErrorCode code = STEMTEX_PROFILE_OK;
    char *result = nullptr;
    char *error = nullptr;
    int ok = stemtex_profile_materialize_v3(&context, &storage.spec, profiles.constData(), &result, &code, &error);
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
    char *preamble = stemtex_profile_preamble_v3_utf8(&context, &storage.spec, &code, &error);
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
    statusBar()->showMessage(QStringLiteral("preamble.tex 已按当前字体与宏包组合更新"), 3000);
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
  QTreeWidget *packageTree_ = nullptr;
  QLabel *packagePlanLabel_ = nullptr;
  QLabel *packageNoticeLabel_ = nullptr;
  QPlainTextEdit *userPreambleEdit_ = nullptr;
  QHash<QString, QTreeWidgetItem *> package_items_;
  QStringList package_catalog_order_;
  QSet<QString> explicit_package_ids_;
  QPlainTextEdit *preambleEdit_ = nullptr;
  QPushButton *createButton_ = nullptr;
  bool name_touched_ = false;
  bool package_selection_initialized_ = false;
  bool updating_packages_ = false;
  bool package_plan_valid_ = false;
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
