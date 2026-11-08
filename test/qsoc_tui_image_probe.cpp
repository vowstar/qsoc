// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "cli/qagentinputmonitor.h"
#include "common/qsocinterrupt.h"
#include "tui/qtuicompositor.h"
#include "tui/qtuigraphics.h"
#include "tui/qtuiimagepreviewblock.h"

#include <QBuffer>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

#include <cstdio>
#include <memory>

namespace {

QTuiImagePreviewBlock *appendImage(
    QTuiScrollView &view, const QColor &color, const QString &label, bool details = false)
{
    QImage image(240, 160, QImage::Format_RGB32);
    image.fill(color);
    if (details) {
        for (int y = 20; y < 140; ++y) {
            for (int x = 20; x < 220; ++x) {
                if (x % 8 == 0 || y % 8 == 0)
                    image.setPixelColor(x, y, Qt::white);
                if (x >= 80 && x < 160 && y >= 50 && y < 110)
                    image.setPixelColor(x, y, (x + y) % 2 == 0 ? Qt::black : Qt::white);
            }
        }
    }
    QByteArray bytes;
    QBuffer    buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    auto block = std::make_unique<QTuiImagePreviewBlock>(
        label, QStringLiteral("image/png"), image.width(), image.height(), bytes);
    auto *ptr = block.get();
    view.appendBlock(std::move(block));
    return ptr;
}

QJsonObject imageGeometry(QTuiCompositor &compositor, const QTuiImagePreviewBlock &image, int index)
{
    int firstRow      = -1;
    int firstBlockRow = -1;
    int visibleRows   = 0;
    for (int row = 0; row < compositor.getTerminalHeight(); ++row) {
        const auto mapping = compositor.contentView().mapScreenToBlock(row);
        if (mapping.blockIdx != index) {
            continue;
        }
        if (firstRow < 0) {
            firstRow      = row;
            firstBlockRow = mapping.rowInBlock;
        }
        ++visibleRows;
    }
    return {
        {QStringLiteral("firstRow"), firstRow},
        {QStringLiteral("firstBlockRow"), firstBlockRow},
        {QStringLiteral("visibleRows"), visibleRows},
        {QStringLiteral("rows"), image.rowCount()},
        {QStringLiteral("imageRows"), image.imageCellRows()},
        {QStringLiteral("imageCols"), image.imageCellCols()},
        {QStringLiteral("folded"), image.isFolded()}};
}

} // namespace

int main(int argc, char **argv)
{
    const QCoreApplication app(argc, argv);
    if (app.arguments().size() == 3 && app.arguments().at(1) == QStringLiteral("--graphics-watch")) {
        QTuiGraphics::probeTerminal();
        const QDir         control(app.arguments().at(2));
        QAgentInputMonitor input;
        QTuiCompositor     compositor;
        compositor.dismissTopBanner();
        appendImage(compositor.contentView(), QColor(49, 101, 229), QStringLiteral("watch.png"), true);
        QString typed;
        QObject::connect(&input, &QAgentInputMonitor::inputChanged, &app, [&](const QString &text) {
            typed = text;
            compositor.inputLine().setText(text);
        });
        if (!QSocInterrupt::installBridge())
            return 3;
        input.start(false);
        compositor.start();
        QTimer report;
        QObject::connect(&report, &QTimer::timeout, &app, [&] {
            QTuiGraphics::refreshTerminal();
            const QJsonObject state{
                {QStringLiteral("protocol"), int(QTuiGraphics::protocol())},
                {QStringLiteral("revision"), qint64(QTuiGraphics::revision())},
                {QStringLiteral("placeholders"), QTuiGraphics::usesPlaceholders()},
                {QStringLiteral("evidence"), int(QTuiGraphics::capabilities().evidence)},
                {QStringLiteral("input"), typed}};
            QSaveFile file(control.filePath(QStringLiteral("state.json")));
            if (file.open(QIODevice::WriteOnly)) {
                file.write(QJsonDocument(state).toJson());
                file.commit();
            }
            if (control.exists(QStringLiteral("stop")))
                app.quit();
        });
        report.start(20);
        QTimer::singleShot(120000, &app, &QCoreApplication::quit);
        return app.exec();
    }
    if (app.arguments().size() == 3 && app.arguments().at(1) == QStringLiteral("--graphics-probe")) {
        QTuiGraphics::probeTerminal();
        QFile result(app.arguments().at(2));
        if (!result.open(QIODevice::WriteOnly))
            return 2;
        const QJsonObject report{{QStringLiteral("protocol"), int(QTuiGraphics::protocol())}};
        return result.write(QJsonDocument(report).toJson()) < 0 ? 2 : 0;
    }
    const bool live = app.arguments().size() == 3
                      && app.arguments().at(1) == QStringLiteral("--graphics-live");
    if (!live && app.arguments().size() != 2) {
        return 2;
    }
    if (live)
        QTuiGraphics::probeTerminal();
    const QDir control(app.arguments().at(live ? 2 : 1));
    if (!control.exists()) {
        return 2;
    }
    fputs("\033]0;QSOC-P1-Probe\007", stdout);
    fflush(stdout);
    QTuiCompositor compositor;
    compositor.setTitle(QStringLiteral("Image lifecycle probe"));
    compositor.dismissTopBanner();
    compositor.inputLine().setText(QStringLiteral("INPUT_SENTINEL"));
    auto *first
        = appendImage(compositor.contentView(), QColor(49, 101, 229), QStringLiteral("blue.png"));
    auto *second
        = appendImage(compositor.contentView(), QColor(37, 221, 113), QStringLiteral("green.png"));
    compositor.contentView().appendLine(QStringLiteral("AFTER_IMAGE_SENTINEL"));
    compositor.start(100);

    int    sequence = -1;
    QTimer commands;
    QObject::connect(&commands, &QTimer::timeout, &app, [&] {
        QFile commandFile(control.filePath(QStringLiteral("command.json")));
        if (!commandFile.open(QIODevice::ReadOnly)) {
            return;
        }
        const QJsonObject request      = QJsonDocument::fromJson(commandFile.readAll()).object();
        const int         nextSequence = request.value(QStringLiteral("sequence")).toInt(-1);
        if (nextSequence <= sequence) {
            return;
        }
        sequence             = nextSequence;
        const QString action = request.value(QStringLiteral("action")).toString();
        if (action == QStringLiteral("both")) {
            first->setFolded(false);
            second->setFolded(false);
        } else if (action == QStringLiteral("single")) {
            first->setFolded(true);
            second->setFolded(false);
        } else if (action == QStringLiteral("fold")) {
            second->setFolded(true);
        } else if (action == QStringLiteral("top")) {
            compositor.contentView().scrollUp(1000);
        } else if (action == QStringLiteral("bottom")) {
            compositor.contentView().scrollToBottom();
        } else if (action == QStringLiteral("step")) {
            compositor.contentView().scrollUp(2);
        } else if (action == QStringLiteral("input")) {
            compositor.inputLine().setText(QStringLiteral("INPUT_SENTINEL_CHANGED"));
        } else if (action == QStringLiteral("invalidate")) {
            compositor.invalidate();
        } else if (action == QStringLiteral("resume")) {
            compositor.pause();
            compositor.resume();
        } else if (action == QStringLiteral("off")) {
            for (int row = 0; row < 40; ++row) {
                compositor.contentView().appendLine(QStringLiteral("Generated filler %1").arg(row));
            }
            compositor.contentView().scrollToBottom();
        } else if (action == QStringLiteral("quit")) {
            compositor.stop();
            app.quit();
            return;
        }
        QMetaObject::invokeMethod(&compositor, "onTimer", Qt::DirectConnection);
        if (action == QStringLiteral("hold")) {
            for (int frame = 0; frame < 10; ++frame) {
                compositor.render();
            }
        }
        const QJsonObject response{
            {QStringLiteral("sequence"), sequence},
            {QStringLiteral("columns"), compositor.getTerminalWidth()},
            {QStringLiteral("rows"), compositor.getTerminalHeight()},
            {QStringLiteral("first"), imageGeometry(compositor, *first, 0)},
            {QStringLiteral("second"), imageGeometry(compositor, *second, 1)}};
        QSaveFile responseFile(control.filePath(QStringLiteral("response.json")));
        if (responseFile.open(QIODevice::WriteOnly)) {
            responseFile.write(QJsonDocument(response).toJson(QJsonDocument::Compact));
            responseFile.commit();
        }
    });
    commands.start(25);
    QTimer::singleShot(120000, &app, &QCoreApplication::quit);
    return app.exec();
}
