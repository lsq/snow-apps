#include "locale_manager.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QTranslator>

// qt_add_translations() embeds the .qm catalogs as a resource named after the
// target. rcc emits a translation unit whose only exports are
// qInitResources_ant_design_qt_translations/qCleanupResources_..., driven by a
// static `initializer dummy` inside it. That never runs when the catalog lives
// in a static library: no other member of libant_design_qt.a references the
// resource object, so the linker drops it outright. catalogPathFor() then lists
// an empty directory and refreshTranslator() quietly installs nothing -- the UI
// falls back to English with no error anywhere. Naming the entry point is what
// keeps the member alive, and the call is idempotent.
//
// Q_INIT_RESOURCE has to sit in a function at **global** scope, and that
// function must have a name: the macro only declares
// `extern int qInitResources_...()` inside the body, and the declaration lands
// in the innermost enclosing namespace. Inside namespace adqt::locale it looks
// for `adqt::locale::qInitResources_...`; inside an anonymous namespace the
// name is mangled with the anonymous-namespace suffix. Neither exists in rcc's
// output, so the only form that links is a plain function in the global
// namespace.
void snowEnsureAdqtTranslationResources() {
  Q_INIT_RESOURCE(ant_design_qt_translations);
}

namespace adqt::locale {

namespace {

constexpr auto kCatalogPrefix = ":/ant_design_qt/i18n/";
constexpr auto kCatalogFilePrefix = "ant_design_qt_";
constexpr auto kCatalogFileSuffix = ".qm";

QString canonicalLocaleName(const QString& rawName) {
  QString value = rawName.trimmed();
  value.replace(u'-', u'_');
  const QLocale locale(value);
  if (locale.language() == QLocale::AnyLanguage || locale.name() == QStringLiteral("C")) {
    return {};
  }
  return locale.name();
}

bool sameScript(const QLocale& lhs, const QLocale& rhs) {
  return lhs.language() == rhs.language() && lhs.script() != QLocale::AnyScript &&
         lhs.script() == rhs.script();
}

bool sameLanguage(const QLocale& lhs, const QLocale& rhs) {
  return lhs.language() == rhs.language();
}

}  // namespace

LocaleManager& LocaleManager::instance() {
  static LocaleManager manager;
  return manager;
}

LocaleManager::LocaleManager(QObject* parent)
    : QObject(parent), config_(), locale_(config_.locale) {}

void LocaleManager::setConfig(const LocaleConfig& config) {
  if (config_ == config) {
    return;
  }

  if (!refreshTranslator(config.locale)) {
    return;
  }

  config_ = config;
  locale_ = config.locale;
  applyApplicationLocale(locale_);
  ++revision_;
  emit localeChanged(locale_);
}

const LocaleConfig& LocaleManager::config() const { return config_; }

void LocaleManager::setLocale(const QLocale& locale) {
  LocaleConfig next = config_;
  next.locale = locale;
  setConfig(next);
}

const QLocale& LocaleManager::locale() const { return locale_; }

quint64 LocaleManager::localeRevision() const { return revision_; }

void LocaleManager::applyTo(QApplication& app) {
  app_ = &app;
  refreshTranslator(locale_);
  applyApplicationLocale(locale_);
}

QString LocaleManager::catalogPathFor(const QLocale& locale) const {
  snowEnsureAdqtTranslationResources();
  const QDir directory(QString::fromLatin1(kCatalogPrefix));
  const QStringList names =
      directory.entryList({QStringLiteral("ant_design_qt_*.qm")}, QDir::Files, QDir::Name);
  if (names.isEmpty()) {
    qWarning("adqt: no translation catalog is embedded under %s", kCatalogPrefix);
    return {};
  }
  const QString filePrefix = QString::fromLatin1(kCatalogFilePrefix);
  const QString fileSuffix = QString::fromLatin1(kCatalogFileSuffix);

  QString exact;
  QString script;
  QString language;
  for (const QString& name : names) {
    const QString localeName = canonicalLocaleName(
        name.mid(filePrefix.size(), name.size() - filePrefix.size() - fileSuffix.size()));
    const QLocale catalogLocale(localeName);
    if (catalogLocale.name() == locale.name()) {
      exact = name;
      break;
    }
    if (script.isEmpty() && sameScript(locale, catalogLocale)) {
      script = name;
    }
    if (language.isEmpty() && sameLanguage(locale, catalogLocale)) {
      language = name;
    }
  }

  const QString selected = !exact.isEmpty() ? exact : (!script.isEmpty() ? script : language);
  return selected.isEmpty() ? QString() : QString::fromLatin1(kCatalogPrefix) + selected;
}

bool LocaleManager::refreshTranslator(const QLocale& locale) {
  std::unique_ptr<QTranslator> nextTranslator;
  QString nextTranslatorLocaleName;

  const QString catalogPath = catalogPathFor(locale);
  if (!catalogPath.isEmpty()) {
    nextTranslator = std::make_unique<QTranslator>();
    if (!nextTranslator->load(catalogPath)) {
      return false;
    }
    nextTranslatorLocaleName = nextTranslator->language();
  }

  if (QCoreApplication::instance()) {
    QTranslator* const previousTranslator = translator_.get();
    if (nextTranslator) {
      QCoreApplication::installTranslator(nextTranslator.get());
    }
    if (previousTranslator) {
      QCoreApplication::removeTranslator(previousTranslator);
    }
  }

  translator_ = std::move(nextTranslator);
  translatorLocaleName_ = std::move(nextTranslatorLocaleName);
  return true;
}

void LocaleManager::applyApplicationLocale(const QLocale& locale) {
  QLocale::setDefault(locale);
  if (app_) {
    app_->setLayoutDirection(locale.textDirection());
  }
}

}  // namespace adqt::locale
