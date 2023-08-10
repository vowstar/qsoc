#ifndef QSTATICLOG_H
#define QSTATICLOG_H

#include <QObject>
#include <QString>
#include <QTextBrowser>

/**
 * @brief   The QStaticLog class
 * @details This class is a static logging class that can be used to log
 *          messages to the console or a text browser.
 */
class QStaticLog : public QObject
{
    Q_OBJECT
public:
    /* Log level silent */
    static const int levelS;
    /* Log level error */
    static const int levelE;
    /* Log level warning */
    static const int levelW;
    /* Log level info */
    static const int levelI;
    /* Log level debug */
    static const int levelD;
    /* Log level verbose */
    static const int levelV;

    /* Static instance of this object */
    static QStaticLog &instance()
    {
        static QStaticLog instance;
        return instance;
    }

    /**
     * @brief Get the log level
     * @return int 
     * @details This function will return the log level
     */
    static int getLevel();

    /**
     * @brief Get the color mode for console
     * @return bool 
     * @details This function will return the color mode for console
     */
    static bool isColorConsole();

    /**
     * @brief Get the color mode for richtext
     * @return bool 
     * @details This function will return the color mode for richtext
     */
    static bool isColorRichtext();

public slots:
    /**
     * @brief Log error message to console
     * @param func
     * @param message
     * @details This function will log the message to the console
     */
    static void logE(const QString &func, const QString &message);
    /**
     * @brief Log warning message to console
     * @param func
     * @param message
     * @details This function will log the message to the console
     */
    static void logW(const QString &func, const QString &message);
    /**
     * @brief Log info message to console
     * @param func
     * @param message
     * @details This function will log the message to the console
     */
    static void logI(const QString &func, const QString &message);
    /**
     * @brief Log debug message to console
     * @param func
     * @param message
     * @details This function will log the message to the console
     */
    static void logD(const QString &func, const QString &message);
    /**
     * @brief Log verbose message to console
     * @param func
     * @param message
     * @details This function will log the message to the console
     */
    static void logV(const QString &func, const QString &message);
    /**
     * @brief Set log level for both console and richtext
     * @param level
     * @details This function will set the log level for both console and richtext
     */
    static void setLevel(int level);
    /**
     * @brief Set color mode for console
     * @param color
     * @details This function will set the color mode for console
     */
    static void setColorConsole(bool color);
    /**
     * @brief Set color mode for richtext
     * @param color
     * @details This function will set the color mode for richtext
     */
    static void setColorRichtext(bool color);
    /**
     * @brief Set color mode for both console and richtext
     * @param color
     * @details This function will set the color mode for both console and richtext
     */
    static void setColor(bool color);

signals:
    /**
     * @brief Log error message to richtext
     * @param func
     * @param message
     * @details This function will log the message to the richtext
     */
    void log(const QString &message);

private:
    /* Disallow creating an instance of this object from external */
    QStaticLog() {}
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
    static int level;
    /* Color mode for console */
    static bool colorConsole;
    /* Color mode for richtext */
    static bool colorRichtext;
};

#endif // QSTATICLOG_H
