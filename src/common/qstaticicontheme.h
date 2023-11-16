#ifndef QSTATICICONTHEME_H
#define QSTATICICONTHEME_H

#include <QObject>

/**
 * @brief The QStaticIconTheme class
 * @details This class is used to setup the theme for the application.
 *          It will set the theme of light mode or dark mode according to the
 *          theme of the system.
 */
class QStaticIconTheme : public QObject
{
    Q_OBJECT
public:
    /* Static instance of this object */
    static QStaticIconTheme &instance()
    {
        static QStaticIconTheme instance;
        return instance;
    }

    /**
     * @brief Check if the theme is dark
     * @details This function will check if the theme is dark.
     * @retval true Theme is dark
     * @retval false Theme is not dark
     */
    static bool isDarkTheme();

public slots:
    /**
     * @brief Setup the theme
     * @details This function will setup the theme for the application.
     *          It will set the theme of light mode or dark mode according to
     *          the theme of the system.
     */
    static void setup();
};

#endif // QSTATICICONTHEME_H
