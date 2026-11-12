// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/tool/qsoctooldocument.h"
#include "qsocofficemedia_p.h"

#include "agent/qsocinputsnapshot.h"
#include "agent/remote/qsocagentremote.h"
#include "agent/remote/qsocsftpclient.h"
#include "agent/tool/qsoctoolfilecore.h"
#include "agent/tool/qsoctoolpath.h"
#include "common/qllmservice.h"
#include "common/qsocimageattach.h"
#include "common/qsocprocesslimits.h"

#include <QBuffer>
#include <QCryptographicHash>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QMimeDatabase>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRectF>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrl>
#include <QXmlStreamReader>
#include <QtEndian>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <optional>

#ifdef Q_OS_MACOS
#include <cerrno>
#include <libproc.h>
#endif

#if defined(Q_OS_UNIX) && QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
#include <signal.h>
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
constexpr auto   kTextStream     = "qsoc-document-text-utf16-v1";
constexpr auto   kBoxStream      = "qsoc-pdf-word-boxes-v1";
constexpr qint64 kBoxXmlBytes    = 8 * 1024 * 1024;
constexpr size_t kBoxResultBytes = 1024 * 1024;

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
    bool                 mergeChannels   = false,
    QDeadlineTimer       deadline        = QDeadlineTimer(QDeadlineTimer::Forever),
    const QString       &officeDirectory = {})
{
    const bool    officeRenderer = !officeDirectory.isEmpty();
    const QString executable     = QStandardPaths::findExecutable(program);
    if (executable.isEmpty())
        return {{}, QStringLiteral("Missing optional dependency: %1").arg(program)};
    if (context && context->isCancellationRequested())
        return {{}, QStringLiteral("Document read cancelled")};
    if (deadline.hasExpired())
        return {{}, QStringLiteral("Document conversion deadline expired")};
    QSocProcessLimits processLimits;
    QProcess          process;
    if (officeRenderer && !processLimits.configure(process, kConverterMemoryBytes))
        return {{}, QStringLiteral("Cannot configure Office renderer process limits")};
    if (mergeChannels)
        process.setProcessChannelMode(QProcess::MergedChannels);
    QProcessEnvironment environment;
    if (officeRenderer) {
        for (const char *key :
             {"PATH",
              "SystemRoot",
              "WINDIR",
              "PATHEXT",
              "COMSPEC",
              "LANG",
              "LC_CTYPE",
              "FONTCONFIG_FILE",
              "FONTCONFIG_PATH",
              "XDG_DATA_DIRS"}) {
            if (qEnvironmentVariableIsSet(key))
                environment.insert(QString::fromLatin1(key), qEnvironmentVariable(key));
        }
        const QDir    directory(officeDirectory);
        const QString home = directory.filePath(QStringLiteral("home"));
        const QString temp = directory.filePath(QStringLiteral("tmp"));
        for (const char *key : {"HOME", "USERPROFILE"})
            environment.insert(QString::fromLatin1(key), home);
        for (const char *key : {"TMPDIR", "TMP", "TEMP"})
            environment.insert(QString::fromLatin1(key), temp);
        environment.insert(QStringLiteral("XDG_CONFIG_HOME"), home + QStringLiteral("/config"));
        environment.insert(QStringLiteral("XDG_CACHE_HOME"), home + QStringLiteral("/cache"));
        environment.insert(QStringLiteral("XDG_DATA_HOME"), home + QStringLiteral("/data"));
        environment.insert(QStringLiteral("APPDATA"), home + QStringLiteral("/appdata"));
        environment.insert(QStringLiteral("LOCALAPPDATA"), home + QStringLiteral("/localdata"));
        environment.insert(QStringLiteral("SAL_USE_VCLPLUGIN"), QStringLiteral("svp"));
        environment.insert(QStringLiteral("SAL_DISABLE_OPENCL"), QStringLiteral("1"));
    } else {
        environment = QProcessEnvironment::systemEnvironment();
    }
    environment.insert(QStringLiteral("LC_ALL"), QStringLiteral("C"));
    process.setProcessEnvironment(environment);
#if defined(Q_OS_UNIX) && QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    process.setChildProcessModifier([officeRenderer] {
        if (officeRenderer && ::setsid() < 0)
            _exit(126);
        if (officeRenderer) {
            const rlimit file{kInputBytes, kInputBytes};
            const rlimit core{0, 0};
            if (setrlimit(RLIMIT_FSIZE, &file) != 0 || setrlimit(RLIMIT_CORE, &core) != 0)
                _exit(126);
        }
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
    timer.setTimerType(Qt::PreciseTimer);
    CommandResult result;
    QByteArray    diagnostics;
    qint64        processGroup = 0;
    auto          killTree     = [&] {
#if defined(Q_OS_UNIX) && QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        if (officeRenderer && processGroup > 0)
            ::kill(-static_cast<pid_t>(processGroup), SIGKILL);
#endif
        process.kill();
    };
    auto stop = [&](const QString &error) {
        if (result.error.isEmpty())
            result.error = error;
        killTree();
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
        std::array<pid_t, 129> members{};
        int                    count = 1;
        members[0]                   = static_cast<pid_t>(process.processId());
        if (officeRenderer) {
            errno = 0;
            count = proc_listpgrppids(
                static_cast<pid_t>(processGroup), members.data(), sizeof(members));
            const int listError = errno;
            if (count <= 0) {
                if (process.waitForFinished(0) || process.state() == QProcess::NotRunning)
                    return;
                stop(QStringLiteral(
                         "Cannot inspect the bounded Office renderer process group "
                         "(members %1, errno %2)")
                         .arg(count)
                         .arg(listError));
                return;
            }
            if (count >= static_cast<int>(members.size())) {
                stop(QStringLiteral(
                         "Office renderer process group exceeds 128 members "
                         "(sampled %1)")
                         .arg(count));
                return;
            }
        }
        quint64 resident = 0, footprint = 0;
        for (int index = 0; index < count; ++index) {
            const auto pid = members[static_cast<size_t>(index)];
            if (pid <= 0)
                continue;
            errno = 0;
            if (officeRenderer && ::getpgid(pid) != processGroup) {
                if (errno == ESRCH)
                    continue;
                stop(QStringLiteral("Office renderer process group changed during inspection"));
                return;
            }
            rusage_info_v2 usage{};
            if (proc_pid_rusage(pid, RUSAGE_INFO_V2, reinterpret_cast<rusage_info_t *>(&usage))
                != 0) {
                if (errno == ESRCH)
                    continue;
                stop(QStringLiteral("Cannot inspect document converter memory"));
                return;
            }
            if (usage.ri_resident_size > kConverterMemoryBytes - resident
                || usage.ri_phys_footprint > kConverterMemoryBytes - footprint) {
                stop(QStringLiteral("Document converter exceeded its 768 MiB memory budget"));
                return;
            }
            resident += usage.ri_resident_size;
            footprint += usage.ri_phys_footprint;
        }
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
    QObject::connect(&process, &QProcess::started, &loop, [&] {
        processGroup = process.processId();
        process.closeWriteChannel();
    });
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
        stop(
            deadline.hasExpired() ? QStringLiteral("Document conversion deadline expired")
                                  : QStringLiteral("Document converter exceeded 15 seconds"));
    });
    if (context)
        QObject::connect(context, &QSocToolCallContext::cancellationRequested, &loop, [&] {
            stop(QStringLiteral("Document read cancelled"));
        });
    timer.start(
        deadline.isForever()
            ? 15000
            : static_cast<int>(qMin<qint64>(15000, qMax<qint64>(1, deadline.remainingTime()))));
    process.start(executable, arguments);
    if (process.state() != QProcess::NotRunning)
        loop.exec();
    if (officeRenderer)
        killTree();
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
    int                   wordOffset     = 0;
    int                   maxWords       = 200;
    int                   mediaOffset    = 0;
    int                   maxMedia       = 50;
    QString               mediaId;
    QString               expectedSource;
    QString               renderer;
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
            return QStringLiteral(
                "mode must be text, image, list, text_boxes, media, or media_image");
        options->mode = QString::fromStdString(arguments["mode"].get<std::string>());
    }
    if (options->mode != "text" && options->mode != "image" && options->mode != "list"
        && options->mode != "text_boxes" && options->mode != "media"
        && options->mode != "media_image")
        return QStringLiteral("mode must be text, image, list, text_boxes, media, or media_image");
    if (arguments.contains("renderer")) {
        if (!arguments["renderer"].is_string() || arguments["renderer"] != "libreoffice"
            || options->mode != "image" || options->count != 1 || arguments.contains("offset")
            || arguments.contains("max_chars"))
            return QStringLiteral(
                "renderer=libreoffice requires mode=image, page_count=1 and no text offsets");
        options->renderer = QStringLiteral("libreoffice");
    }
    const bool media = options->mode == "media" || options->mode == "media_image";
    if (media) {
        if (arguments.contains("offset") || arguments.contains("max_chars"))
            return QStringLiteral("Office media modes do not use text offsets");
        if (!integerOption(arguments, "media_offset", 0, 4096, &options->mediaOffset)
            || !integerOption(arguments, "max_media", 1, 100, &options->maxMedia))
            return QStringLiteral("Invalid media_offset (0 to 4096) or max_media (1 to 100)");
        if (options->mode == "media_image") {
            if (!arguments.contains("media_id") || !arguments["media_id"].is_string()
                || arguments["media_id"].get_ref<const std::string &>().empty()
                || arguments["media_id"].get_ref<const std::string &>().size() > 8192)
                return QStringLiteral("media_image requires media_id from the media list");
            if (arguments.contains("media_offset") || arguments.contains("max_media"))
                return QStringLiteral(
                    "media_image selects one media_id without media_offset/max_media");
            options->mediaId = QString::fromStdString(arguments["media_id"].get<std::string>());
        } else if (arguments.contains("media_id")) {
            return QStringLiteral("media_id requires mode=media_image");
        }
    } else if (
        arguments.contains("media_id") || arguments.contains("media_offset")
        || arguments.contains("max_media")) {
        return QStringLiteral("media selectors require an Office media mode");
    }
    if (arguments.contains("source_sha256")) {
        if ((options->mode != "media_image" && options->renderer.isEmpty())
            || !arguments["source_sha256"].is_string())
            return QStringLiteral(
                "source_sha256 requires media_image or rendered Office image mode");
        options->expectedSource = QString::fromStdString(
            arguments["source_sha256"].get<std::string>());
        static const QRegularExpression hash(QStringLiteral("^[0-9a-f]{64}$"));
        if (!hash.match(options->expectedSource).hasMatch())
            return QStringLiteral("source_sha256 must be a lowercase SHA-256 digest");
    }
    if (options->mode == "text_boxes") {
        if (options->count != 1 || arguments.contains("offset") || arguments.contains("max_chars"))
            return QStringLiteral(
                "text_boxes requires page_count=1 and uses word_offset/max_words, not "
                "offset/max_chars");
        if (!integerOption(arguments, "word_offset", 0, 1000000, &options->wordOffset)
            || !integerOption(arguments, "max_words", 1, 1000, &options->maxWords))
            return QStringLiteral("Invalid word_offset (0 to 1000000) or max_words (1 to 1000)");
    } else if (arguments.contains("word_offset") || arguments.contains("max_words")) {
        return QStringLiteral("word_offset and max_words require mode=text_boxes");
    }
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
        if ((options->mode != "text" && options->mode != "text_boxes") || options->region
            || arguments.contains("page") || arguments.contains("page_count")
            || arguments.contains("offset") || arguments.contains("word_offset"))
            return QStringLiteral(
                "cursor requires text or text_boxes without page, page_count, offset, word_offset "
                "or region");
        options->cursor = decodeCursor(arguments["cursor"]);
        if (!options->cursor)
            return QStringLiteral("Invalid document cursor");
        options->page = options->cursor->at("page").get<int>();
        if (options->mode == "text_boxes")
            options->wordOffset = options->cursor->at("offset").get<int>();
        else
            options->offset = options->cursor->at("offset").get<int>();
        if (options->wordOffset > 1000000)
            return QStringLiteral("Invalid word cursor offset");
    }
    return {};
}

QString extractorBackend(
    const QString       &format,
    QSocToolCallContext *context,
    QString             *error,
    QDeadlineTimer       deadline = QDeadlineTimer(QDeadlineTimer::Forever))
{
    const QString qt = QStringLiteral("Qt %1").arg(QString::fromLatin1(qVersion()));
    if (format != "pdf")
        return QStringLiteral("native XML; %1").arg(qt);
    const auto version
        = run(QStringLiteral("pdftotext"), {QStringLiteral("-v")}, 16384, context, true, deadline);
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

void addCursors(
    json             *result,
    const QString    &source,
    const QByteArray &sha256,
    const QString    &backend,
    bool              boxes = false)
{
    (*result)[boxes ? "box_stream" : "text_stream"]
        = {{"algorithm", boxes ? kBoxStream : kTextStream},
           {"backend", backend.toStdString()},
           {"offset_unit", boxes ? "source_word_index" : "utf16"}};
    if (boxes)
        (*result)["box_stream"]["index_space"]
            = "Poppler-emitted XML word nodes for the selected CropBox view";
    for (auto &part : (*result)["parts"]) {
        if (!part.value("truncated", false))
            continue;
        const auto next = part.at(boxes ? "next_word_offset" : "next_offset").get<qint64>();
        if (next > kCursorMaxOffset) {
            part["cursor_unavailable"]
                = "Text continuation exceeds the supported UTF-16 offset range";
            continue;
        }
        part["next_cursor"] = encodeCursor({{"v", 1},
                                            {"source", source.toStdString()},
                                            {"sha256", sha256.toStdString()},
                                            {"stream", boxes ? kBoxStream : kTextStream},
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

struct PdfView
{
    QRectF media;
    QRectF crop;
    QSizeF size;
    int    rotation;
};

std::optional<PdfView> pdfView(const QString &info)
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
    if (!std::isfinite(width) || !std::isfinite(height) || width <= 0 || height <= 0)
        return {};
    return PdfView{*media, *crop, QSizeF(width, height), rotation};
}

std::optional<PdfRegion> pdfRegion(const QString &info, const Options &options)
{
    const auto view = pdfView(info);
    if (!view)
        return {};
    const double width = view->size.width(), height = view->size.height();
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
           {"rotation_degrees", view->rotation},
           {"raw_transform", nullptr},
           {"backend_reported_media_box", rectangle(view->media)},
           {"backend_reported_crop_box", rectangle(view->crop)},
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

bool boxNumber(const QXmlStreamAttributes &attributes, const char *name, double *value)
{
    bool valid = false;
    *value     = attributes.value(QString::fromLatin1(name)).toDouble(&valid);
    return valid && std::isfinite(*value);
}

json boxRectangle(const QRectF &box)
{
    return {{"x", box.x()}, {"y", box.y()}, {"width", box.width()}, {"height", box.height()}};
}

QString readPdfBoxes(
    const QString                  &path,
    const QString                  &info,
    const Options                  &options,
    int                             total,
    QSocToolCallContext            *callContext,
    const QSocImageAttach::Options &attachment,
    const QString                  &knownBackend,
    QDeadlineTimer                  deadline)
{
    const QPointer<QSocToolCallContext> context(callContext);
    const bool                          hadContext = context != nullptr;
    const auto                          view       = pdfView(info);
    if (!view)
        return QStringLiteral("Error: Unsupported PDF CropBox or rotation for word boxes");
    QString       error;
    const QString backend = knownBackend.isEmpty()
                                ? extractorBackend(QStringLiteral("pdf"), context, &error, deadline)
                                : knownBackend;
    if (backend.isEmpty())
        return QStringLiteral("Error: %1").arg(error);
    const QString version = backend.section(';', 0, 0).mid(
        QStringLiteral("pdftotext version ").size());
    json versions = {{"pdftotext", backend.toStdString()}};
    for (const QString &program : {QStringLiteral("pdfinfo"), QStringLiteral("pdftoppm")}) {
        const auto reported = run(program, {QStringLiteral("-v")}, 16384, context, true, deadline);
        if (!reported.error.isEmpty())
            return QStringLiteral("Error: %1").arg(reported.error);
        const QString first = QString::fromUtf8(reported.bytes).section('\n', 0, 0).simplified();
        if (first != program + QStringLiteral(" version ") + version)
            return QStringLiteral(
                "Error: PDF word boxes require matching Poppler extractor, geometry and renderer "
                "versions");
        versions[program.toStdString()] = first.toStdString();
    }
    const QString page      = QString::number(options.page);
    const auto    extracted = run(
        QStringLiteral("pdftotext"),
        {"-f", page, "-l", page, "-r", "72", "-bbox-layout", "-cropbox", "-enc", "UTF-8", path, "-"},
        kBoxXmlBytes,
        context,
        false,
        deadline);
    if (!extracted.error.isEmpty())
        return QStringLiteral("Error: %1").arg(extracted.error);
    QXmlStreamReader xml(extracted.bytes);
    QSizeF           dimensions, reportedSize;
    bool             seenPage = false, inPage = false;
    int              words = 0, outside = 0, omitted = 0, clipped = 0;
    json             selected = json::array();
    const int        end      = options.wordOffset + options.maxWords;
    while (!xml.atEnd()) {
        if (deadline.hasExpired()
            || (hadContext && (!context || context->isCancellationRequested())))
            return QStringLiteral(
                "Error: PDF word-box read cancelled or conversion deadline expired");
        xml.readNext();
        if ((xml.isDTD()
             && (xml.dtdName() != QStringLiteral("html") || !xml.entityDeclarations().isEmpty()
                 || !xml.notationDeclarations().isEmpty()))
            || xml.isEntityReference()) {
            xml.raiseError(QStringLiteral("Unsupported PDF word-box XML declarations"));
        } else if (xml.isStartElement() && xml.name() == QStringLiteral("page")) {
            double width = 0, height = 0;
            if (seenPage || !boxNumber(xml.attributes(), "width", &width)
                || !boxNumber(xml.attributes(), "height", &height) || width <= 0 || height <= 0) {
                xml.raiseError(QStringLiteral("Invalid PDF word-box page dimensions"));
                continue;
            }
            seenPage = inPage = true;
            reportedSize      = QSizeF(width, height);
            dimensions = view->rotation == 90 || view->rotation == 270 ? QSizeF(height, width)
                                                                       : QSizeF(width, height);
            if (std::abs(dimensions.width() - view->size.width()) > 0.021
                || std::abs(dimensions.height() - view->size.height()) > 0.021)
                xml.raiseError(
                    QStringLiteral("Unsupported PDF word-box dimension/rotation contract"));
        } else if (xml.isEndElement() && xml.name() == QStringLiteral("page")) {
            inPage = false;
        } else if (xml.isStartElement() && xml.name() == QStringLiteral("word")) {
            double x0 = 0, y0 = 0, x1 = 0, y1 = 0;
            if (!inPage || !boxNumber(xml.attributes(), "xMin", &x0)
                || !boxNumber(xml.attributes(), "yMin", &y0)
                || !boxNumber(xml.attributes(), "xMax", &x1)
                || !boxNumber(xml.attributes(), "yMax", &y1) || x1 <= x0 || y1 <= y0
                || !std::isfinite(x1 - x0) || !std::isfinite(y1 - y0)) {
                xml.raiseError(QStringLiteral("Invalid PDF word rectangle"));
                continue;
            }
            const QString text = xml.readElementText();
            const QRectF  native(x0, y0, x1 - x0, y1 - y0);
            const QRectF  visible = native.intersected(QRectF(QPointF(), dimensions));
            const int     index   = words++;
            if (visible.isEmpty())
                ++outside;
            if (index < options.wordOffset || index >= end)
                continue;
            if (visible.isEmpty()) {
                ++omitted;
                continue;
            }
            if (text.size() > 8192)
                return QStringLiteral(
                    "Error: PDF word text exceeds 8192 UTF-16 units. Use mode=text for this part");
            const bool partial = visible != native;
            if (partial)
                ++clipped;
            selected.push_back(
                {{"word_index", index},
                 {"text", text.toStdString()},
                 {"box",
                  boxRectangle(QRectF(
                      visible.x() / dimensions.width(),
                      visible.y() / dimensions.height(),
                      visible.width() / dimensions.width(),
                      visible.height() / dimensions.height()))},
                 {"backend_box", boxRectangle(native)},
                 {"partially_visible", partial}});
        }
    }
    if (xml.hasError() || !seenPage)
        return QStringLiteral("Error: Invalid PDF word-box XML: %1").arg(xml.errorString());
    const auto reference = run(
        QStringLiteral("pdftoppm"),
        {"-f", page, "-singlefile", "-cropbox", "-scale-to", "1600", "-png", path},
        16 * 1024 * 1024,
        context,
        false,
        deadline);
    if (!reference.error.isEmpty())
        return QStringLiteral("Error: %1").arg(reference.error);
    const QSize  pixels = imageSize(reference.bytes);
    const double scale  = 1600.0 / qMax(dimensions.width(), dimensions.height());
    if (!pixels.isValid() || std::abs(pixels.width() - dimensions.width() * scale) > 1.1
        || std::abs(pixels.height() - dimensions.height() * scale) > 1.1)
        return QStringLiteral("Error: PDF word-box view could not be calibrated with the renderer");
    const bool truncated = end < words;
    json       part
        = {{"page", options.page},
           {"source", QStringLiteral("page %1").arg(options.page).toStdString()},
           {"source_id", attachment.sourceId.toStdString()},
           {"view_id", attachment.viewId.toStdString()},
           {"word_offset", options.wordOffset},
           {"word_count", qMax(0, qMin(options.maxWords, words - options.wordOffset))},
           {"total_words", words},
           {"words", std::move(selected)},
           {"truncated", truncated},
           {"text_layer_empty", words == 0},
           {"page_words_outside_view", outside},
           {"omitted_outside_view", omitted},
           {"partially_visible_words", clipped},
           {"source_kind", "pdf_text_layer"}};
    if (truncated)
        part["next_word_offset"] = end;
    part["geometry"]
        = {{"view", "rotated_cropbox"},
           {"coordinate_space", "view_norm"},
           {"rotation_degrees", view->rotation},
           {"raw_transform", nullptr},
           {"physical_units", "unknown"},
           {"backend_reported_media_box", boxRectangle(view->media)},
           {"backend_reported_crop_box", boxRectangle(view->crop)},
           {"backend_page_size",
            {{"width", reportedSize.width()}, {"height", reportedSize.height()}}},
           {"view_size", {{"width", dimensions.width()}, {"height", dimensions.height()}}},
           {"reference_size", {{"width", pixels.width()}, {"height", pixels.height()}}},
           {"backend_to_view_norm",
            {1.0 / dimensions.width(), 0.0, 0.0, 0.0, 1.0 / dimensions.height(), 0.0}},
           {"view_norm_to_reference_pixels", {pixels.width(), 0, 0, 0, pixels.height(), 0}},
           {"backend_versions", versions},
           {"calibration", "backend page dimensions and reference raster checked"},
           {"word_coordinate_contract",
            "Poppler words are already rotated; XML page dimensions are unrotated"}};
    json result
        = {{"format", "pdf"},
           {"unit", "page"},
           {"total_pages", total},
           {"parts", json::array({part})},
           {"box_stream",
            {{"algorithm", kBoxStream},
             {"backend", backend.toStdString()},
             {"offset_unit", "source_word_index"}}},
           {"notice",
            "Words come from the PDF text layer and can include existing OCR errors. XML word "
            "order is not a reading-order guarantee. Partially visible words retain their full "
            "text. Boxes are font-based word rectangles, not glyph outlines, table structure or "
            "formula interpretation. OCR is not performed."}};
    if (options.page < total)
        result["next_page"] = options.page + 1;
    return QString::fromStdString(result.dump());
}

QString readPdf(
    const QString                  &path,
    const QString                  &source,
    const Options                  &options,
    QLLMService                    *llm,
    QSocToolCallContext            *callContext,
    const QSocImageAttach::Options &attachmentOptions,
    const QString                  &knownBackend,
    QDeadlineTimer                  deadline,
    QString                        *producedViewId = nullptr)
{
    const QPointer<QLLMService>         model(llm);
    const QPointer<QSocToolCallContext> context(callContext);
    const QString                       page = QString::number(options.page);
    const auto                          info = run(
        QStringLiteral("pdfinfo"),
        options.region || options.mode == "text_boxes"
            ? QStringList{"-f", page, "-l", page, "-box", path}
            : QStringList{path},
        65536,
        context,
        false,
        deadline);
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
    if (options.mode == "text_boxes")
        return readPdfBoxes(
            path,
            QString::fromUtf8(info.bytes),
            options,
            total,
            context,
            attachmentOptions,
            knownBackend,
            deadline);
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
            const auto reference = run(
                QStringLiteral("pdftoppm"),
                renderArguments,
                16 * 1024 * 1024,
                context,
                false,
                deadline);
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
            imageOptions.viewId         = attachmentOptions.viewId + QStringLiteral(":crop=")
                                          + digest(QByteArray::fromStdString(geometry.dump()));
        }
        const auto image = run(
            QStringLiteral("pdftoppm"), renderArguments, 16 * 1024 * 1024, context, false, deadline);
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
        if (producedViewId)
            *producedViewId = imageOptions.viewId;
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
        context,
        false,
        deadline);
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
    OfficeArchive(
        QString              path,
        QList<ZipEntry>      entries,
        QSocToolCallContext *context,
        QDeadlineTimer       deadline = QDeadlineTimer(QDeadlineTimer::Forever))
        : path_(std::move(path))
        , entries_(std::move(entries))
        , context_(context)
        , deadline_(deadline)
    {}

    bool contains(const QString &name) const
    {
        return std::any_of(entries_.cbegin(), entries_.cend(), [&](const ZipEntry &entry) {
            return entry.name == name;
        });
    }

    const QList<ZipEntry> &entries() const { return entries_; }

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
        QString pattern;
        for (const QChar ch : name) {
            if (ch == QLatin1Char('['))
                pattern += QStringLiteral("[[]");
            else if (ch == QLatin1Char('*'))
                pattern += QStringLiteral("[*]");
            else if (ch == QLatin1Char('?'))
                pattern += QStringLiteral("[?]");
            else
                pattern += ch;
        }
        const auto result = run(
            QStringLiteral("unzip"),
            {QStringLiteral("-p"), path_, pattern},
            qMin<qint64>(entry->size, kExtractBytes - extracted_),
            context_,
            false,
            deadline_);
        extracted_ += result.bytes.size();
        if (!result.error.isEmpty())
            *error = result.error;
        else if (result.bytes.size() != entry->size)
            *error
                = QStringLiteral(
                      "Expanded Office entry %1 has %2 bytes, expected %3 from its ZIP directory")
                      .arg(name)
                      .arg(result.bytes.size())
                      .arg(entry->size);
        return result.bytes;
    }

private:
    QString                       path_;
    QList<ZipEntry>               entries_;
    QPointer<QSocToolCallContext> context_;
    qint64                        extracted_ = 0;
    QDeadlineTimer                deadline_;
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

QString readOfficeMedia(
    OfficeArchive                  &archive,
    const QString                  &extension,
    const QList<OfficePart>        &parts,
    const Options                  &options,
    const QString                  &source,
    qint64                          sourceBytes,
    QLLMService                    *llm,
    QSocToolCallContext            *callContext,
    const QSocImageAttach::Options &attachmentOptions,
    QDeadlineTimer                  deadline)
{
    const QPointer<QSocToolCallContext> context(callContext);
    const bool                          hadContext = context != nullptr;
    const auto                          cancelled  = [context, hadContext, deadline] {
        return deadline.hasExpired()
               || (hadContext && (!context || context->isCancellationRequested()));
    };
    QSocOfficeMedia::Package package{
        [&](const QString &name) { return archive.contains(name); },
        [&](const QString &name, QString *error) { return archive.read(name, error); },
        cancelled};
    QList<QSocOfficeMedia::Part> selected;
    const int last = qMin<qsizetype>(parts.size(), options.page + options.count - 1);
    for (int i = options.page - 1; i < last; ++i)
        selected.append({i + 1, parts[i].title, parts[i].path});
    QString error;
    auto    result = QSocOfficeMedia::discover(extension, selected, package, &error);
    if (!error.isEmpty())
        return QStringLiteral("Error: %1").arg(error);
    auto all              = std::move(result["media"]);
    result["format"]      = extension.toStdString();
    result["total_parts"] = parts.size();
    result["total_media"] = all.size();
    result["unit"] = extension == "docx" ? "document" : extension == "xlsx" ? "worksheet" : "slide";
    result["mode"] = options.mode.toStdString();
    if (options.mode == "media")
        result["media_offset"] = options.mediaOffset;
    else
        result["selected_media_id"] = options.mediaId.toStdString();
    result["media"] = json::array();
    const int end   = options.mode == "media_image"
                          ? static_cast<int>(all.size())
                          : qMin<int>(all.size(), options.mediaOffset + options.maxMedia);
    const int start = options.mode == "media_image" ? 0 : options.mediaOffset;
    for (int i = start; i < end; ++i) {
        auto          media = all[i];
        const QString part  = QString::fromStdString(media["archive_part"].get<std::string>());
        if (options.mode == "media_image" && part != options.mediaId)
            continue;
        const auto bytes = archive.read(part, &error);
        if (!error.isEmpty())
            return QStringLiteral("Error: %1").arg(error);
        const QString direct = QSocImageAttach::detectMimeByMagic(bytes);
        const QString mime   = direct.isEmpty() ? QMimeDatabase().mimeTypeForData(bytes).name()
                                                : direct;
        media["sha256"]      = digest(bytes).toStdString();
        media["bytes"]       = bytes.size();
        media["mime"]        = mime.toStdString();
        media["direct_image_candidate"] = !direct.isEmpty();
        if (options.mode == "media_image") {
            media["origin"]              = "original_embedded_bytes";
            media["appearance_rendered"] = false;
            if (direct.isEmpty()) {
                media["image_available"] = false;
                media["notice"]
                    = "The original entry remains in the Office archive. This encoding is not "
                      "supported for direct model image input. No conversion was performed.";
                result["media"].push_back(media);
                return QString::fromStdString(result.dump(2));
            }
            auto imageOptions           = attachmentOptions;
            imageOptions.preserveSource = true;
            imageOptions.viewId = QStringLiteral("office:media=%1").arg(digest(part.toUtf8()));
            const json evidence
                = {{"source", source.toStdString()},
                   {"source_sha256", attachmentOptions.sourceId.toStdString()},
                   {"source_bytes", sourceBytes},
                   {"media", media},
                   {"coverage", result["coverage"]},
                   {"view_id", imageOptions.viewId.toStdString()},
                   {"stage", "produced"}};
            const auto serialized = evidence.dump();
            if (serialized.size() > 1024 * 1024)
                return QStringLiteral(
                    "Error: Office media evidence exceeds 1 MiB. Select fewer parts");
            return QStringLiteral("Office original embedded media\n")
                   + QString::fromStdString(serialized) + QLatin1Char('\n')
                   + QSocImageAttach::buildAttachmentResult(
                       source + QStringLiteral("#part=") + part,
                       mime,
                       bytes,
                       llm,
                       true,
                       imageOptions);
        }
        result["media"].push_back(std::move(media));
    }
    if (options.mode == "media_image")
        return QStringLiteral(
            "Error: media_id is not an embedded image in the selected Office parts");
    if (end < static_cast<int>(all.size()))
        result["next_media_offset"] = end;
    if (last < parts.size())
        result["next_page"] = last + 1;
    return QString::fromStdString(result.dump(2));
}

bool screenOfficeRenderer(OfficeArchive &archive, const QString &extension, QString *error)
{
    const QString main = extension == "docx"   ? QStringLiteral("word/document.xml")
                         : extension == "xlsx" ? QStringLiteral("xl/workbook.xml")
                                               : QStringLiteral("ppt/presentation.xml");
    if (!archive.contains(main) || !archive.contains(QStringLiteral("[Content_Types].xml"))) {
        *error = QStringLiteral("Office rendering requires a complete OOXML package");
        return false;
    }
    for (const auto &entry : archive.entries()) {
        const QString lower = entry.name.toLower();
        if (lower.contains(QStringLiteral("vbaproject"))
            || lower.contains(QStringLiteral("/embeddings/"))
            || lower.contains(QStringLiteral("/activex/"))
            || lower.startsWith(QStringLiteral("customui/"))
            || lower.startsWith(QStringLiteral("basic/"))
            || lower.startsWith(QStringLiteral("scripts/")) || lower == QStringLiteral("mimetype")
            || lower.contains(QStringLiteral("/externallinks/"))
            || lower == QStringLiteral("xl/connections.xml")) {
            *error = QStringLiteral(
                "Office rendering refuses macros, embedded objects and data connections");
            return false;
        }
        if (!lower.endsWith(QStringLiteral(".xml")) && !lower.endsWith(QStringLiteral(".rels")))
            continue;
        const auto bytes = archive.read(entry.name, error);
        if (!error->isEmpty())
            return false;
        QXmlStreamReader xml(bytes);
        while (advance(xml)) {
            if (!xml.isStartElement())
                continue;
            const auto name = xml.name();
            if (name == QStringLiteral("Relationship")) {
                const QString type
                    = xml.attributes().value(QStringLiteral("Type")).toString().toLower();
                const bool external = xml.attributes()
                                          .value(QStringLiteral("TargetMode"))
                                          .compare(QStringLiteral("External"), Qt::CaseInsensitive)
                                      == 0;
                if ((external && !type.endsWith(QStringLiteral("/image"))
                     && !type.endsWith(QStringLiteral("/hyperlink")))
                    || type.endsWith(QStringLiteral("/attachedtemplate"))
                    || type.endsWith(QStringLiteral("/oleobject"))
                    || type.endsWith(QStringLiteral("/control"))
                    || type.endsWith(QStringLiteral("/afchunk")))
                    xml.raiseError(QStringLiteral(
                        "Active Office relationship is unsupported by the renderer"));
            } else if (name == QStringLiteral("Override") || name == QStringLiteral("Default")) {
                if (xml.attributes()
                        .value(QStringLiteral("ContentType"))
                        .contains(QStringLiteral("macroEnabled"), Qt::CaseInsensitive))
                    xml.raiseError(QStringLiteral(
                        "Macro-enabled Office content is unsupported by the renderer"));
            } else if (
                name == QStringLiteral("instrText") || name == QStringLiteral("f")
                || name == QStringLiteral("definedName") || name == QStringLiteral("fldSimple")) {
                QString formula = name == QStringLiteral("fldSimple")
                                      ? xml.attributes()
                                            .value(
                                                QStringLiteral(
                                                    "http://schemas.openxmlformats.org/"
                                                    "wordprocessingml/2006/main"),
                                                QStringLiteral("instr"))
                                            .toString()
                                            .toUpper()
                                      : xml.readElementText().toUpper();
                static const QRegularExpression strings(QStringLiteral("\"(?:[^\"]|\"\")*\""));
                formula.remove(strings);
                static const QRegularExpression activeFunction(QStringLiteral(
                    "(?:^|[^A-Z0-9_])(?:_XLFN\\.|COM\\.MICROSOFT\\.)?(?:WEBSERVICE|DDE|RTD|IMAGE)"
                    "\\s*\\("));
                static const QRegularExpression activeField(
                    QStringLiteral("^\\s*(?:DDE(?:AUTO)?|INCLUDETEXT|INCLUDEPICTURE)\\b"));
                if (((name == QStringLiteral("instrText") || name == QStringLiteral("fldSimple"))
                         ? activeField
                         : activeFunction)
                        .match(formula)
                        .hasMatch())
                    xml.raiseError(QStringLiteral(
                        "Active Office field or formula is unsupported by the renderer"));
            }
        }
        if (xml.hasError()) {
            *error = QStringLiteral("Office renderer screening failed: %1").arg(xml.errorString());
            return false;
        }
    }
    return true;
}

QString renderOffice(
    QFile                          &file,
    const QString                  &extension,
    const Options                  &options,
    OfficeArchive                  &archive,
    QSocToolCallContext            *context,
    QLLMService                    *llm,
    const QSocImageAttach::Options &attachmentOptions,
    const QString                  &source,
    QDeadlineTimer                  deadline)
{
    QString error;
    if (!screenOfficeRenderer(archive, extension, &error))
        return QStringLiteral("Error: %1").arg(error);
    QString program;
#ifdef Q_OS_WIN
    const QStringList names{
        QStringLiteral("soffice.com"), QStringLiteral("libreoffice"), QStringLiteral("soffice")};
#else
    const QStringList names{QStringLiteral("libreoffice"), QStringLiteral("soffice")};
#endif
    for (const auto &name : names) {
        program = QStandardPaths::findExecutable(name);
        if (!program.isEmpty())
            break;
    }
    if (program.isEmpty())
        return QStringLiteral(
            "Error: Missing optional dependency: LibreOffice 25.2 or newer CLI "
            "(libreoffice/soffice)");
    QTemporaryDir temporary;
    if (!temporary.isValid())
        return QStringLiteral("Error: Cannot create an Office renderer directory");
    const QString profile = temporary.filePath(QStringLiteral("profile"));
    if (!QDir().mkpath(profile + QStringLiteral("/user"))
        || !QDir().mkpath(temporary.filePath(QStringLiteral("home")))
        || !QDir().mkpath(temporary.filePath(QStringLiteral("tmp"))))
        return QStringLiteral("Error: Cannot create an Office renderer profile");
    QFile            config(profile + QStringLiteral("/user/registrymodifications.xcu"));
    const QByteArray settings = R"(<?xml version="1.0" encoding="UTF-8"?>
<oor:items xmlns:oor="http://openoffice.org/2001/registry">
<item oor:path="/org.openoffice.Office.Common/Security/Scripting">
<prop oor:name="DisableMacrosExecution" oor:op="fuse"><value>true</value></prop>
<prop oor:name="MacroSecurityLevel" oor:op="fuse"><value>3</value></prop>
<prop oor:name="DisableActiveContent" oor:op="fuse"><value>true</value></prop>
<prop oor:name="BlockUntrustedRefererLinks" oor:op="fuse"><value>true</value></prop>
</item>
<item oor:path="/org.openoffice.Office.Writer/Content/Update"><prop oor:name="Link" oor:op="fuse"><value>2</value></prop></item>
<item oor:path="/org.openoffice.Office.Calc/Content/Update"><prop oor:name="Link" oor:op="fuse"><value>1</value></prop></item>
</oor:items>)";
    if (!config.open(QIODevice::WriteOnly) || config.write(settings) != settings.size())
        return QStringLiteral("Error: Cannot write an Office renderer profile");
    config.close();
    const auto version = run(
        program,
        {QStringLiteral("-env:UserInstallation=")
             + QUrl::fromLocalFile(profile).toString(QUrl::FullyEncoded),
         QStringLiteral("--version")},
        4096,
        context,
        true,
        deadline,
        temporary.path());
    if (!version.error.isEmpty())
        return QStringLiteral("Error: %1").arg(version.error);
    const QString identity = QString::fromUtf8(version.bytes).trimmed();
    static const QRegularExpression supported(QStringLiteral("^LibreOffice ([0-9]+)\\.([0-9]+)\\."));
    const auto match = supported.match(identity);
    if (!match.hasMatch() || identity.size() > 512)
        return QStringLiteral("Error: Cannot identify the Office renderer version");
    if (match.captured(1).toInt() < 25
        || (match.captured(1).toInt() == 25 && match.captured(2).toInt() < 2))
        return QStringLiteral("Error: Office rendering requires LibreOffice 25.2 or newer");
    const QString input = temporary.filePath(QStringLiteral("source.") + extension);
    if (!QFile::copy(file.fileName(), input))
        return QStringLiteral("Error: Cannot stage the Office renderer input");
    const auto rendered = run(
        program,
        {QStringLiteral("-env:UserInstallation=")
             + QUrl::fromLocalFile(profile).toString(QUrl::FullyEncoded),
         QStringLiteral("--headless"),
         QStringLiteral("--nologo"),
         QStringLiteral("--nodefault"),
         QStringLiteral("--norestore"),
         QStringLiteral("--convert-to"),
         QStringLiteral("pdf"),
         QStringLiteral("--outdir"),
         temporary.path(),
         input},
        16384,
        context,
        false,
        deadline,
        temporary.path());
    if (!rendered.error.isEmpty())
        return QStringLiteral("Error: %1").arg(rendered.error);
    QFile pdf(temporary.filePath(QStringLiteral("source.pdf")));
    if (!pdf.open(QIODevice::ReadOnly) || pdf.size() <= 0 || pdf.size() > kInputBytes)
        return QStringLiteral("Error: Office renderer did not produce a bounded PDF");
    const QByteArray bytes = pdf.read(kInputBytes + 1);
    if (bytes.size() != pdf.size() || !bytes.startsWith("%PDF-"))
        return QStringLiteral("Error: Office renderer produced an invalid PDF");
    const QString pdfHash       = digest(bytes);
    auto          imageOptions  = attachmentOptions;
    imageOptions.preserveSource = true;
    imageOptions.viewId = QStringLiteral("office:pdf=%1:page=%2").arg(pdfHash).arg(options.page);
    const auto backend
        = run(QStringLiteral("pdftoppm"), {QStringLiteral("-v")}, 4096, context, true, deadline);
    if (!backend.error.isEmpty())
        return QStringLiteral("Error: %1").arg(backend.error);
    const QString pdfBackend = QString::fromUtf8(backend.bytes).section('\n', 0, 0).simplified();
    if (!pdfBackend.startsWith(QStringLiteral("pdftoppm version ")) || pdfBackend.size() > 200)
        return QStringLiteral("Error: Cannot identify the derived PDF renderer version");
    QString    producedViewId;
    const auto image = readPdf(
        pdf.fileName(), source, options, llm, context, imageOptions, {}, deadline, &producedViewId);
    if (image.startsWith(QStringLiteral("Error:")))
        return image;
    const json provenance
        = {{"stage", "produced"},
           {"format", extension.toStdString()},
           {"view_kind", "office_derived_pdf"},
           {"source", source.toStdString()},
           {"source_sha256", attachmentOptions.sourceId.toStdString()},
           {"source_bytes", file.size()},
           {"renderer", "libreoffice"},
           {"renderer_version", identity.toStdString()},
           {"pdf_renderer", pdfBackend.toStdString()},
           {"derived_pdf_sha256", pdfHash.toStdString()},
           {"derived_pdf_bytes", bytes.size()},
           {"derived_page", options.page},
           {"view_id", producedViewId.toStdString()},
           {"notice",
            "Derived layout uses local fonts and renderer evaluation. Page numbers are PDF pages, "
            "not original XML parts. External link updates are disabled. Missing linked content, "
            "substituted fonts, reflow and formula values can differ from the original. Native "
            "text/media remain independent."}};
    return QString::fromStdString(provenance.dump()) + QLatin1Char('\n') + image;
}

QString readOffice(
    QFile                          &file,
    const QString                  &extension,
    const Options                  &options,
    QSocToolCallContext            *context,
    QLLMService                    *llm,
    const QSocImageAttach::Options &attachmentOptions,
    const QString                  &source,
    QDeadlineTimer                  deadline)
{
    if (options.mode == "image" && options.renderer.isEmpty())
        return QStringLiteral("Error: Office image mode requires explicit renderer=libreoffice");
    QString         error;
    QList<ZipEntry> entries;
    if (!archiveEntries(file, &entries, &error))
        return QStringLiteral("Error: %1").arg(error);
    const bool    media = options.mode == "media" || options.mode == "media_image";
    OfficeArchive archive(file.fileName(), std::move(entries), context, deadline);
    if (options.mode == "image")
        return renderOffice(
            file, extension, options, archive, context, llm, attachmentOptions, source, deadline);
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
    if (media)
        return readOfficeMedia(
            archive,
            extension,
            parts,
            options,
            source,
            file.size(),
            llm,
            context,
            attachmentOptions,
            deadline);
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
        "a normalized view rectangle from the source. mode=text_boxes returns bounded native word "
        "boxes with separate word-index continuation. OCR is not performed. "
        "Office media lists name original images and occurrence metadata. media_image explicitly "
        "selects one original. Office mode=image requires explicit renderer=libreoffice (25.2+), "
        "and selects derived PDF pages with source/derived digests. Native Office text never "
        "recalculates formulas. Derived pages can reflow or evaluate differently. "
        "Optional dependencies: Poppler "
        "pdfinfo/pdftotext/pdftoppm for "
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
            {"enum", {"text", "image", "list", "text_boxes", "media", "media_image"}},
            {"description",
             "text by default; image renders one PDF page or an explicit derived Office page; "
             "text_boxes returns one PDF page of "
             "native word boxes; list shows parts"}}},
          {"page",
           {{"type", "integer"},
            {"minimum", 1},
            {"description",
             "First page, slide or worksheet (1-based, default 1); native DOCX uses 1. Office "
             "renderer uses derived PDF pages"}}},
          {"page_count",
           {{"type", "integer"},
            {"minimum", 1},
            {"maximum", 5},
            {"description", "Number of parts, default 1; image and text_boxes require 1"}}},
          {"renderer",
           {{"type", "string"},
            {"enum", {"libreoffice"}},
            {"description",
             "Office image only: explicitly render a derived PDF locally with an optional "
             "installed LibreOffice. Page is a derived PDF page, not a worksheet or slide index"}}},
          {"region",
           {{"type", "object"},
            {"description",
             "PDF or rendered Office image: top-left normalized rectangle in the rotated CropBox "
             "view; requires "
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
             "Opaque next_cursor from one truncated part. Use the original text/text_boxes mode "
             "without page, "
             "page_count, offset or word_offset. The matching max_chars/max_words can change"}}},
          {"word_offset",
           {{"type", "integer"},
            {"minimum", 0},
            {"maximum", 1000000},
            {"description",
             "text_boxes only: source XML word index, including words outside the CropBox. Default "
             "0"}}},
          {"max_words",
           {{"type", "integer"},
            {"minimum", 1},
            {"maximum", 1000},
            {"description",
             "text_boxes only: source words per window, default 200. Outside-view words are "
             "counted and omitted"}}},
          {"media_offset",
           {{"type", "integer"},
            {"minimum", 0},
            {"maximum", 4096},
            {"description",
             "Office media mode: first unique media entry in the selected parts, default 0"}}},
          {"max_media",
           {{"type", "integer"},
            {"minimum", 1},
            {"maximum", 100},
            {"description", "Office media mode: entries per result, default 50"}}},
          {"media_id",
           {{"type", "string"},
            {"maxLength", 8192},
            {"description",
             "Required by media_image: archive part from a media list. Reads original bytes "
             "without occurrence crop/transform"}}},
          {"source_sha256",
           {{"type", "string"},
            {"description",
             "Optional media_image or rendered Office image guard: require the original source "
             "digest"}}},
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
    const QDeadlineTimer deadline(30000);
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
            deadline,
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
            else if (deadline.hasExpired())
                error = QStringLiteral("Document input deadline expired");
            return QStringLiteral("Error: %1").arg(error);
        }
    } else {
        path = QSocToolFileCore::localPath(requested, base, home, windows);
    }
    const QString extension = QFileInfo(requested).suffix().toLower();
    if (extension != "pdf" && extension != "docx" && extension != "xlsx" && extension != "pptx")
        return QStringLiteral("Error: read_document supports PDF, DOCX, XLSX and PPTX only");
    if (options.region && extension != "pdf" && options.renderer.isEmpty())
        return QStringLiteral("Error: region is supported only for PDF image mode");
    if (!options.renderer.isEmpty() && extension == "pdf")
        return QStringLiteral("Error: renderer applies only to Office image mode");
    if (options.mode == "text_boxes" && extension != "pdf")
        return QStringLiteral("Error: text_boxes is supported only for PDF");
    if ((options.mode == "media" || options.mode == "media_image") && extension == "pdf")
        return QStringLiteral("Error: media and media_image support Office files only");
    const QString canonical = remote_ ? path : QFileInfo(path).canonicalFilePath();
    const json    sourceKey
        = {{"transport", remote_ ? "sftp" : "local"},
           {"endpoint", remote_ ? remote_->endpointIdentity().toStdString() : std::string()},
           {"path",
            (remote_ ? canonical : QDir::cleanPath(QDir::fromNativeSeparators(canonical)))
                .toStdString()}};
    const QString sourceIdentity = digest(QByteArray::fromStdString(sourceKey.dump()));
    auto          snapshot
        = remote_ ? QSocInputSnapshot::fromRemote(
                        remote_->sftp(), path, kInputBytes, &error, cancelled, deadline)
                  : QSocInputSnapshot::fromLocal(path, kInputBytes, &error, cancelled, deadline);
    if (!snapshot)
        return QStringLiteral("Error: %1").arg(error);
    if (cancelled())
        return QStringLiteral("Error: Document read cancelled");
    if (!options.expectedSource.isEmpty()
        && options.expectedSource != QString::fromLatin1(snapshot->sha256()))
        return QStringLiteral("Error: Office source changed. Read the source again");
    QString backend;
    if (options.cursor) {
        const auto &cursor = *options.cursor;
        if (cursor.at("source") != sourceIdentity.toStdString())
            return QStringLiteral("Error: Document cursor belongs to a different source");
        if (cursor.at("sha256") != snapshot->sha256().toStdString())
            return QStringLiteral(
                "Error: Document source changed. Read the part again without cursor");
        if (cursor.at("format") != extension.toStdString()
            || cursor.at("stream") != (options.mode == "text_boxes" ? kBoxStream : kTextStream))
            return QStringLiteral("Error: Document cursor belongs to a different text stream");
        backend = extractorBackend(extension, context, &error, deadline);
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
        output = readPdf(
            snapshot->path(), path, options, llm_, context, attachmentOptions, backend, deadline);
    else {
        QFile file(snapshot->path());
        if (!file.open(QIODevice::ReadOnly))
            return QStringLiteral("Error: Cannot open input snapshot");
        output
            = readOffice(file, extension, options, context, llm_, attachmentOptions, path, deadline);
    }
    if (options.mode != "image" && options.mode != "media_image" && deadline.hasExpired())
        return QStringLiteral("Error: Document read deadline expired");
    if (output.startsWith(QLatin1Char('{'))) {
        auto result = json::parse(output.toStdString(), nullptr, false);
        if (result.is_object()) {
            result["source"]          = path.toStdString();
            result["source_sha256"]   = snapshot->sha256().toStdString();
            result["source_bytes"]    = snapshot->size();
            result["source_identity"] = sourceIdentity.toStdString();
            if (options.mode == "text_boxes") {
                const auto &part = result.at("parts").at(0);
                if (options.cursor
                    && options.cursor->at("part") != partIdentity(result, part).toStdString())
                    return QStringLiteral("Error: Document cursor belongs to a different part");
                addCursors(
                    &result,
                    sourceIdentity,
                    snapshot->sha256(),
                    QString::fromStdString(result.at("box_stream").at("backend").get<std::string>()),
                    true);
            }
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
                    backend = extractorBackend(extension, context, &error, deadline);
                if (cancelled())
                    return QStringLiteral("Error: Document read cancelled");
                if (deadline.hasExpired())
                    return QStringLiteral("Error: Document read deadline expired");
                if (!backend.isEmpty())
                    addCursors(&result, sourceIdentity, snapshot->sha256(), backend);
                else if (truncated)
                    result["cursor_unavailable"] = error.toStdString();
            }
            const auto serialized = result.dump(2);
            if ((options.mode == "media" || options.mode == "media_image")
                && serialized.size() > 1024 * 1024)
                return QStringLiteral(
                    "Error: Office media result exceeds 1 MiB. Select fewer parts or reduce "
                    "max_media");
            if (options.mode == "text_boxes" && serialized.size() > kBoxResultBytes)
                return QStringLiteral("Error: PDF word-box result exceeds 1 MiB. Reduce max_words");
            return QString::fromStdString(serialized);
        }
    }
    return output;
}

#include "moc_qsoctooldocument.cpp"
