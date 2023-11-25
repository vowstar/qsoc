#ifndef QSTATICICONTHEME_H
#define QSTATICICONTHEME_H

#include <QObject>

/**
 * @brief The QStaticIconTheme class.
 * @details This class is used to setup the theme for the application.
 *          It will set the theme of light mode or dark mode according to the
 *          theme of the system.
 */
class QStaticIconTheme : public QObject
{
    Q_OBJECT
public:
    /**
     * @brief Get the static instance of this object.
     * @details This function returns the static instance of this object. It is
     *          used to provide a singleton instance of the class, ensuring that
     *          only one instance of the class exists throughout the
     *          application.
     * @return The static instance of QStaticIconTheme.
     */
    static QStaticIconTheme &instance()
    {
        static QStaticIconTheme instance;
        return instance;
    }

    /**
     * @brief Check if the theme is dark.
     * @details This function will check if the theme is dark.
     * @retval true Theme is dark.
     * @retval false Theme is not dark.
     */
    static bool isDarkTheme();

public slots:
    /**
     * @brief Setup the theme.
     * @details This function will setup the theme for the application.
     *          It will set the theme of light mode or dark mode according to
     *          the theme of the system.
     */
    static void setup();

private:
    /**
     * @brief Constructor.
     * @details This is a private constructor for QStaticIconTheme to prevent
     *          instantiation. Making the constructor private ensures that no
     *          objects of this class can be created from outside the class,
     *          enforcing a static-only usage pattern.
     */
    QStaticIconTheme() {}
};

#endif // QSTATICICONTHEME_H
