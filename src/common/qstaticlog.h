#ifndef QSTATICLOG_H
#define QSTATICLOG_H

#include <QObject>
#include <QString>
#include <QTextBrowser>
#include <QtCore>

/**
 * @brief The QStaticLog class
 * @details This class is a static logging class that can be used to log
 *          messages to the console or a text browser.
 */
class QStaticLog : public QObject
{
    Q_OBJECT
public:
    /**
     * @brief The Level enum
     * @details This enum represents the different log levels.
     */
    enum Level {
        Silent  = 0, /*!< Log level silent */
        Error   = 1, /*!< Log level error */
        Warning = 2, /*!< Log level warning */
        Info    = 3, /*!< Log level info */
        Debug   = 4, /*!< Log level debug */
        Verbose = 5, /*!< Log level verbose */
    };

    Q_DECLARE_FLAGS(Levels, Level)

    /**
     * @brief Get the static instance of this object
     * @details This function will return the static instance of this object.
     * @return The static instance of this object
     */
    static QStaticLog &instance()
    {
        static QStaticLog instance;
        return instance;
    }

    /**
     * @brief Get the log level
     * @details This function will return the log level.
     * @retval QStaticLog::Level::Silent Log level 0 is silent
     * @retval QStaticLog::Level::Error Log level 1 is error
     * @retval QStaticLog::Level::Warning Log level 2 is warning
     * @retval QStaticLog::Level::Info Log level 3 is info
     * @retval QStaticLog::Level::Debug Log level 4 is debug
     * @retval QStaticLog::Level::Verbose Log level 5 is verbose
     */
    static QStaticLog::Level getLevel();

    /**
     * @brief Get the color mode for console
     * @details This function will return the color mode for console.
     * @retval true Console in color mode
     * @retval false Console in colorless mode
     */
    static bool isColorConsole();

    /**
     * @brief Get the color mode for richtext
     * @details This function will return the color mode for richtext.
     * @retval true Richtext in color mode
     * @retval false Richtext in colorless mode
     */
    static bool isColorRichtext();

public slots:
    /**
     * @brief Log error message to console
     * @details This function will log the message to the console.
     * @param func The function name
     * @param message The log message
     */
    static void logE(const QString &func, const QString &message);
    /**
     * @brief Log warning message to console
     * @details This function will log the message to the console.
     * @param func The function name
     * @param message The log message
     */
    static void logW(const QString &func, const QString &message);
    /**
     * @brief Log info message to console
     * @details This function will log the message to the console.
     * @param func The function name
     * @param message The log message
     */
    static void logI(const QString &func, const QString &message);
    /**
     * @brief Log debug message to console
     * @details This function will log the message to the console.
     * @param func The function name
     * @param message The log message
     */
    static void logD(const QString &func, const QString &message);
    /**
     * @brief Log verbose message to console
     * @details This function will log the message to the console.
     * @param func The function name
     * @param message The log message
     */
    static void logV(const QString &func, const QString &message);
    /**
     * @brief Set log level for both console and richtext
     * @details This function will set the log level for both console and
     *          richtext.
     * @param level The log level to set
     *              - QStaticLog::Level::Silent 0 is silent
     *              - QStaticLog::Level::Error 1 is error
     *              - QStaticLog::Level::Warning 2 is warning
     *              - QStaticLog::Level::Info 3 is info
     *              - QStaticLog::Level::Debug 4 is debug
     *              - QStaticLog::Level::Verbose 5 is verbose
     */
    static void setLevel(QStaticLog::Level level);
    /**
     * @brief Set color mode for console
     * @details This function will set the color mode for console.
     * @param color The color mode to set
     *              - true is color
     *              - false is colorless
     */
    static void setColorConsole(bool color);
    /**
     * @brief Set color mode for richtext
     * @details This function will set the color mode for richtext.
     * @param color The color mode to set
     *              - true is color
     *              - false is colorless
     */
    static void setColorRichtext(bool color);
    /**
     * @brief Set color mode for both console and richtext
     * @details This function will set the color mode for both console and richtext.
     * @param color The color mode to set
     *              - true is color
     *              - false is colorless

     */
    static void setColor(bool color);

signals:
    /**
     * @brief Log error message to richtext
     * @details This function will log the message to the richtext.
     * @param message The log message
     */
    void log(const QString &message);

private:
    /* Style for console reset */
    static const QString styleReset;
    /* Style for console bold */
    static const QString styleBold;
    /* Style for console color black */
    static const QString styleBlack;
    /* Style for console color red */
    static const QString styleRed;
    /* Style for console color green */
    static const QString styleGreen;
    /* Style for console color yellow */
    static const QString styleYellow;
    /* Style for console color blue */
    static const QString styleBlue;
    /* Style for console color magenta */
    static const QString styleMagenta;
    /* Style for console color cyan */
    static const QString styleCyan;
    /* Style for console color white */
    static const QString styleWhite;
    /* String for console error */
    static QString strEConsole;
    /* String for console warning */
    static QString strWConsole;
    /* String for console info */
    static QString strIConsole;
    /* String for console debug */
    static QString strDConsole;
    /* String for console verbose */
    static QString strVConsole;
    /* String for richtext error */
    static QString strERichtext;
    /* String for richtext warning */
    static QString strWRichtext;
    /* String for richtext info */
    static QString strIRichtext;
    /* String for richtext debug */
    static QString strDRichtext;
    /* String for richtext verbose */
    static QString strVRichtext;
    /* Log level */
    static QStaticLog::Level level;
    /* Color mode for console */
    static bool colorConsole;
    /* Color mode for richtext */
    static bool colorRichtext;
    /**
     * @brief Constructor
     * @details This is a private constructor for this class to prevent
     *          instantiation. Making the constructor private ensures that no
     *          objects of this class can be created from outside the class,
     *          enforcing a static-only usage pattern.
     */
    QStaticLog() {}
};

#endif // QSTATICLOG_H
