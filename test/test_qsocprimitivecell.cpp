// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "common/qsocgenerateprimitiveclock.h"
#include "common/qsocgenerateprimitivepower.h"
#include "common/qsocgenerateprimitivereset.h"
#include "qsoc_test.h"

#include <QFile>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>

namespace {

QString cellText(const QString &name)
{
    if (name == "clock_cell.v")
        return QSocClockPrimitive().generateCellVerilog();
    if (name == "reset_cell.v")
        return QSocResetPrimitive().generateCellVerilog();
    return QSocPowerPrimitive().generateCellVerilog();
}

bool save(const QString &path, const QString &text)
{
    QFile      file(path);
    const auto bytes = text.toUtf8();
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}

/* Run a tool to completion in dir, with its merged output in log. */
bool run(
    const QString     &tool,
    const QStringList &args,
    const QString     &dir,
    QString           *log,
    int                timeout = 120000)
{
    QProcess process;
    process.setWorkingDirectory(dir);
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start(tool, args);
    const bool finished = process.waitForFinished(timeout);
    *log                = QString::fromUtf8(process.readAll());
    return finished && process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
}

class Test : public QObject
{
    Q_OBJECT

private slots:
    void verilog2005_data()
    {
        QTest::addColumn<QString>("cell");
        QTest::addColumn<QString>("tool");
        for (const char *cell : {"clock_cell.v", "reset_cell.v", "power_cell.v"}) {
            for (const char *tool : {"iverilog", "yosys"})
                QTest::newRow(qPrintable(QString("%1-%2").arg(cell, tool))) << cell << tool;
        }
    }

    /* Cells are plain Verilog-2005: no SystemVerilog-only construct may slip in. */
    void verilog2005()
    {
        QFETCH(QString, cell);
        QFETCH(QString, tool);
        if (QStandardPaths::findExecutable(tool).isEmpty())
            QSOC_TEST_MISSING_DEPENDENCY(tool);

        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QVERIFY(save(dir.filePath(cell), cellText(cell)));
        const QStringList args
            = tool == "iverilog"
                  ? QStringList{"-g2005", "-o", "a.out", cell}
                  : QStringList{"-q", "-p", "read_verilog " + cell + "; hierarchy; proc"};
        QString log;
        QVERIFY2(run(tool, args, dir.path(), &log), qPrintable(log));
    }
};

} // namespace

QSOC_TEST_MAIN(Test)
#include "test_qsocprimitivecell.moc"
