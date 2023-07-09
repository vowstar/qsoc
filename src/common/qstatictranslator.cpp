#include "common/qstatictranslator.h"

#include <QCoreApplication>
#include <QLocale>

QTranslator QStaticTranslator::translator;
QTranslator QStaticTranslator::translatorBase;

void QStaticTranslator::initTranslator(QTranslator &translator, const QString &prefix)
{
    const QStringList uiLanguages = QLocale::system().uiLanguages();

    for (const QString &locale : uiLanguages) {
        const QString baseName = QLocale(locale).name();

        if (translator.load(prefix + baseName + ".qm")) {
            break;
        }
    }

    QCoreApplication::installTranslator(&translator);
}

void QStaticTranslator::setup()
{
    initTranslator(QStaticTranslator::translator, ":/i18n/app_");
    initTranslator(QStaticTranslator::translatorBase, ":/i18n/qtbase_");
}
