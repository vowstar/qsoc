#ifndef QSTATICTRANSLATOR_H
#define QSTATICTRANSLATOR_H

#include <QObject>
#include <QTranslator>

/**
 * @brief The QStaticTranslator class
 * @details This class is used to setup the translator for the application.
 *          It will load the translation files from the resource file.
 */
class QStaticTranslator : public QObject
{
    Q_OBJECT
public:
    /* Static instance of this object */
    static QStaticTranslator &instance()
    {
        static QStaticTranslator instance;
        return instance;
    }

public slots:
    /**
     * @brief Setup the translator
     * @details This function will setup the translator for the application.
     *          It will load the translation files from the resource file.
     */
    static void setup();

private:
    /* Disallow creating an instance of this object from external */
    QStaticTranslator(){};
    /* App translator */
    static QTranslator translator;
    /* QT Base translator */
    static QTranslator translatorBase;
    /**
     * @brief Initialize the translator
     * @details This function will initialize the translator for the application.
     *          It will load the translation files from the resource file.
     * @param translator translator object
     * @param prefix prefix of the translation files
     */
    static void initTranslator(QTranslator &translator, const QString &prefix = ":/i18n/");
};

#endif // QSTATICTRANSLATOR_H
