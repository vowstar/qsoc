// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/tool/qsoctoolwaveform.h"

#include "agent/tool/qsoctoolfilecore.h"
#include "agent/tool/qsoctoolpath.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QRegularExpression>

#include <algorithm>
#include <cmath>
#include <limits>

namespace {

constexpr qint64    kScanLimit   = 128 * 1024 * 1024;
constexpr int       kMaxSignals  = 50000;
constexpr int       kMaxSelected = 32;
constexpr qsizetype kOutputLimit = 1024 * 1024;

class Tokens
{
public:
    explicit Tokens(QFile &file)
        : file_(file)
    {
        timer_.start();
    }

    QByteArray next()
    {
        QByteArray token;
        while (error.isEmpty()) {
            if (position_ >= buffer_.size()) {
                if (file_.atEnd())
                    return token;
                if (bytes_ >= kScanLimit || timer_.elapsed() > 5000) {
                    error = QStringLiteral("VCD scan limit reached; use a smaller waveform file");
                    return {};
                }
                buffer_ = file_.read(qMin<qint64>(65536, kScanLimit - bytes_));
                bytes_ += buffer_.size();
                position_ = 0;
                if (file_.error() != QFileDevice::NoError || buffer_.isEmpty()) {
                    error = QStringLiteral("Cannot read VCD input");
                    return {};
                }
            }
            const char ch = buffer_.at(position_++);
            if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') {
                if (!token.isEmpty())
                    return token;
                continue;
            }
            token += ch;
            if (token.size() > 65536) {
                error = QStringLiteral("VCD token exceeds 64 KiB");
            }
        }
        return {};
    }

    QString error;

private:
    QFile        &file_;
    QByteArray    buffer_;
    qsizetype     position_ = 0;
    qint64        bytes_    = 0;
    QElapsedTimer timer_;
};

struct Signal
{
    QString    name;
    QString    baseName;
    QByteArray id;
    QString    type;
    int        width = 0;
};

bool unsignedArgument(const json &arguments, const char *name, quint64 fallback, quint64 *value)
{
    const auto field = arguments.find(name);
    *value           = fallback;
    if (field == arguments.end())
        return true;
    if (!field->is_number_integer())
        return false;
    if (field->is_number_unsigned()) {
        *value = field->get<quint64>();
        return true;
    }
    const auto number = field->get<qint64>();
    if (number < 0)
        return false;
    *value = static_cast<quint64>(number);
    return true;
}

bool readHeader(Tokens &tokens, QList<Signal> *declarations, QString *timescale, QString *error)
{
    QStringList scope;
    qsizetype   declarationBytes = 0;
    for (QByteArray token = tokens.next(); !token.isEmpty(); token = tokens.next()) {
        if (!token.startsWith('$')) {
            *error = QStringLiteral("Expected a VCD header directive");
            return false;
        }
        QList<QByteArray> fields;
        qsizetype         fieldBytes = 0;
        QByteArray        field;
        while (!(field = tokens.next()).isEmpty() && field != "$end") {
            fields.append(field);
            fieldBytes += field.size();
            if (fields.size() > 1024 || fieldBytes > 65536) {
                *error = QStringLiteral("VCD header directive exceeds its size limit");
                return false;
            }
        }
        if (field != "$end")
            break;
        if (token == "$enddefinitions")
            return true;
        if (token == "$scope" && fields.size() >= 2) {
            if (scope.size() >= 64 || fields.at(1).size() > 256) {
                *error = QStringLiteral("VCD scope exceeds its depth or name limit");
                return false;
            }
            scope.append(QString::fromUtf8(fields.at(1)));
        } else if (token == "$upscope") {
            if (scope.isEmpty()) {
                *error = QStringLiteral("Unbalanced VCD scope");
                return false;
            }
            scope.removeLast();
        } else if (token == "$timescale") {
            timescale->clear();
            for (const auto &part : fields)
                *timescale += QString::fromLatin1(part);
        } else if (token == "$var") {
            bool      ok    = false;
            const int width = fields.size() >= 4 ? fields.at(1).toInt(&ok) : 0;
            if (!ok || width <= 0 || width > 65536 || declarations->size() >= kMaxSignals) {
                *error = QStringLiteral("Invalid VCD variable or more than 50000 declarations");
                return false;
            }
            const QString prefix   = scope.isEmpty() ? QString() : scope.join('.') + '.';
            const QString baseName = prefix + QString::fromUtf8(fields.at(3));
            QString       range;
            for (qsizetype index = 4; index < fields.size(); ++index)
                range += QString::fromLatin1(fields.at(index));
            static const QRegularExpression rangePattern(
                QStringLiteral("^\\[[+-]?[0-9]+(?::[+-]?[0-9]+)?\\]$"));
            if (!range.isEmpty() && !rangePattern.match(range).hasMatch()) {
                *error = QStringLiteral("Invalid VCD reference range");
                return false;
            }
            const QString name = baseName + range;
            declarationBytes += (name.size() + baseName.size()) * 2 + fields.at(2).size();
            if (name.size() > 4096 || declarationBytes > 8 * 1024 * 1024) {
                *error = QStringLiteral("VCD declarations exceed the 8 MiB name limit");
                return false;
            }
            declarations->append(
                {name, baseName, fields.at(2), QString::fromLatin1(fields.at(0)), width});
        }
    }
    *error = tokens.error.isEmpty() ? QStringLiteral("VCD enddefinitions is missing")
                                    : tokens.error;
    return false;
}

QList<Signal> selectSignals(const QList<Signal> &all, const json &arguments, QString *error)
{
    if (!arguments.contains("signals"))
        return {};
    const auto &names = arguments["signals"];
    if (!names.is_array() || names.empty() || names.size() > kMaxSelected) {
        *error = QStringLiteral("signals must contain 1 to 32 signal names or glob patterns");
        return {};
    }
    struct Pattern
    {
        QString            text;
        QRegularExpression regex;
        bool               exact = false;
    };
    QSet<QString>           exactNames;
    QHash<QString, QString> baseAliases;
    for (const auto &signal : all) {
        exactNames.insert(signal.name);
        auto alias = baseAliases.find(signal.baseName);
        if (alias == baseAliases.end())
            baseAliases.insert(signal.baseName, signal.name);
        else if (alias.value() != signal.name)
            alias.value().clear();
    }
    QList<Pattern> patterns;
    for (const auto &name : names) {
        if (!name.is_string()) {
            *error = QStringLiteral("Each signal request must be a string");
            return {};
        }
        QString    literal = QString::fromStdString(name.get<std::string>());
        bool       exact   = exactNames.contains(literal);
        const auto alias   = baseAliases.constFind(literal);
        if (!exact && alias != baseAliases.cend()) {
            if (alias.value().isEmpty()) {
                *error = QStringLiteral(
                    "Signal base name is ambiguous; select a complete indexed or ranged name from "
                    "the signal list");
                return {};
            }
            literal = alias.value();
            exact   = true;
        }
        if (!exact && name.get_ref<const std::string &>().size() > 256) {
            *error = QStringLiteral(
                "Signal glob patterns must be at most 256 bytes; listed exact names may be longer");
            return {};
        }
        patterns.append(
            {literal,
             exact ? QRegularExpression()
                   : QRegularExpression(QRegularExpression::wildcardToRegularExpression(literal)),
             exact});
    }
    QList<Signal> selected;
    for (const auto &signal : all) {
        for (const auto &pattern : patterns) {
            if (pattern.exact ? pattern.text == signal.name
                              : pattern.regex.match(signal.name).hasMatch()) {
                selected.append(signal);
                break;
            }
        }
        if (selected.size() > kMaxSelected) {
            *error = QStringLiteral("More than 32 signals matched; narrow the signal patterns");
            return {};
        }
    }
    if (selected.isEmpty())
        *error = QStringLiteral("No VCD signals match the requested patterns");
    return selected;
}

QString queryVcd(QFile &file, const json &arguments)
{
    quint64 start        = 0;
    quint64 end          = 0;
    quint64 maxEvents    = 0;
    quint64 signalOffset = 0;
    if (!unsignedArgument(arguments, "start_time", 0, &start)
        || !unsignedArgument(arguments, "end_time", std::numeric_limits<quint64>::max(), &end)
        || !unsignedArgument(arguments, "max_events", 200, &maxEvents)
        || !unsignedArgument(arguments, "signal_offset", 0, &signalOffset) || end < start
        || maxEvents == 0 || maxEvents > 2000) {
        return QStringLiteral(
            "Error: Use nonnegative time ticks, end_time >= start_time, and max_events from 1 to "
            "2000");
    }
    Tokens        tokens(file);
    QList<Signal> all;
    QString       timescale;
    QString       error;
    if (!readHeader(tokens, &all, &timescale, &error))
        return QStringLiteral("Error: %1").arg(error);
    const auto selected = selectSignals(all, arguments, &error);
    if (!error.isEmpty())
        return QStringLiteral("Error: %1").arg(error);
    json result
        = {{"format", "vcd"},
           {"timescale", timescale.toStdString()},
           {"time_unit", "ticks"},
           {"signals", json::array()}};
    const bool  listOnly    = !arguments.contains("signals");
    const auto &listed      = listOnly ? all : selected;
    qsizetype   signalBytes = static_cast<qsizetype>(result.dump().size()) + 256;
    for (qsizetype index = 0; index < listed.size(); ++index) {
        if (listOnly && static_cast<quint64>(index) < signalOffset)
            continue;
        const auto &signal = listed.at(index);
        if (result["signals"].size() == 200)
            break;
        json entry
            = {{"name", signal.name.toStdString()},
               {"width", signal.width},
               {"type", signal.type.toStdString()}};
        const qsizetype entryBytes = static_cast<qsizetype>(entry.dump().size()) + 1;
        if (listOnly && signalBytes + entryBytes > kOutputLimit)
            break;
        signalBytes += entryBytes;
        result["signals"].push_back(std::move(entry));
    }
    if (listOnly) {
        result["total_signals"] = all.size();
        result["signal_offset"] = signalOffset;
        const quint64 next      = signalOffset + result["signals"].size();
        result["truncated"]     = next < static_cast<quint64>(all.size());
        if (result["truncated"] == true)
            result["next_signal_offset"] = next;
        return QString::fromStdString(result.dump());
    }
    QHash<QByteArray, QList<Signal>> byId;
    for (const auto &signal : selected)
        byId[signal.id].append(signal);
    QHash<QByteArray, QByteArray> previous;
    result["start_time"] = start;
    if (arguments.contains("end_time"))
        result["end_time"] = end;
    result["values_at_start"] = json::object();
    for (const auto &signal : selected)
        result["values_at_start"][signal.name.toStdString()] = nullptr;
    result["events"]      = json::array();
    quint64   time        = 0;
    bool      truncated   = false;
    qsizetype outputBytes = static_cast<qsizetype>(result.dump().size()) + 256;
    if (outputBytes > kOutputLimit)
        return QStringLiteral("Error: VCD metadata exceeds the 1 MiB output limit");
    for (QByteArray token = tokens.next(); !token.isEmpty(); token = tokens.next()) {
        if (token == "$comment") {
            QByteArray part;
            while (!(part = tokens.next()).isEmpty() && part != "$end") {
            }
            if (part.isEmpty() && tokens.error.isEmpty())
                return QStringLiteral("Error: Unterminated VCD comment");
            continue;
        }
        if (token.startsWith('$'))
            continue;
        if (token.startsWith('#')) {
            bool          ok   = false;
            const quint64 next = token.mid(1).toULongLong(&ok);
            if (!ok || next < time)
                return QStringLiteral("Error: Invalid or decreasing VCD timestamp");
            time = next;
            if (time > end)
                break;
            continue;
        }
        QByteArray value;
        QByteArray id;
        const bool realValue = token.startsWith('r') || token.startsWith('R');
        if (token.startsWith('b') || token.startsWith('B') || token.startsWith('r')
            || token.startsWith('R')) {
            value = token.mid(1).toLower();
            id    = tokens.next();
        } else if (token.size() >= 2 && QByteArray("01xXzZ").contains(token.at(0))) {
            value = token.left(1).toLower();
            id    = token.mid(1);
        } else {
            return QStringLiteral("Error: Invalid VCD value change");
        }
        bool validValue = !value.isEmpty() && !id.isEmpty();
        if (realValue) {
            bool         numberOk = false;
            const double number   = value.toDouble(&numberOk);
            validValue            = validValue && numberOk && std::isfinite(number);
        } else {
            validValue = validValue && std::all_of(value.cbegin(), value.cend(), [](char bit) {
                             return bit == '0' || bit == '1' || bit == 'x' || bit == 'z';
                         });
        }
        if (!validValue)
            return QStringLiteral("Error: Invalid VCD value change");
        if (!byId.contains(id))
            continue;
        const QByteArray old = previous.value(id);
        previous[id]         = value;
        for (const auto &signal : byId.value(id)) {
            const auto name = signal.name.toStdString();
            if (time <= start) {
                auto           &initial   = result["values_at_start"][name];
                const json      nextValue = value.toStdString();
                const qsizetype delta     = static_cast<qsizetype>(nextValue.dump().size())
                                            - static_cast<qsizetype>(initial.dump().size());
                if (outputBytes + delta > kOutputLimit) {
                    truncated = true;
                    break;
                }
                outputBytes += delta;
                initial = nextValue;
            }
            if (time < start || value == old)
                continue;
            if (result["events"].size() >= maxEvents) {
                truncated = true;
                break;
            }
            const bool  digitalBit = signal.width == 1 && signal.type != "real"
                                     && signal.type != "realtime" && !realValue;
            const char *edge       = digitalBit && old == "0" && value == "1"   ? "rising"
                                     : digitalBit && old == "1" && value == "0" ? "falling"
                                                                                : "change";
            json        event
                = {{"time", time}, {"signal", name}, {"value", value.toStdString()}, {"edge", edge}};
            const qsizetype eventBytes = static_cast<qsizetype>(event.dump().size()) + 1;
            if (outputBytes + eventBytes > kOutputLimit) {
                truncated = true;
                break;
            }
            outputBytes += eventBytes;
            result["events"].push_back(std::move(event));
        }
        if (truncated)
            break;
    }
    if (!tokens.error.isEmpty()) {
        result["scan_error"] = tokens.error.toStdString();
        truncated            = true;
    }
    result["truncated"]       = truncated;
    result["window_complete"] = !truncated;
    result["last_time"]       = time;
    return QString::fromStdString(result.dump());
}

} // namespace

QSocToolWaveformRead::QSocToolWaveformRead(QObject *parent, QSocPathContext *paths)
    : QSocTool(parent)
    , paths_(paths)
{}

QString QSocToolWaveformRead::getName() const
{
    return QStringLiteral("read_waveform");
}

QString QSocToolWaveformRead::getDescription() const
{
    return QStringLiteral(
        "Read a local VCD waveform. Omit signals to list hierarchical signal names; select up to "
        "32 names or globs to query values and rising/falling edges. Times are raw ticks in the "
        "file's timescale. Streams at most 128 MiB or 5 seconds. Not offered in remote workspaces. "
        "Output is limited to 1 MiB, including initial values and events. FST and analog waveforms "
        "are not supported.");
}

json QSocToolWaveformRead::getParametersSchema() const
{
    return {
        {"type", "object"},
        {"properties",
         {{"file_path",
           {{"type", "string"},
            {"description", "Local VCD path, relative to the working directory or absolute"}}},
          {"signals",
           {{"type", "array"},
            {"items", {{"type", "string"}}},
            {"minItems", 1},
            {"maxItems", 32},
            {"description", "Hierarchical signal names or glob patterns; omit to list signals"}}},
          {"start_time",
           {{"type", "integer"},
            {"minimum", 0},
            {"description", "Inclusive start in VCD ticks, default 0"}}},
          {"end_time",
           {{"type", "integer"}, {"minimum", 0}, {"description", "Inclusive end in VCD ticks"}}},
          {"max_events",
           {{"type", "integer"},
            {"minimum", 1},
            {"maximum", 2000},
            {"description", "Maximum returned events, default 200"}}},
          {"signal_offset",
           {{"type", "integer"},
            {"minimum", 0},
            {"description", "Signal-list offset, default 0; use next_signal_offset to continue"}}}}},
        {"required", json::array({"file_path"})}};
}

QString QSocToolWaveformRead::execute(const json &arguments)
{
    if (!arguments.contains("file_path") || !arguments["file_path"].is_string())
        return QStringLiteral("Error: file_path is required");
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
    if (QFileInfo(path).suffix().compare(QStringLiteral("vcd"), Qt::CaseInsensitive) != 0)
        return QStringLiteral("Error: read_waveform supports VCD files only");
    QFile file(path);
    if (!QFileInfo(path).isFile() || !file.open(QIODevice::ReadOnly))
        return QStringLiteral("Error: Cannot read waveform: %1").arg(path);
    return queryVcd(file, arguments);
}

#include "moc_qsoctoolwaveform.cpp"
