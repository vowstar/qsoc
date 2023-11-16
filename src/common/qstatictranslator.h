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
    /**
     * @brief Get the static instance of this object
     * @details This function will return the static instance of this object.
     * @return The static instance of this object
     */
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
    /* App translator */
    static QTranslator translator;
    /* QT Base translator */
    static QTranslator translatorBase;
    /**
     * @brief Constructor
     * @details This is a private constructor for this class to prevent
     *          instantiation. Making the constructor private ensures that no
     *          objects of this class can be created from outside the class,
     *          enforcing a static-only usage pattern.
     */
    QStaticTranslator(){};
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
