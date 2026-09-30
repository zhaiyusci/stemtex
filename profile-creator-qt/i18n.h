#pragma once

#include <QCoreApplication>
#include <QDebug>
#include <QLibraryInfo>
#include <QLocale>
#include <QStringList>
#include <QTranslator>

namespace CreatorI18n {

// Fixed context: CreatorWindow has no Q_OBJECT, so inherited tr() would use
// QMainWindow's context. Keep UI translations independent of QObject inheritance.
inline QString text(const char *source) {
  return QCoreApplication::translate("ProfileCreator", source);
}

inline QString catalogText(const QString &source) {
  const QByteArray utf8 = source.toUtf8();
  return QCoreApplication::translate("ProfileCatalog", utf8.constData());
}

class Translations {
 public:
  bool initialize(const QStringList &arguments) {
    QString language = QStringLiteral("system");
    for (int i = 1; i < arguments.size(); ++i) {
      if (arguments.at(i) == QStringLiteral("--language")) {
        if (++i >= arguments.size()) return invalidLanguage();
        language = arguments.at(i);
      } else if (arguments.at(i).startsWith(QStringLiteral("--language="))) {
        language = arguments.at(i).mid(11);
      }
    }
    if (language != QStringLiteral("system") && language != QStringLiteral("en") &&
        language != QStringLiteral("zh_CN")) return invalidLanguage();
    const QLocale locale = language == QStringLiteral("system")
                               ? QLocale::system() : QLocale(language);
    chinese_ = locale.language() == QLocale::Chinese &&
               locale.script() == QLocale::SimplifiedChineseScript;
    if (!chinese_) return true;  // English source text is the fallback.

    if (qt_.load(QStringLiteral(":/i18n/qtbase_zh_CN.qm")) ||
        qt_.load(QStringLiteral("qtbase_zh_CN"),
                 QLibraryInfo::path(QLibraryInfo::TranslationsPath))) {
      QCoreApplication::installTranslator(&qt_);
    }
    if (!app_.load(QStringLiteral(":/i18n/stemtex-profile-creator_zh_CN.qm"))) {
      qCritical("Cannot load embedded Profile Creator translation: :/i18n/stemtex-profile-creator_zh_CN.qm");
      return false;
    }
    QCoreApplication::installTranslator(&app_);
    return true;
  }

  bool smokeCheck() const {
    // Test both a UI string and an API-originated dynamic description. Checking
    // exact catalog values also catches an accidentally mismatched TS context.
    const QString title = text("Create Profile");
    const QString description = catalogText(QStringLiteral("Do not load xeCJK"));
    // The stock catalog owns this string: do not extract it into our own TS.
    const char *stockCancelSource = "Cancel";
    const QString cancel = QCoreApplication::translate("QPlatformTheme", stockCancelSource);
    const bool valid = chinese_
        ? (!app_.isEmpty() && title == QString::fromUtf8("创建 Profile") &&
           description == QString::fromUtf8("不加载 xeCJK") &&
           !qt_.isEmpty() && cancel == QString::fromUtf8("取消"))
        : (title == QStringLiteral("Create Profile") &&
           description == QStringLiteral("Do not load xeCJK"));
    if (!valid) qCritical("Profile Creator translation smoke check failed");
    else qInfo().noquote() << "Profile Creator i18n smoke passed:" << (chinese_ ? "zh_CN" : "en");
    return valid;
  }

 private:
  static bool invalidLanguage() {
    qCritical("Usage: --language en|zh_CN|system");
    return false;
  }
  bool chinese_ = false;
  QTranslator qt_;
  QTranslator app_;
};

}  // namespace CreatorI18n
