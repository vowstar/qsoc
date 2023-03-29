#ifndef QSTATICTRANSLATOR_H
#define QSTATICTRANSLATOR_H

#include <QObject>
#include <QTranslator>

class QStaticTranslator : public QObject
{
    Q_OBJECT

    static QStaticTranslator &instance()
    {
        static QStaticTranslator instance;
        return instance;
    }

public slots:
    static void setup(void);

private:
    /* Disallow creating an instance of this object from external */
    QStaticTranslator(){};

    static QTranslator translator;
    static QTranslator translatorBase;

    static void initTranslator(QTranslator &translator, const QString &prefix = ":/i18n/");
};

#endif // QSTATICTRANSLATOR_H
