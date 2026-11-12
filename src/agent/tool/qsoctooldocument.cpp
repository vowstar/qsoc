// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/tool/qsoctooldocument.h"

#include "agent/qsocinputsnapshot.h"
#include "agent/remote/qsocagentremote.h"
#include "agent/remote/qsocsftpclient.h"
#include "agent/tool/qsoctoolfilecore.h"
#include "agent/tool/qsoctoolpath.h"
#include "common/qllmservice.h"
#include "common/qsocimageattach.h"

#include <QBuffer>
#include <QCryptographicHash>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRectF>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTimer>
#include <QXmlStreamReader>
#include <QtEndian>

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>

#ifdef Q_OS_MACOS
#include <cerrno>
#include <libproc.h>
#endif

#if defined(Q_OS_UNIX) && QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
#include <sys/resource.h>
#include <unistd.h>
#endif

namespace {

constexpr qint64  kInputBytes           = 32 * 1024 * 1024;
constexpr qint64  kEntryBytes           = 8 * 1024 * 1024;
constexpr qint64  kExtractBytes         = 32 * 1024 * 1024;
constexpr quint64 kConverterMemoryBytes = 768ULL * 1024 * 1024;
constexpr int     kRegionMaxEdge        = 4096;
constexpr int     kVirtualMaxEdge       = 32768;
constexpr qint64  kVirtualMaxPixels     = 64000000;
constexpr qint64  kRegionMaxPixels      = 16000000;
constexpr int     kCursorMaxOffset      = std::numeric_limits<int>::max() - 100001;
/* Bump when the extracted text stream changes. */
constexpr auto kTextStream = "qsoc-document-text-utf16-v1";

struct CommandResult
{
    QByteArray bytes;
    QString    error;
};

CommandResult run(
    const QString       &program,
    const QStringList   &arguments,
    qint64               limit,
    QSocToolCallContext *context,
    bool                 mergeChannels = false)
{
    const QString executable = QStandardPaths::findExecutable(program);
    if (executable.isEmpty())
        return {{}, QStringLiteral("Missing optional dependency: %1").arg(program)};
    if (context && context->isCancellationRequested())
        return {{}, QStringLiteral("Document read cancelled")};
    QProcess process;
    if (mergeChannels)
        process.setProcessChannelMode(QProcess::MergedChannels);
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("LC_ALL"), QStringLiteral("C"));
    process.setProcessEnvironment(environment);
#if defined(Q_OS_UNIX) && QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    process.setChildProcessModifier([] {
        const rlimit cpu{15, 15};
        if (setrlimit(RLIMIT_CPU, &cpu) != 0)
            _exit(126);
#ifndef Q_OS_MACOS
        const rlimit memory{kConverterMemoryBytes, kConverterMemoryBytes};
        if (setrlimit(RLIMIT_AS, &memory) != 0)
            _exit(126);
#endif
    });
#endif
    QEventLoop loop;
    QTimer     timer;
    timer.setSingleShot(true);
    CommandResult result;
    QByteArray    diagnostics;
    auto          stop = [&](const QString &error) {
        if (result.error.isEmpty())
            result.error = error;
        process.kill();
        if (process.state() == QProcess::NotRunning)
            loop.quit();
    };
#ifdef Q_OS_MACOS
    /* Reserved mappings can already exceed the address-space budget before exec. */
    QTimer memoryTimer;
    memoryTimer.setInterval(20);
    memoryTimer.setTimerType(Qt::PreciseTimer);
    QObject::connect(&process, &QProcess::started, &memoryTimer, [&] { memoryTimer.start(); });
    QObject::connect(&memoryTimer, &QTimer::timeout, &loop, [&] {
        if (process.state() != QProcess::Running)
            return;
        rusage_info_v2 usage{};
        if (proc_pid_rusage(
                static_cast<int>(process.processId()),
                RUSAGE_INFO_V2,
                reinterpret_cast<rusage_info_t *>(&usage))
            != 0) {
            if (errno != ESRCH)
                stop(QStringLiteral("Cannot inspect document converter memory"));
            return;
        }
        if (usage.ri_resident_size > kConverterMemoryBytes
            || usage.ri_phys_footprint > kConverterMemoryBytes)
            stop(QStringLiteral("Document converter exceeded its 768 MiB memory budget"));
    });
#endif
    auto drain = [&] {
        process.setReadChannel(QProcess::StandardOutput);
        result.bytes += process.read(qMax<qint64>(0, limit - result.bytes.size()) + 1);
        if (!mergeChannels) {
            process.setReadChannel(QProcess::StandardError);
            diagnostics += process.read(qMax<qint64>(0, 16384 - diagnostics.size()) + 1);
        }
        if (result.bytes.size() > limit || diagnostics.size() > 16384) {
            result.bytes.clear();
            stop(QStringLiteral("Document converter output exceeds its size limit"));
        }
    };
    QObject::connect(&process, &QProcess::readyReadStandardOutput, &loop, drain);
    QObject::connect(&process, &QProcess::readyReadStandardError, &loop, drain);
    QObject::connect(&process, &QProcess::started, &loop, [&] { process.closeWriteChannel(); });
    if (context)
        QObject::connect(context, &QObject::destroyed, &loop, [&] {
            stop(QStringLiteral("Document read owner closed"));
        });
    QObject::connect(
        &process,
        QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
        &loop,
        [&](int, QProcess::ExitStatus) {
            drain();
            loop.quit();
        });
    QObject::connect(&process, &QProcess::errorOccurred, &loop, [&](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            result.error = QStringLiteral("Cannot start %1: %2").arg(program, process.errorString());
            loop.quit();
        }
    });
    QObject::connect(&timer, &QTimer::timeout, &loop, [&] {
        stop(QStringLiteral("Document converter exceeded 15 seconds"));
    });
    if (context)
        QObject::connect(context, &QSocToolCallContext::cancellationRequested, &loop, [&] {
            stop(QStringLiteral("Document read cancelled"));
        });
    timer.start(15000);
    process.start(executable, arguments);
    if (process.state() != QProcess::NotRunning)
        loop.exec();
    if (result.error.isEmpty()
        && (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0)) {
        result.error = QStringLiteral("%1 failed (exit %2): %3")
                           .arg(program)
                           .arg(process.exitCode())
                           .arg(QString::fromUtf8(diagnostics).trimmed().left(2000));
    }
    return result;
}

struct Options
{
    int                   page     = 1;
    int                   count    = 1;
    int                   offset   = 0;
    int                   maxChars = 20000;
    QString               mode     = QStringLiteral("text");
    std::optional<QRectF> region;
    std::optional<json>   cursor;
    int                   targetLongEdge = 0;
};

bool integerOption(const json &arguments, const char *name, int minimum, int maximum, int *target)
{
    const auto field = arguments.find(name);
    if (field == arguments.end())
        return true;
    if (!field->is_number_integer())
        return false;
    if (field->is_number_unsigned() && field->get<quint64>() > static_cast<quint64>(maximum))
        return false;
    const qint64 value = field->get<qint64>();
    if (value < minimum || value > maximum)
        return false;
    *target = static_cast<int>(value);
    return true;
}

QString digest(const QByteArray &bytes)
{
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

std::optional<json> decodeCursor(const json &value)
{
    if (!value.is_string())
        return {};
    const auto &text = value.get_ref<const std::string &>();
    if (text.size() > 2048 || !text.starts_with("doc1."))
        return {};
    const QByteArray encoded = QByteArray::fromStdString(text.substr(5));
    const auto       bytes   = QByteArray::fromBase64(encoded, QByteArray::Base64UrlEncoding);
    if (bytes.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals) != encoded)
        return {};
    auto cursor = json::parse(bytes.toStdString(), nullptr, false);
    if (!cursor.is_object() || cursor.size() != 9 || !cursor.value("v", json()).is_number_integer()
        || cursor.at("v") != 1)
        return {};
    for (const char *name : {"source", "sha256", "stream", "backend", "format", "part"}) {
        if (!cursor.contains(name) || !cursor[name].is_string()
            || cursor[name].get_ref<const std::string &>().empty()
            || cursor[name].get_ref<const std::string &>().size() > 256)
            return {};
    }
    int page = 0, offset = 0;
    if (!cursor.contains("page") || !cursor.contains("offset")
        || !integerOption(cursor, "page", 1, 1000000, &page)
        || !integerOption(cursor, "offset", 1, kCursorMaxOffset, &offset))
        return {};
    return cursor;
}

QString encodeCursor(const json &cursor)
{
    return QStringLiteral("doc1.")
           + QString::fromLatin1(
               QByteArray::fromStdString(cursor.dump())
                   .toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
}

QString checkOptions(const json &arguments, Options *options)
{
    if (!integerOption(arguments, "page", 1, 1000000, &options->page)
        || !integerOption(arguments, "page_count", 1, 5, &options->count)
        || !integerOption(arguments, "offset", 0, 32000000, &options->offset)
        || !integerOption(arguments, "max_chars", 1, 100000, &options->maxChars)) {
        return QStringLiteral(
            "Invalid page, page_count (1 to 5), offset, or max_chars (1 to 100000)");
    }
    if (arguments.contains("mode")) {
        if (!arguments["mode"].is_string())
            return QStringLiteral("mode must be text, image, or list");
        options->mode = QString::fromStdString(arguments["mode"].get<std::string>());
    }
    if (options->mode != "text" && options->mode != "image" && options->mode != "list")
        return QStringLiteral("mode must be text, image, or list");
    if (arguments.contains("region")) {
        const auto &region = arguments["region"];
        if (!region.is_object())
            return QStringLiteral("region must contain numeric x, y, width and height");
        double      values[4]{};
        const char *names[] = {"x", "y", "width", "height"};
        for (int i = 0; i < 4; ++i) {
            if (!region.contains(names[i]) || !region[names[i]].is_number())
                return QStringLiteral("region must contain numeric x, y, width and height");
            values[i] = region[names[i]].get<double>();
            if (!std::isfinite(values[i]) || values[i] < 0 || values[i] > 1)
                return QStringLiteral("region coordinates must be finite and within 0 to 1");
        }
        if (values[2] <= 0 || values[3] <= 0 || values[0] + values[2] > 1
            || values[1] + values[3] > 1)
            return QStringLiteral("region must have positive size and remain within the page view");
        if (options->mode != "image" || options->count != 1)
            return QStringLiteral("region requires PDF mode=image and page_count=1");
        options->region = QRectF(values[0], values[1], values[2], values[3]);
        if (!arguments.contains("target_long_edge")
            || !integerOption(
                arguments, "target_long_edge", 64, kRegionMaxEdge, &options->targetLongEdge))
            return QStringLiteral("region requires target_long_edge from 64 to 4096 pixels");
    } else if (arguments.contains("target_long_edge")) {
        return QStringLiteral("target_long_edge requires region");
    }
    if (arguments.contains("cursor")) {
        if (options->mode != "text" || options->region || arguments.contains("page")
            || arguments.contains("page_count") || arguments.contains("offset"))
            return QStringLiteral(
                "cursor requires mode=text without page, page_count, offset or region");
        options->cursor = decodeCursor(arguments["cursor"]);
        if (!options->cursor)
            return QStringLiteral("Invalid document cursor");
        options->page   = options->cursor->at("page").get<int>();
        options->offset = options->cursor->at("offset").get<int>();
    }
    return {};
}

QString extractorBackend(const QString &format, QSocToolCallContext *context, QString *error)
{
    const QString qt = QStringLiteral("Qt %1").arg(QString::fromLatin1(qVersion()));
    if (format != "pdf")
        return QStringLiteral("native XML; %1").arg(qt);
    const auto version
        = run(QStringLiteral("pdftotext"), {QStringLiteral("-v")}, 16384, context, true);
    if (!version.error.isEmpty()) {
        *error = version.error;
        return {};
    }
    const QString first = QString::fromUtf8(version.bytes).section('\n', 0, 0).simplified();
    if (!first.startsWith(QStringLiteral("pdftotext version ")) || first.size() > 200) {
        *error = QStringLiteral("Cannot identify the PDF text extractor version");
        return {};
    }
    return first + QStringLiteral("; ") + qt;
}

QString partIdentity(const json &result, const json &part)
{
    return digest(
        QByteArray::fromStdString(json::array({result.at("unit"), part.at("source")}).dump()));
}

void addCursors(json *result, const QString &source, const QByteArray &sha256, const QString &backend)
{
    (*result)["text_stream"]
        = {{"algorithm", kTextStream}, {"backend", backend.toStdString()}, {"offset_unit", "utf16"}};
    for (auto &part : (*result)["parts"]) {
        if (!part.value("truncated", false))
            continue;
        const auto next = part.at("next_offset").get<qint64>();
        if (next > kCursorMaxOffset) {
            part["cursor_unavailable"]
                = "Text continuation exceeds the supported UTF-16 offset range";
            continue;
        }
        part["next_cursor"] = encodeCursor({{"v", 1},
                                            {"source", source.toStdString()},
                                            {"sha256", sha256.toStdString()},
                                            {"stream", kTextStream},
                                            {"backend", backend.toStdString()},
                                            {"format", result->at("format")},
                                            {"part", partIdentity(*result, part).toStdString()},
                                            {"page", part.at("page")},
                                            {"offset", next}})
                                  .toStdString();
    }
}

struct TextSlice
{
    QString text;
    int     offset    = 0;
    bool    truncated = false;
};

class TextWindow
{
public:
    explicit TextWindow(const Options &options)
        : offset_(options.offset)
        , end_(static_cast<qint64>(options.offset) + options.maxChars)
    {}

    void append(const QString &value)
    {
        if (more_ || value.isEmpty())
            return;
        if (position_ + value.size() <= offset_) {
            position_ += value.size();
            return;
        }
        qsizetype first = static_cast<qsizetype>(qMax<qint64>(0, offset_ - position_));
        qsizetype last  = static_cast<qsizetype>(qMin<qint64>(value.size(), end_ - position_));
        if (first > 0 && first < value.size() && value.at(first).isLowSurrogate()
            && value.at(first - 1).isHighSurrogate()) {
            --first;
            --offset_;
        }
        if (last > first && last < value.size() && value.at(last - 1).isHighSurrogate()
            && value.at(last).isLowSurrogate()) {
            ++last;
            ++end_;
        }
        if (last > first)
            text_ += value.mid(first, last - first);
        position_ += value.size();
        more_ = position_ > end_;
    }

    TextSlice result() const { return {text_, static_cast<int>(offset_), more_}; }

private:
    QString text_;
    qint64  position_ = 0;
    qint64  offset_   = 0;
    qint64  end_      = 0;
    bool    more_     = false;
};

json textPart(int page, const QString &title, const QString &source, const TextSlice &slice)
{
    json part
        = {{"page", page},
           {"title", title.toStdString()},
           {"source", source.toStdString()},
           {"offset", slice.offset},
           {"text", slice.text.toStdString()},
           {"truncated", slice.truncated}};
    if (slice.truncated)
        part["next_offset"] = slice.offset + slice.text.size();
    return part;
}

QSize imageSize(const QByteArray &bytes)
{
    QBuffer buffer;
    buffer.setData(bytes);
    if (!buffer.open(QIODevice::ReadOnly))
        return {};
    QImageReader reader(&buffer, "PNG");
    return reader.size();
}

struct PdfRegion
{
    QSize viewport;
    QRect pixels;
    json  geometry;
};

std::optional<QRectF> pdfBox(const QString &info, const QString &name)
{
    const QRegularExpression expression(
        QStringLiteral("^Page\\s+(?:\\d+\\s+)?%1:\\s+([^\\r\\n]+)").arg(name),
        QRegularExpression::MultilineOption);
    const auto fields = expression.match(info).captured(1).simplified().split(' ');
    if (fields.size() != 4)
        return {};
    double values[4]{};
    for (int i = 0; i < 4; ++i) {
        bool valid = false;
        values[i]  = fields[i].toDouble(&valid);
        if (!valid || !std::isfinite(values[i]))
            return {};
    }
    if (values[2] <= values[0] || values[3] <= values[1])
        return {};
    return QRectF(values[0], values[1], values[2] - values[0], values[3] - values[1]);
}

std::optional<PdfRegion> pdfRegion(const QString &info, const Options &options)
{
    const auto               media = pdfBox(info, QStringLiteral("MediaBox"));
    const auto               crop  = pdfBox(info, QStringLiteral("CropBox"));
    const QRegularExpression rotationExpression(
        QStringLiteral("^Page\\s+(?:\\d+\\s+)?rot:\\s+(-?\\d+)"),
        QRegularExpression::MultilineOption);
    bool valid    = false;
    int  rotation = rotationExpression.match(info).captured(1).toInt(&valid);
    if (!media || !crop || !valid || rotation % 90 != 0)
        return {};
    rotation             = ((rotation % 360) + 360) % 360;
    const QRectF visible = media->intersected(*crop);
    double       width = visible.width(), height = visible.height();
    if (rotation == 90 || rotation == 270)
        std::swap(width, height);
    if (width <= 0 || height <= 0)
        return {};
    const auto  &region         = *options.region;
    const double longest        = qMax(region.width() * width, region.height() * height);
    const double scale          = (options.targetLongEdge - 1.0) / longest;
    const double viewportWidth  = std::floor(width * scale);
    const double viewportHeight = std::floor(height * scale);
    if (!std::isfinite(scale) || !std::isfinite(viewportWidth) || !std::isfinite(viewportHeight)
        || viewportWidth < 1 || viewportHeight < 1 || viewportWidth > kVirtualMaxEdge
        || viewportHeight > kVirtualMaxEdge || viewportWidth * viewportHeight > kVirtualMaxPixels)
        return {};
    const QSize viewport(static_cast<int>(viewportWidth), static_cast<int>(viewportHeight));
    const int   left = static_cast<int>(std::floor(region.x() * viewport.width()));
    const int   top  = static_cast<int>(std::floor(region.y() * viewport.height()));
    const int   right
        = qMin(viewport.width(), static_cast<int>(std::ceil(region.right() * viewport.width())));
    const int bottom
        = qMin(viewport.height(), static_cast<int>(std::ceil(region.bottom() * viewport.height())));
    const QRect pixels(left, top, right - left, bottom - top);
    if (pixels.isEmpty() || qMax(pixels.width(), pixels.height()) > options.targetLongEdge
        || static_cast<qint64>(pixels.width()) * pixels.height() > kRegionMaxPixels)
        return {};
    const auto rectangle = [](const QRectF &box) {
        return json{{"x", box.x()}, {"y", box.y()}, {"width", box.width()}, {"height", box.height()}};
    };
    json geometry
        = {{"view", "rotated_cropbox"},
           {"coordinate_space", "view_norm"},
           {"rotation_degrees", rotation},
           {"raw_transform", nullptr},
           {"backend_reported_media_box", rectangle(*media)},
           {"backend_reported_crop_box", rectangle(*crop)},
           {"requested_region", rectangle(region)},
           {"effective_region",
            rectangle(QRectF(
                double(left) / viewport.width(),
                double(top) / viewport.height(),
                double(pixels.width()) / viewport.width(),
                double(pixels.height()) / viewport.height()))},
           {"target_long_edge", options.targetLongEdge},
           {"virtual_viewport", {{"width", viewport.width()}, {"height", viewport.height()}}},
           {"pixel_region",
            {{"x", left}, {"y", top}, {"width", pixels.width()}, {"height", pixels.height()}}},
           {"view_norm_to_image", {viewport.width(), 0, -left, 0, viewport.height(), -top}},
           {"image_to_view_norm",
            {1.0 / viewport.width(),
             0.0,
             double(left) / viewport.width(),
             0.0,
             1.0 / viewport.height(),
             double(top) / viewport.height()}},
           {"rounding", "floor left/top, ceil right/bottom in the virtual viewport"},
           {"physical_units", "unknown"}};
    geometry["reference_expected"]
        = {width * 1600.0 / qMax(width, height), height * 1600.0 / qMax(width, height)};
    return PdfRegion{viewport, pixels, std::move(geometry)};
}

QString readPdf(
    const QString                  &path,
    const QString                  &source,
    const Options                  &options,
    QLLMService                    *llm,
    QSocToolCallContext            *callContext,
    const QSocImageAttach::Options &attachmentOptions)
{
    const QPointer<QLLMService>         model(llm);
    const QPointer<QSocToolCallContext> context(callContext);
    const QString                       page = QString::number(options.page);
    const auto                          info = run(
        QStringLiteral("pdfinfo"),
        options.region ? QStringList{"-f", page, "-l", page, "-box", path} : QStringList{path},
        65536,
        context);
    if (!info.error.isEmpty())
        return QStringLiteral("Error: %1").arg(info.error);
    static const QRegularExpression
               pages(QStringLiteral("^Pages:\\s+(\\d+)"), QRegularExpression::MultilineOption);
    const auto match = pages.match(QString::fromUtf8(info.bytes));
    bool       valid = false;
    const int  total = match.captured(1).toInt(&valid);
    if (!valid || options.page > total)
        return QStringLiteral("Error: Requested PDF page is out of range");
    json result
        = {{"format", "pdf"}, {"unit", "page"}, {"total_pages", total}, {"parts", json::array()}};
    if (options.mode == "list")
        return QString::fromStdString(result.dump(2));
    if (options.mode == "image") {
        if (options.count != 1)
            return QStringLiteral("Error: PDF image mode renders one page per call");
        QStringList renderArguments
            = {"-f", page, "-singlefile", "-cropbox", "-scale-to", "1600", "-png", path};
        auto                     imageOptions = attachmentOptions;
        json                     geometry;
        std::optional<PdfRegion> region;
        if (options.region) {
            region = pdfRegion(QString::fromUtf8(info.bytes), options);
            if (!region)
                return QStringLiteral(
                    "Error: PDF region geometry or virtual page exceeds safe rendering limits; "
                    "request a larger region or lower target_long_edge");
            const auto reference
                = run(QStringLiteral("pdftoppm"), renderArguments, 16 * 1024 * 1024, context);
            if (!reference.error.isEmpty())
                return QStringLiteral("Error: %1").arg(reference.error);
            const auto  size     = imageSize(reference.bytes);
            const auto &expected = region->geometry["reference_expected"];
            if (!size.isValid() || std::abs(size.width() - expected[0].get<double>()) > 1.1
                || std::abs(size.height() - expected[1].get<double>()) > 1.1)
                return QStringLiteral(
                    "Error: PDF CropBox view could not be calibrated with the renderer");
            geometry = region->geometry;
            geometry.erase("reference_expected");
            geometry["reference_size"] = {{"width", size.width()}, {"height", size.height()}};
            geometry["calibration"] = "Poppler reported boxes checked against a discrete CropBox "
                                      "reference raster. Physical precision is unknown";
            renderArguments
                = {"-f",
                   page,
                   "-singlefile",
                   "-cropbox",
                   "-scale-dimension-before-rotation",
                   "-scale-to-x",
                   QString::number(region->viewport.width()),
                   "-scale-to-y",
                   QString::number(region->viewport.height()),
                   "-x",
                   QString::number(region->pixels.x()),
                   "-y",
                   QString::number(region->pixels.y()),
                   "-W",
                   QString::number(region->pixels.width()),
                   "-H",
                   QString::number(region->pixels.height()),
                   "-png",
                   path};
            imageOptions.preserveSource = true;
            imageOptions.viewId = QStringLiteral("pdf:page=%1:crop=%2")
                                      .arg(options.page)
                                      .arg(
                                          QString::fromLatin1(
                                              QCryptographicHash::hash(
                                                  QByteArray::fromStdString(geometry.dump()),
                                                  QCryptographicHash::Sha256)
                                                  .toHex()));
        }
        const auto image
            = run(QStringLiteral("pdftoppm"), renderArguments, 16 * 1024 * 1024, context);
        if (!image.error.isEmpty())
            return QStringLiteral("Error: %1").arg(image.error);
        QString evidence;
        if (region) {
            if (imageSize(image.bytes) != region->pixels.size())
                return QStringLiteral("Error: PDF renderer returned unexpected region dimensions");
            const json provenance
                = {{"stage", "produced"},
                   {"render_method", "source_rerender"},
                   {"source_id", imageOptions.sourceId.toStdString()},
                   {"view_id", imageOptions.viewId.toStdString()},
                   {"page", options.page},
                   {"geometry", geometry}};
            evidence = QString::fromStdString(provenance.dump()) + QLatin1Char('\n');
        }
        return QStringLiteral("PDF page %1 of %2\n").arg(options.page).arg(total) + evidence
               + QSocImageAttach::buildAttachmentResult(
                   QStringLiteral("%1#page=%2").arg(source).arg(options.page),
                   QStringLiteral("image/png"),
                   image.bytes,
                   model,
                   true,
                   imageOptions);
    }
    const int  last      = qMin(total, options.page + options.count - 1);
    const auto extracted = run(
        QStringLiteral("pdftotext"),
        {QStringLiteral("-f"),
         page,
         QStringLiteral("-l"),
         QString::number(last),
         QStringLiteral("-layout"),
         QStringLiteral("-enc"),
         QStringLiteral("UTF-8"),
         path,
         QStringLiteral("-")},
        2 * 1024 * 1024,
        context);
    if (!extracted.error.isEmpty())
        return QStringLiteral("Error: %1").arg(extracted.error);
    const auto texts = QString::fromUtf8(extracted.bytes).split(QChar::fromLatin1('\f'));
    for (int current = options.page; current <= last; ++current) {
        const QString text = texts.value(current - options.page).trimmed();
        TextWindow    window(options);
        window.append(text);
        auto part = textPart(
            current,
            QStringLiteral("Page %1").arg(current),
            QStringLiteral("page %1").arg(current),
            window.result());
        part["text_layer_empty"] = text.isEmpty();
        result["parts"].push_back(std::move(part));
    }
    if (last < total)
        result["next_page"] = last + 1;
    result["notice"] = "Text extraction preserves layout but does not interpret charts. Use "
                       "mode=image for diagrams or scanned pages. OCR is not performed.";
    return QString::fromStdString(result.dump(2));
}

struct ZipEntry
{
    QString name;
    quint32 size = 0;
};

quint16 number16(const QByteArray &bytes, qsizetype offset)
{
    return qFromLittleEndian<quint16>(reinterpret_cast<const uchar *>(bytes.constData() + offset));
}

quint32 number32(const QByteArray &bytes, qsizetype offset)
{
    return qFromLittleEndian<quint32>(reinterpret_cast<const uchar *>(bytes.constData() + offset));
}

bool archiveEntries(QFile &file, QList<ZipEntry> *entries, QString *error)
{
    file.seek(qMax<qint64>(0, file.size() - 65557));
    const QByteArray tail = file.readAll();
    qsizetype        end  = tail.lastIndexOf(QByteArray("PK\005\006", 4));
    if (end < 0 || end + 22 > tail.size() || end + 22 + number16(tail, end + 20) != tail.size()) {
        *error = QStringLiteral("Invalid Office ZIP directory");
        return false;
    }
    const quint16 count  = number16(tail, end + 10);
    const quint32 size   = number32(tail, end + 12);
    const quint32 offset = number32(tail, end + 16);
    if (number16(tail, end + 4) != 0 || number16(tail, end + 6) != 0
        || number16(tail, end + 8) != count || count > 4096 || size > 4 * 1024 * 1024
        || static_cast<quint64>(offset) + size > static_cast<quint64>(file.size())) {
        *error = QStringLiteral(
            "Office ZIP exceeds directory limits or uses unsupported multi-volume/ZIP64 storage");
        return false;
    }
    file.seek(offset);
    const QByteArray directory = file.read(size);
    qsizetype        pos       = 0;
    quint64          total     = 0;
    QSet<QString>    names;
    for (int index = 0; index < count; ++index) {
        if (pos + 46 > directory.size() || directory.mid(pos, 4) != QByteArray("PK\001\002", 4)) {
            *error = QStringLiteral("Invalid Office ZIP entry");
            return false;
        }
        const quint16   nameSize = number16(directory, pos + 28);
        const qsizetype next     = pos + 46 + nameSize + number16(directory, pos + 30)
                                   + number16(directory, pos + 32);
        if (next > directory.size()) {
            *error = QStringLiteral("Truncated Office ZIP entry");
            return false;
        }
        const QString name     = QString::fromUtf8(directory.mid(pos + 46, nameSize));
        const quint32 unpacked = number32(directory, pos + 24);
        total += unpacked;
        const auto    components = name.split('/');
        const quint16 method     = number16(directory, pos + 10);
        const QString cleaned    = QDir::cleanPath(name);
        const bool    normalized = cleaned == (name.endsWith('/') ? name.chopped(1) : name);
        const bool    controls   = std::any_of(name.cbegin(), name.cend(), [](QChar ch) {
            return ch.unicode() < 32 || ch.unicode() == 127;
        });
        const bool invalidName   = !normalized || controls || name.isEmpty() || name.startsWith('/')
                                   || name.contains('\\') || name.contains(':')
                                   || name.contains(QChar::Null)
                                   || components.contains(QStringLiteral(".."))
                                   || names.contains(name);
        const bool symlink       = ((number32(directory, pos + 38) >> 16) & 0170000) == 0120000;
        if (invalidName || symlink || (number16(directory, pos + 8) & 1) != 0
            || (method != 0 && method != 8) || unpacked > kEntryBytes || total > 64 * 1024 * 1024) {
            *error = QStringLiteral(
                "Office ZIP has unsafe paths, encryption, unsupported compression, or excessive "
                "expanded sizes");
            return false;
        }
        names.insert(name);
        entries->append({name, unpacked});
        pos = next;
    }
    if (pos != directory.size()) {
        *error = QStringLiteral("Office ZIP directory length mismatch");
        return false;
    }
    return true;
}

class OfficeArchive
{
public:
    OfficeArchive(QString path, QList<ZipEntry> entries, QSocToolCallContext *context)
        : path_(std::move(path))
        , entries_(std::move(entries))
        , context_(context)
    {}

    bool contains(const QString &name) const
    {
        return std::any_of(entries_.cbegin(), entries_.cend(), [&](const ZipEntry &entry) {
            return entry.name == name;
        });
    }

    QByteArray read(const QString &name, QString *error)
    {
        const auto entry
            = std::find_if(entries_.cbegin(), entries_.cend(), [&](const ZipEntry &item) {
                  return item.name == name;
              });
        if (entry == entries_.cend()) {
            *error = QStringLiteral("Office part is missing: %1").arg(name);
            return {};
        }
        const auto result = run(
            QStringLiteral("unzip"),
            {QStringLiteral("-p"), path_, name},
            qMin<qint64>(entry->size, kExtractBytes - extracted_),
            context_);
        extracted_ += result.bytes.size();
        if (!result.error.isEmpty())
            *error = result.error;
        else if (result.bytes.size() != entry->size)
            *error = QStringLiteral("Expanded Office entry size differs from its ZIP directory");
        return result.bytes;
    }

private:
    QString                       path_;
    QList<ZipEntry>               entries_;
    QPointer<QSocToolCallContext> context_;
    qint64                        extracted_ = 0;
};

bool advance(QXmlStreamReader &xml)
{
    xml.readNext();
    if (xml.isDTD() || xml.isEntityReference())
        xml.raiseError(QStringLiteral("DTD and entity declarations are unsupported"));
    return !xml.atEnd();
}

TextSlice extractParagraphs(const QByteArray &bytes, const Options &options, QString *error)
{
    QXmlStreamReader xml(bytes);
    TextWindow       text(options);
    int              paragraph = 0;
    bool             cell      = false;
    while (advance(xml)) {
        const auto name = xml.name();
        if (xml.isStartElement()) {
            if (name == QStringLiteral("p")) {
                text.append(QStringLiteral("[paragraph %1] ").arg(++paragraph));
            } else if (name == QStringLiteral("tc")) {
                cell = true;
            } else if (name == QStringLiteral("t")) {
                text.append(xml.readElementText());
            } else if (name == QStringLiteral("tab")) {
                text.append(QStringLiteral("\t"));
            } else if (name == QStringLiteral("br")) {
                text.append(QStringLiteral("\n"));
            }
        } else if (xml.isEndElement()) {
            if (name == QStringLiteral("p"))
                text.append(cell ? QStringLiteral(" ") : QStringLiteral("\n"));
            else if (name == QStringLiteral("tc")) {
                cell = false;
                text.append(QStringLiteral("\t"));
            } else if (name == QStringLiteral("tr"))
                text.append(QStringLiteral("\n"));
        }
    }
    if (xml.hasError())
        *error = QStringLiteral("Invalid Office XML: %1").arg(xml.errorString());
    return text.result();
}

QStringList sharedStrings(const QByteArray &bytes, QString *error)
{
    QStringList      strings;
    QXmlStreamReader xml(bytes);
    QString          current;
    while (advance(xml)) {
        if (xml.isStartElement() && xml.name() == QStringLiteral("si"))
            current.clear();
        else if (xml.isStartElement() && xml.name() == QStringLiteral("rPh"))
            xml.skipCurrentElement();
        else if (xml.isStartElement() && xml.name() == QStringLiteral("t"))
            current += xml.readElementText();
        else if (xml.isEndElement() && xml.name() == QStringLiteral("si"))
            strings.append(current);
    }
    if (xml.hasError())
        *error = QStringLiteral("Invalid spreadsheet strings: %1").arg(xml.errorString());
    return strings;
}

struct OfficePart
{
    QString title;
    QString path;
};

QList<OfficePart> relatedParts(OfficeArchive &archive, bool spreadsheet, QString *error)
{
    QHash<QString, QString> targets;
    const QString           root     = spreadsheet ? QStringLiteral("xl/") : QStringLiteral("ppt/");
    const QString           metadata = spreadsheet ? QStringLiteral("workbook")
                                                   : QStringLiteral("presentation");
    QXmlStreamReader        relationships(
        archive
            .read(root + QStringLiteral("_rels/") + metadata + QStringLiteral(".xml.rels"), error));
    if (!error->isEmpty())
        return {};
    const QRegularExpression allowed(
        spreadsheet ? QStringLiteral("^xl/worksheets/[A-Za-z0-9_.-]+\\.xml$")
                    : QStringLiteral("^ppt/slides/slide[0-9]+\\.xml$"));
    while (advance(relationships)) {
        if (!relationships.isStartElement()
            || relationships.name() != QStringLiteral("Relationship"))
            continue;
        const auto attributes = relationships.attributes();
        if (attributes.value(QStringLiteral("TargetMode")) == QStringLiteral("External"))
            continue;
        const QString raw  = attributes.value(QStringLiteral("Target")).toString();
        const QString path = QDir::cleanPath(raw.startsWith('/') ? raw.mid(1) : root + raw);
        if (allowed.match(path).hasMatch())
            targets.insert(attributes.value(QStringLiteral("Id")).toString(), path);
    }
    if (relationships.hasError()) {
        *error = QStringLiteral("Invalid spreadsheet relationships");
        return {};
    }
    QList<OfficePart> parts;
    QXmlStreamReader  workbook(archive.read(root + metadata + QStringLiteral(".xml"), error));
    if (!error->isEmpty())
        return {};
    while (advance(workbook)) {
        if (!workbook.isStartElement()
            || workbook.name() != (spreadsheet ? QStringLiteral("sheet") : QStringLiteral("sldId")))
            continue;
        QString id;
        for (const auto &attribute : workbook.attributes()) {
            if (attribute.name() == QStringLiteral("id")
                && attribute.namespaceUri().endsWith(QStringLiteral("/relationships")))
                id = attribute.value().toString();
        }
        if (!spreadsheet && id.isEmpty())
            continue;
        const QString target = targets.value(id);
        if (target.isEmpty()) {
            *error = QStringLiteral("Spreadsheet sheet has no supported internal worksheet");
            return {};
        }
        parts.append(
            {spreadsheet ? workbook.attributes().value(QStringLiteral("name")).toString()
                         : QStringLiteral("Slide %1").arg(parts.size() + 1),
             target});
    }
    if (workbook.hasError())
        *error = QStringLiteral("Invalid spreadsheet workbook");
    return parts;
}

TextSlice spreadsheetText(
    const QByteArray &bytes, const QStringList &strings, const Options &options, QString *error)
{
    QXmlStreamReader xml(bytes);
    TextWindow       text(options);
    QString          address, type, value, formula;
    while (advance(xml)) {
        const auto name = xml.name();
        if (xml.isStartElement() && name == QStringLiteral("c")) {
            address = xml.attributes().value(QStringLiteral("r")).toString();
            type    = xml.attributes().value(QStringLiteral("t")).toString();
            value.clear();
            formula.clear();
        } else if (xml.isStartElement() && (name == QStringLiteral("v") || name == QStringLiteral("t"))) {
            value += xml.readElementText();
        } else if (xml.isStartElement() && name == QStringLiteral("f")) {
            formula = xml.readElementText();
        } else if (xml.isEndElement() && name == QStringLiteral("c")) {
            if (type == QStringLiteral("s")) {
                bool      ok    = false;
                const int index = value.toInt(&ok);
                if (!ok || index < 0 || index >= strings.size()) {
                    *error = QStringLiteral("Invalid spreadsheet shared-string index");
                    return {};
                }
                value = strings.at(index);
            }
            text.append(address);
            text.append(QStringLiteral("\t"));
            text.append(value);
            if (!formula.isEmpty()) {
                text.append(QStringLiteral("\tformula: "));
                text.append(formula);
            }
            text.append(QStringLiteral("\n"));
        }
    }
    if (xml.hasError())
        *error = QStringLiteral("Invalid worksheet XML: %1").arg(xml.errorString());
    return text.result();
}

QString readOffice(
    QFile &file, const QString &extension, const Options &options, QSocToolCallContext *context)
{
    if (options.mode == "image")
        return QStringLiteral(
            "Error: Office image rendering is unsupported; use mode=text or mode=list");
    QString         error;
    QList<ZipEntry> entries;
    if (!archiveEntries(file, &entries, &error))
        return QStringLiteral("Error: %1").arg(error);
    OfficeArchive     archive(file.fileName(), std::move(entries), context);
    QList<OfficePart> parts;
    QString           unit;
    if (extension == QStringLiteral("docx")) {
        unit = QStringLiteral("document");
        parts.append({QStringLiteral("Document body"), QStringLiteral("word/document.xml")});
    } else if (extension == QStringLiteral("xlsx")) {
        unit  = QStringLiteral("worksheet");
        parts = relatedParts(archive, true, &error);
    } else {
        unit  = QStringLiteral("slide");
        parts = relatedParts(archive, false, &error);
    }
    if (!error.isEmpty())
        return QStringLiteral("Error: %1").arg(error);
    if (parts.isEmpty() || options.page > parts.size())
        return QStringLiteral("Error: Requested Office part is out of range");
    json result
        = {{"format", extension.toStdString()},
           {"unit", unit.toStdString()},
           {"total_parts", parts.size()},
           {"parts", json::array()}};
    QStringList strings;
    if (extension == "xlsx" && options.mode == "text"
        && archive.contains(QStringLiteral("xl/sharedStrings.xml"))) {
        const auto bytes = archive.read(QStringLiteral("xl/sharedStrings.xml"), &error);
        if (error.isEmpty())
            strings = sharedStrings(bytes, &error);
    }
    const int last = qMin<qsizetype>(parts.size(), options.page + options.count - 1);
    for (int i = options.page - 1; i < last && error.isEmpty(); ++i) {
        const auto &part = parts.at(i);
        if (!archive.contains(part.path)) {
            error = QStringLiteral("Office part is missing: %1").arg(part.path);
            break;
        }
        if (options.mode == "list") {
            result["parts"].push_back(
                {{"page", i + 1},
                 {"title", part.title.toStdString()},
                 {"source", part.path.toStdString()}});
            continue;
        }
        const auto bytes = archive.read(part.path, &error);
        if (!error.isEmpty())
            break;
        const auto text = extension == "xlsx" ? spreadsheetText(bytes, strings, options, &error)
                                              : extractParagraphs(bytes, options, &error);
        result["parts"].push_back(textPart(i + 1, part.title, part.path, text));
    }
    if (!error.isEmpty())
        return QStringLiteral("Error: %1").arg(error);
    if (last < parts.size())
        result["next_page"] = last + 1;
    result["notice"] = "Text and cell values only. Images, charts, page layout, formatting and "
                       "spreadsheet date styles are not rendered. Formulas are reported, never "
                       "recalculated. Macros and external links are not executed.";
    return QString::fromStdString(result.dump(2));
}

} // namespace

QSocToolDocumentRead::QSocToolDocumentRead(
    QObject *parent, QSocPathContext *paths, QLLMService *llm, QSocRemoteConnection *remote)
    : QSocTool(parent)
    , paths_(paths)
    , llm_(llm)
    , remote_(remote)
{}

QString QSocToolDocumentRead::getName() const
{
    return QStringLiteral("read_document");
}

QString QSocToolDocumentRead::getDescription() const
{
    return QStringLiteral(
        "Read PDF, DOCX, XLSX or PPTX documents with source locations. page/page_count "
        "select PDF pages, spreadsheet sheets or presentation slides; DOCX has one document body. "
        "Each truncated text part has its own stable next_cursor. Use cursor without page or "
        "offset "
        "to resume that part. Changed sources or text extractors are rejected. "
        "mode=image renders one rotated CropBox PDF view; region plus target_long_edge rerenders "
        "a normalized view rectangle from the source. OCR is not performed. "
        "Office supports text only. Optional dependencies: Poppler pdfinfo/pdftotext/pdftoppm for "
        "PDF, unzip for Office. Inputs are limited to 32 MiB and Office expanded entries to 8 MiB "
        "each/64 MiB total. Remote files are read through SFTP into a private local snapshot; "
        "converters run locally without remote shell commands.");
}

json QSocToolDocumentRead::getParametersSchema() const
{
    return {
        {"type", "object"},
        {"properties",
         {{"file_path",
           {{"type", "string"},
            {"description", "Document path on the bound workspace, relative or absolute"}}},
          {"mode",
           {{"type", "string"},
            {"enum", {"text", "image", "list"}},
            {"description",
             "text by default; image renders one PDF page; list shows available parts"}}},
          {"page",
           {{"type", "integer"},
            {"minimum", 1},
            {"description", "First page, slide or worksheet (1-based, default 1); DOCX uses 1"}}},
          {"page_count",
           {{"type", "integer"},
            {"minimum", 1},
            {"maximum", 5},
            {"description", "Number of parts, default 1; PDF image mode requires 1"}}},
          {"region",
           {{"type", "object"},
            {"description",
             "PDF image only: top-left normalized rectangle in the rotated CropBox view; requires "
             "target_long_edge"},
            {"properties",
             {{"x", {{"type", "number"}, {"minimum", 0}, {"maximum", 1}}},
              {"y", {{"type", "number"}, {"minimum", 0}, {"maximum", 1}}},
              {"width", {{"type", "number"}, {"exclusiveMinimum", 0}, {"maximum", 1}}},
              {"height", {{"type", "number"}, {"exclusiveMinimum", 0}, {"maximum", 1}}}}},
            {"required", {"x", "y", "width", "height"}}}},
          {"target_long_edge",
           {{"type", "integer"},
            {"minimum", 64},
            {"maximum", kRegionMaxEdge},
            {"description",
             "Requires region: maximum output crop long edge in pixels; rounding may yield one "
             "fewer pixel. Never enlarges the overview"}}},
          {"offset",
           {{"type", "integer"},
            {"minimum", 0},
            {"description",
             "Text offset within each part, default 0; use next_offset to continue truncated "
             "text"}}},
          {"cursor",
           {{"type", "string"},
            {"maxLength", 2048},
            {"description",
             "Opaque next_cursor from one truncated text part. Requires mode=text without page, "
             "page_count or offset. max_chars can change"}}},
          {"max_chars",
           {{"type", "integer"},
            {"minimum", 1},
            {"maximum", 100000},
            {"description", "Characters per part, default 20000"}}}}},
        {"required", json::array({"file_path"})}};
}

QString QSocToolDocumentRead::execute(const json &arguments)
{
    if (!arguments.contains("file_path") || !arguments["file_path"].is_string())
        return QStringLiteral("Error: file_path is required");
    Options    options;
    const auto refusal = checkOptions(arguments, &options);
    if (!refusal.isEmpty())
        return QStringLiteral("Error: %1").arg(refusal);
    const QPointer<QSocToolCallContext> context(currentCallContext());
    const bool                          hadContext = context != nullptr;
    const auto                          cancelled  = [context, hadContext] {
        return hadContext && (!context || context->isCancellationRequested());
    };
    const QDeadlineTimer inputDeadline(30000);
    QString              base = paths_ ? paths_->getWorkingDir() : QString();
    if (base.isEmpty() && paths_)
        base = paths_->getProjectDir();
    if (base.isEmpty())
        base = QDir::currentPath();
    QString home = qEnvironmentVariable("HOME");
    if (home.isEmpty())
        home = QDir::homePath();
#ifdef Q_OS_WIN
    constexpr bool windows = true;
#else
    constexpr bool windows = false;
#endif
    const QString requested = QString::fromStdString(arguments["file_path"].get<std::string>());
    QString       path;
    QString       error;
    if (remote_) {
        if (!remote_->sftp())
            return QStringLiteral("Error: Remote SFTP input is not connected");
        const bool resolved = remote_->sftp()->runWithin(
            inputDeadline,
            [&] {
                remote_->learnHome(requested);
                return remote_->sftp()
                           ->canonicalize(remote_->path()->normalize(requested), &path, &error)
                       == QSocSftpClient::Canonical::Ok;
            },
            cancelled);
        if (!resolved) {
            if (cancelled())
                error = QStringLiteral("Document read cancelled");
            else if (inputDeadline.hasExpired())
                error = QStringLiteral("Document input deadline expired");
            return QStringLiteral("Error: %1").arg(error);
        }
    } else {
        path = QSocToolFileCore::localPath(requested, base, home, windows);
    }
    const QString extension = QFileInfo(requested).suffix().toLower();
    if (extension != "pdf" && extension != "docx" && extension != "xlsx" && extension != "pptx")
        return QStringLiteral("Error: read_document supports PDF, DOCX, XLSX and PPTX only");
    if (options.region && extension != "pdf")
        return QStringLiteral("Error: region is supported only for PDF image mode");
    const QString canonical = remote_ ? path : QFileInfo(path).canonicalFilePath();
    const json    sourceKey
        = {{"transport", remote_ ? "sftp" : "local"},
           {"endpoint", remote_ ? remote_->endpointIdentity().toStdString() : std::string()},
           {"path",
            (remote_ ? canonical : QDir::cleanPath(QDir::fromNativeSeparators(canonical)))
                .toStdString()}};
    const QString sourceIdentity = digest(QByteArray::fromStdString(sourceKey.dump()));
    auto          snapshot
        = remote_
              ? QSocInputSnapshot::fromRemote(
                    remote_->sftp(), path, kInputBytes, &error, cancelled, inputDeadline)
              : QSocInputSnapshot::fromLocal(path, kInputBytes, &error, cancelled, inputDeadline);
    if (!snapshot)
        return QStringLiteral("Error: %1").arg(error);
    if (cancelled())
        return QStringLiteral("Error: Document read cancelled");
    QString backend;
    if (options.cursor) {
        const auto &cursor = *options.cursor;
        if (cursor.at("source") != sourceIdentity.toStdString())
            return QStringLiteral("Error: Document cursor belongs to a different source");
        if (cursor.at("sha256") != snapshot->sha256().toStdString())
            return QStringLiteral(
                "Error: Document source changed. Read the part again without cursor");
        if (cursor.at("format") != extension.toStdString() || cursor.at("stream") != kTextStream)
            return QStringLiteral("Error: Document cursor belongs to a different text stream");
        backend = extractorBackend(extension, context, &error);
        if (backend.isEmpty())
            return QStringLiteral("Error: Cannot resume document cursor: %1").arg(error);
        if (cursor.at("backend") != backend.toStdString())
            return QStringLiteral(
                "Error: Document text extractor changed. Read the part again without cursor");
    }
    QString output;
    auto    attachmentOptions  = imageAttachmentOptions();
    attachmentOptions.sourceId = QString::fromLatin1(snapshot->sha256());
    attachmentOptions.viewId   = QStringLiteral("pdf:page=%1").arg(options.page);
    if (extension == "pdf")
        output = readPdf(snapshot->path(), path, options, llm_, context, attachmentOptions);
    else {
        QFile file(snapshot->path());
        if (!file.open(QIODevice::ReadOnly))
            return QStringLiteral("Error: Cannot open input snapshot");
        output = readOffice(file, extension, options, context);
    }
    if (output.startsWith(QLatin1Char('{'))) {
        auto result = json::parse(output.toStdString(), nullptr, false);
        if (result.is_object()) {
            result["source"]          = path.toStdString();
            result["source_sha256"]   = snapshot->sha256().toStdString();
            result["source_bytes"]    = snapshot->size();
            result["source_identity"] = sourceIdentity.toStdString();
            if (options.mode == "text") {
                const auto &parts = result.at("parts");
                if (options.cursor
                    && (parts.size() != 1
                        || options.cursor->at("part")
                               != partIdentity(result, parts.at(0)).toStdString()))
                    return QStringLiteral("Error: Document cursor belongs to a different part");
                const bool truncated = std::any_of(parts.begin(), parts.end(), [](const json &part) {
                    return part.value("truncated", false);
                });
                if (cancelled())
                    return QStringLiteral("Error: Document read cancelled");
                if (truncated && backend.isEmpty())
                    backend = extractorBackend(extension, context, &error);
                if (cancelled())
                    return QStringLiteral("Error: Document read cancelled");
                if (!backend.isEmpty())
                    addCursors(&result, sourceIdentity, snapshot->sha256(), backend);
                else if (truncated)
                    result["cursor_unavailable"] = error.toStdString();
            }
            return QString::fromStdString(result.dump(2));
        }
    }
    return output;
}

#include "moc_qsoctooldocument.cpp"
