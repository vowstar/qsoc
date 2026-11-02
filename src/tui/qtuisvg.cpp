// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "tui/qtuisvg.h"

#include "common/qsocsvg.h"
#include "tui/qtuigraphics.h"

#include <QBuffer>
#include <QCache>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QFile>
#include <QGuiApplication>
#include <QImage>
#include <QPainter>
#include <QProcess>
#include <QQueue>
#include <QSvgRenderer>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <utility>

#ifdef Q_OS_WIN
#include <fcntl.h>
#include <io.h>
#endif

namespace QTuiSvg {

namespace {

constexpr char kHelperArgument[] = "--svg-raster";
constexpr int  kTimeoutMs        = 5000;
constexpr int  kMaxEdgePx        = 4096;
/* One terminal row spans this many SVG units, so font-size 14 reads as body text. */
constexpr double kUnitsPerRow = 16.0;

class Queue : public QObject
{
public:
    using Done = std::function<void(const QByteArray &, const QString &)>;

    using QObject::QObject;

    static Queue &instance()
    {
        /* Owned by the application, so a running helper is reaped before Qt goes away. */
        static Queue *queue = new Queue(QCoreApplication::instance());
        return *queue;
    }

    void add(const QByteArray &source, Done done)
    {
        const QSize      cell  = QTuiGraphics::cellSize();
        const bool       light = QTuiGraphics::hasLightBackground();
        const QByteArray key   = QCryptographicHash::hash(
            source + QByteArray::number(cell.height()) + (light ? "l" : "d"),
            QCryptographicHash::Sha256);
        if (const QByteArray *png = cache.object(key)) {
            QTimer::singleShot(0, this, [png = *png, done = std::move(done)] { done(png, {}); });
            return;
        }
        pending.enqueue({source, key, light, cell.height(), std::move(done)});
        startNext();
    }

private:
    struct Job
    {
        QByteArray source;
        QByteArray key;
        bool       light = false;
        int        cellH = 16;
        Done       done;
    };

    void startNext()
    {
        if (running != nullptr || pending.isEmpty())
            return;
        Job job       = pending.dequeue();
        running       = new QProcess(this);
        auto *process = running;
        auto *timeout = new QTimer(process);
        timeout->setSingleShot(true);
        connect(timeout, &QTimer::timeout, process, [process] { process->kill(); });
        connect(
            process,
            &QProcess::finished,
            this,
            [this, process, job](int code, QProcess::ExitStatus status) {
                const QByteArray png = process->readAllStandardOutput();
                const bool       ok = status == QProcess::NormalExit && code == 0 && !png.isEmpty();
                const QString    error
                    = ok ? QString() : QString::fromUtf8(process->readAllStandardError()).trimmed();
                if (ok)
                    cache.insert(job.key, new QByteArray(png), int(png.size()));
                finish(process);
                job.done(
                    png,
                    ok                ? QString()
                    : error.isEmpty() ? QStringLiteral("drawing failed")
                                      : error);
            });
        connect(
            process, &QProcess::errorOccurred, this, [this, process, job](QProcess::ProcessError e) {
                if (e != QProcess::FailedToStart)
                    return;
                finish(process);
                job.done({}, QStringLiteral("drawing helper did not start"));
            });
        process->start(
            QCoreApplication::applicationFilePath(),
            {QString::fromLatin1(kHelperArgument),
             job.light ? QStringLiteral("light") : QStringLiteral("dark"),
             QString::number(job.cellH)});
        process->write(job.source);
        process->closeWriteChannel();
        timeout->start(kTimeoutMs);
    }

    void finish(QProcess *process)
    {
        process->deleteLater();
        if (running == process)
            running = nullptr;
        startNext();
    }

    QQueue<Job>                    pending;
    QProcess                      *running = nullptr;
    QCache<QByteArray, QByteArray> cache{64 * 1024 * 1024};
};

int fail(const QString &reason)
{
    fputs(reason.toUtf8().constData(), stderr);
    return 2;
}

} // namespace

int helperMain(int argc, char *argv[])
{
    if (argc < 4 || std::strcmp(argv[1], kHelperArgument) != 0)
        return -1;
    const bool light = std::strcmp(argv[2], "light") == 0;
    const int  cellH = std::max(1, std::atoi(argv[3]));
    /* The first available plugin wins. Packages may ship only a native one. */
    qputenv("QT_QPA_PLATFORM", "offscreen;minimal;windows;cocoa");
    QGuiApplication app(argc, argv);

    QFile input;
    if (!input.open(stdin, QIODevice::ReadOnly))
        return fail(QStringLiteral("cannot read SVG"));
    QString          error;
    const QByteArray svg
        = QSocSvg::sanitize(input.read(QSocSvg::kMaxSourceBytes + 1), light, &error);
    if (svg.isEmpty())
        return fail(error);
    QSvgRenderer renderer(svg);
    if (!renderer.isValid())
        return fail(QStringLiteral("invalid SVG"));
    QSizeF units = renderer.viewBoxF().size();
    if (units.isEmpty())
        units = renderer.defaultSize();
    if (units.isEmpty())
        return fail(QStringLiteral("SVG has no size"));
    QSizeF pixels = units * (cellH / kUnitsPerRow);
    if (std::max(pixels.width(), pixels.height()) > kMaxEdgePx)
        pixels.scale(kMaxEdgePx, kMaxEdgePx, Qt::KeepAspectRatio);
    QImage image(
        std::max(1, int(std::ceil(pixels.width()))),
        std::max(1, int(std::ceil(pixels.height()))),
        QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    {
        QPainter painter(&image);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setRenderHint(QPainter::TextAntialiasing);
        renderer.render(&painter);
    }
    QByteArray png;
    QBuffer    buffer(&png);
    buffer.open(QIODevice::WriteOnly);
    if (!image.save(&buffer, "PNG"))
        return fail(QStringLiteral("cannot encode PNG"));
#ifdef Q_OS_WIN
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    fwrite(png.constData(), 1, size_t(png.size()), stdout);
    return 0;
}

void rasterize(
    const QByteArray &source, std::function<void(const QByteArray &png, const QString &error)> done)
{
    Queue::instance().add(source, std::move(done));
}

} // namespace QTuiSvg
