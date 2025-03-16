#include "common/qstaticlog.h"

#include <QDebug>

const QString QStaticLog::styleReset   = "\033[0m";
const QString QStaticLog::styleBold    = "\033[1m";
const QString QStaticLog::styleBlack   = "\033[30m";
const QString QStaticLog::styleRed     = "\033[31m";
const QString QStaticLog::styleGreen   = "\033[32m";
const QString QStaticLog::styleYellow  = "\033[33m";
const QString QStaticLog::styleBlue    = "\033[34m";
const QString QStaticLog::styleMagenta = "\033[35m";
const QString QStaticLog::styleCyan    = "\033[36m";
const QString QStaticLog::styleWhite   = "\033[37m";

QString QStaticLog::strEConsole = styleBold + styleRed + "[E]:" + styleReset;
QString QStaticLog::strWConsole = styleBold + styleYellow + "[W]:" + styleReset;
QString QStaticLog::strIConsole = styleBold + styleBlue + "[I]:" + styleReset;
QString QStaticLog::strDConsole = styleBold + styleGreen + "[D]:" + styleReset;
QString QStaticLog::strVConsole = styleBold + styleWhite + "[V]:" + styleReset;

QString QStaticLog::strERichtext = "<b style='color: red'>[E]:</b>";
QString QStaticLog::strWRichtext = "<b style='color: orange'>[W]:</b>";
QString QStaticLog::strIRichtext = "<b style='color: blue'>[I]:</b>";
QString QStaticLog::strDRichtext = "<b style='color: green'>[D]:</b>";
QString QStaticLog::strVRichtext = "<b style='color: silver'>[V]:</b>";

QStaticLog::Level QStaticLog::level         = QStaticLog::Level::Error;
bool              QStaticLog::colorConsole  = true;
bool              QStaticLog::colorRichtext = true;

void QStaticLog::logE(const QString &func, const QString &message)
{
    if (QStaticLog::level >= QStaticLog::Level::Error) {
        qCritical() << QString(strEConsole + func + ":" + message).toStdString().c_str();
        emit instance().log(strERichtext + func + ":" + message);
    }
}

void QStaticLog::logW(const QString &func, const QString &message)
{
    if (QStaticLog::level >= QStaticLog::Level::Warning) {
        qWarning() << QString(strWConsole + func + ":" + message).toStdString().c_str();
        emit instance().log(strWRichtext + func + ":" + message);
    }
}

void QStaticLog::logI(const QString &func, const QString &message)
{
    if (QStaticLog::level >= QStaticLog::Level::Info) {
        qInfo() << QString(strIConsole + func + ":" + message).toStdString().c_str();
        emit instance().log(strIRichtext + func + ":" + message);
    }
}

void QStaticLog::logD(const QString &func, const QString &message)
{
    if (QStaticLog::level >= QStaticLog::Level::Debug) {
        qDebug() << QString(strDConsole + func + ":" + message).toStdString().c_str();
        emit instance().log(strDRichtext + func + ":" + message);
    }
}

void QStaticLog::logV(const QString &func, const QString &message)
{
    if (QStaticLog::level >= QStaticLog::Level::Verbose) {
        qDebug() << QString(strVConsole + func + ":" + message).toStdString().c_str();
        emit instance().log(strVRichtext + func + ":" + message);
    }
}

void QStaticLog::setLevel(QStaticLog::Level level)
{
    QStaticLog::level = level;
}

QStaticLog::Level QStaticLog::getLevel()
{
    return QStaticLog::level;
}

void QStaticLog::setColorConsole(bool color)
{
    QStaticLog::colorConsole = color;
    if (color) {
        QStaticLog::strEConsole = styleBold + styleRed + "[E]:" + styleReset;
        QStaticLog::strWConsole = styleBold + styleYellow + "[W]:" + styleReset;
        QStaticLog::strIConsole = styleBold + styleBlue + "[I]:" + styleReset;
        QStaticLog::strDConsole = styleBold + styleGreen + "[D]:" + styleReset;
        QStaticLog::strVConsole = styleBold + styleWhite + "[V]:" + styleReset;
    } else {
        QStaticLog::strEConsole = "[E]:";
        QStaticLog::strWConsole = "[W]:";
        QStaticLog::strIConsole = "[I]:";
        QStaticLog::strDConsole = "[D]:";
        QStaticLog::strVConsole = "[V]:";
    }
}

bool QStaticLog::isColorConsole()
{
    return QStaticLog::colorConsole;
}

void QStaticLog::setColorRichtext(bool color)
{
    QStaticLog::colorRichtext = color;
    if (color) {
        QStaticLog::strERichtext = "<b style='color: red'>[E]:</b>";
        QStaticLog::strWRichtext = "<b style='color: orange'>[W]:</b>";
        QStaticLog::strIRichtext = "<b style='color: blue'>[I]:</b>";
        QStaticLog::strDRichtext = "<b style='color: green'>[D]:</b>";
        QStaticLog::strVRichtext = "<b style='color: silver'>[V]:</b>";
    } else {
        QStaticLog::strERichtext = "[E]:";
        QStaticLog::strWRichtext = "[W]:";
        QStaticLog::strIRichtext = "[I]:";
        QStaticLog::strDRichtext = "[D]:";
        QStaticLog::strVRichtext = "[V]:";
    }
}

bool QStaticLog::isColorRichtext()
{
    return QStaticLog::colorRichtext;
}

void QStaticLog::setColor(bool color)
{
    QStaticLog::setColorConsole(color);
    QStaticLog::setColorRichtext(color);
}
