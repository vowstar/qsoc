#ifndef QSTATICLOG_H
#define QSTATICLOG_H

#include <QObject>
#include <QString>
#include <QTextBrowser>

class QStaticLog : public QObject
{
    Q_OBJECT
public:
    static const int levelS;
    static const int levelE;
    static const int levelW;
    static const int levelI;
    static const int levelD;
    static const int levelV;

    static QStaticLog &instance()
    {
        static QStaticLog instance;
        return instance;
    }

public slots:
    static void logE(const QString &func, const QString &message);
    static void logW(const QString &func, const QString &message);
    static void logI(const QString &func, const QString &message);
    static void logD(const QString &func, const QString &message);
    static void logV(const QString &func, const QString &message);

    static void setLevel(int level);
    static void setColorConsole(bool color);
    static void setColorRichtext(bool color);
    static void setColor(bool color);

signals:
    void log(const QString &message);

private:
    /* Disallow creating an instance of this object from external */
    QStaticLog() {}

    static const QString styleReset;
    static const QString styleBold;
    static const QString styleBlack;
    static const QString styleRed;
    static const QString styleGreen;
    static const QString styleYellow;
    static const QString styleBlue;
    static const QString styleMagenta;
    static const QString styleCyan;
    static const QString styleWhite;

    static QString strEConsole;
    static QString strWConsole;
    static QString strIConsole;
    static QString strDConsole;
    static QString strVConsole;

    static QString strERichtext;
    static QString strWRichtext;
    static QString strIRichtext;
    static QString strDRichtext;
    static QString strVRichtext;

    static int level;
    static bool colorConsole;
    static bool colorRichtext;
};

#endif // QSTATICLOG_H
