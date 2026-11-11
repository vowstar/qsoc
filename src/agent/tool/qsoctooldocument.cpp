// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/tool/qsoctooldocument.h"

#include "agent/tool/qsoctoolfilecore.h"
#include "agent/tool/qsoctoolpath.h"
#include "common/qllmservice.h"
#include "common/qsocimageattach.h"

#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTimer>
#include <QXmlStreamReader>
#include <QtEndian>

#include <algorithm>

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

struct CommandResult
{
    QByteArray bytes;
    QString    error;
};

CommandResult run(
    const QString &program, const QStringList &arguments, qint64 limit, QSocToolCallContext *context)
{
    const QString executable = QStandardPaths::findExecutable(program);
    if (executable.isEmpty())
        return {{}, QStringLiteral("Missing optional dependency: %1").arg(program)};
    if (context && context->isCancellationRequested())
        return {{}, QStringLiteral("Document read cancelled")};
    QProcess process;
    auto     environment = QProcessEnvironment::systemEnvironment();
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
        process.setReadChannel(QProcess::StandardError);
        diagnostics += process.read(qMax<qint64>(0, 16384 - diagnostics.size()) + 1);
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
    int     page     = 1;
    int     count    = 1;
    int     offset   = 0;
    int     maxChars = 20000;
    QString mode     = QStringLiteral("text");
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
    return options->mode == "text" || options->mode == "image" || options->mode == "list"
               ? QString()
               : QStringLiteral("mode must be text, image, or list");
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

QString readPdf(
    const QString &path, const Options &options, QLLMService *llm, QSocToolCallContext *context)
{
    const QPointer<QLLMService> model(llm);
    const auto                  info = run(QStringLiteral("pdfinfo"), {path}, 65536, context);
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
    const QString page = QString::number(options.page);
    if (options.mode == "image") {
        if (options.count != 1)
            return QStringLiteral("Error: PDF image mode renders one page per call");
        const auto image = run(
            QStringLiteral("pdftoppm"),
            {QStringLiteral("-f"),
             page,
             QStringLiteral("-singlefile"),
             QStringLiteral("-scale-to"),
             QStringLiteral("1600"),
             QStringLiteral("-png"),
             path},
            16 * 1024 * 1024,
            context);
        if (!image.error.isEmpty())
            return QStringLiteral("Error: %1").arg(image.error);
        return QStringLiteral("PDF page %1 of %2\n").arg(options.page).arg(total)
               + QSocImageAttach::buildAttachmentResult(
                   QStringLiteral("%1#page=%2").arg(path).arg(options.page),
                   QStringLiteral("image/png"),
                   image.bytes,
                   model,
                   true);
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

QSocToolDocumentRead::QSocToolDocumentRead(QObject *parent, QSocPathContext *paths, QLLMService *llm)
    : QSocTool(parent)
    , paths_(paths)
    , llm_(llm)
{}

QString QSocToolDocumentRead::getName() const
{
    return QStringLiteral("read_document");
}

QString QSocToolDocumentRead::getDescription() const
{
    return QStringLiteral(
        "Read local PDF, DOCX, XLSX or PPTX documents with source locations. page/page_count "
        "select PDF pages, spreadsheet sheets or presentation slides; DOCX has one document body. "
        "mode=image renders one PDF page for diagrams or scanned pages; OCR is not performed. "
        "Office supports text only. Optional dependencies: Poppler pdfinfo/pdftotext/pdftoppm for "
        "PDF, unzip for Office. Inputs are limited to 32 MiB and Office expanded entries to 8 MiB "
        "each/64 MiB total. Not offered in remote workspaces.");
}

json QSocToolDocumentRead::getParametersSchema() const
{
    return {
        {"type", "object"},
        {"properties",
         {{"file_path",
           {{"type", "string"},
            {"description", "Local document path, relative to the working directory or absolute"}}},
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
          {"offset",
           {{"type", "integer"},
            {"minimum", 0},
            {"description",
             "Text offset within each part, default 0; use next_offset to continue truncated "
             "text"}}},
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
    QString base = paths_ ? paths_->getWorkingDir() : QString();
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
    const QString path = QSocToolFileCore::localPath(
        QString::fromStdString(arguments["file_path"].get<std::string>()), base, home, windows);
    QFile file(path);
    if (!QFileInfo(path).isFile() || !file.open(QIODevice::ReadOnly))
        return QStringLiteral("Error: Cannot read document: %1").arg(path);
    if (file.size() > kInputBytes)
        return QStringLiteral("Error: Document exceeds the 32 MiB input limit");
    const QString extension = QFileInfo(path).suffix().toLower();
    if (extension == "pdf")
        return readPdf(path, options, llm_, currentCallContext());
    if (extension == "docx" || extension == "xlsx" || extension == "pptx")
        return readOffice(file, extension, options, currentCallContext());
    return QStringLiteral("Error: read_document supports PDF, DOCX, XLSX and PPTX only");
}

#include "moc_qsoctooldocument.cpp"
