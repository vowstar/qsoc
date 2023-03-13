#include "qstaticlog.h"

#include <QDebug>

int QStaticLog::level = QStaticLog::levelS;
QTextBrowser *QStaticLog::textBrowser = nullptr;

void QStaticLog::logE(const QString &func, const QString &message)
{
    if (QStaticLog::level >= QStaticLog::levelE)
    {
        qCritical() << "[E]:" << func.toStdString().c_str() << ":" << message.toStdString().c_str();
        if (QStaticLog::textBrowser)
        {
            QStaticLog::textBrowser->append("<p style='color: red'>[E]:" + func + ":</p>" + message);
        }
    }
}

void QStaticLog::logW(const QString &func, const QString &message)
{
    if (QStaticLog::level >= QStaticLog::levelW)
    {
        qWarning() << "[I]:" << func.toStdString().c_str() << ":" << message.toStdString().c_str();
        if (QStaticLog::textBrowser)
        {
            QStaticLog::textBrowser->append("<p style='color: orange'>[W]:" + func + ":</p>" + message);
        }
    }
}

void QStaticLog::logI(const QString &func, const QString &message)
{
    if (QStaticLog::level >= QStaticLog::levelI)
    {
        qInfo() << "[I]:" << func.toStdString().c_str() << ":" << message.toStdString().c_str();
        if (QStaticLog::textBrowser)
        {
            QStaticLog::textBrowser->append("<p style='color: blue'>[I]:" + func + ":</p>" + message);
        }
    }
}

void QStaticLog::logD(const QString &func, const QString &message)
{
    if (QStaticLog::level >= QStaticLog::levelD)
    {
        qDebug() << "[D]:" << func.toStdString().c_str() << ":" << message.toStdString().c_str();
        if (QStaticLog::textBrowser)
        {
            QStaticLog::textBrowser->append("<p style='color: grey'>[D]:" + func + ":</p>" + message);
        }
    }
}

void QStaticLog::logV(const QString &func, const QString &message)
{
    if (QStaticLog::level >= QStaticLog::levelV)
    {
        qDebug() << "[V]:" << func.toStdString().c_str() << ":" << message.toStdString().c_str();
        if (QStaticLog::textBrowser)
        {
            QStaticLog::textBrowser->append("<p style='color: silver'>[V]:" + func + ":</p>" + message);
        }
    }
}

void QStaticLog::setLevel(int level)
{
    QStaticLog::level = level;
}

void QStaticLog::setTextBrowser(QTextBrowser *textBrowser)
{
    QStaticLog::textBrowser = textBrowser;
}
