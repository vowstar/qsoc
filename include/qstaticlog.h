#ifndef QSTATICLOG_H
#define QSTATICLOG_H

#include <QString>
#include <QTextBrowser>

class QStaticLog
{
public:
    static const int levelS = 0;
    static const int levelE = 1;
    static const int levelW = 2;
    static const int levelI = 3;
    static const int levelD = 4;
    static const int levelV = 5;

    static void logE(const QString &func, const QString &message);
    static void logW(const QString &func, const QString &message);
    static void logI(const QString &func, const QString &message);
    static void logD(const QString &func, const QString &message);
    static void logV(const QString &func, const QString &message);

    static void setLevel(int level);
    static void setTextBrowser(QTextBrowser *textBrowser);

private:
    /* Disallow creating an instance of this object */
    QStaticLog() {}

    static int level;
    static QTextBrowser *textBrowser;
};

#endif // QSTATICLOG_H
