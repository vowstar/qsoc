// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 Huang Rui <vowstar@gmail.com>

#include "agent/tool/qsoctoolfile.h"

#include "agent/qsocfilehistory.h"
#include "agent/tool/qsoctoolfilecore.h"
#include "common/qlspservice.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>

#include <optional>

namespace {

namespace Core = QSocToolFileCore;

#ifdef Q_OS_WIN
constexpr bool kWindows = true;
#else
constexpr bool kWindows = false;
#endif

const QString kWriteScope = QStringLiteral(
    "an allowed directory (project, working, user dirs, or temp)");

/* The home `~` names in bash: $HOME when set (Git Bash on Windows sets it),
 * else the platform home directory. */
QString bashHome()
{
    const QString home = qEnvironmentVariable("HOME");
    return home.isEmpty() ? QDir::homePath() : QDir::fromNativeSeparators(home);
}

/* A path argument as the local machine names it: relative to the working
 * directory bash runs in, `~` the local home. */
QString resolvePath(const QSocPathContext *pathContext, const QString &raw)
{
    QString base = pathContext != nullptr ? pathContext->getWorkingDir() : QString();
    if (base.isEmpty() && pathContext != nullptr) {
        base = pathContext->getProjectDir();
    }
    if (base.isEmpty()) {
        base = QDir::currentPath();
    }
    return Core::localPath(raw, base, bashHome(), kWindows);
}

/* Stream @p path into @p reader; false when the file could not be read. */
bool streamFile(const QString &path, Core::Reader *reader)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }
    constexpr qint64 kChunk = 64 * 1024;
    while (!file.atEnd()) {
        const QByteArray chunk = file.read(kChunk);
        if (file.error() != QFileDevice::NoError) {
            return false;
        }
        if (!reader->feed(chunk)) {
            return true;
        }
    }
    reader->finish();
    return file.error() == QFileDevice::NoError;
}

bool listLocalDir(const QString &dir, QList<Core::ListEntry> *entries, QString *error)
{
    const QFileInfo info(dir);
    if (!info.isDir() || !info.isReadable()) {
        *error = QStringLiteral("cannot read directory");
        return false;
    }
    const QDir::Filters filters = QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot | QDir::Hidden;
    for (const QFileInfo &entry : QDir(dir).entryInfoList(filters, QDir::Unsorted)) {
        entries->append(
            {.name        = entry.fileName(),
             .isDirectory = entry.isDir(),
             .isSymlink   = entry.isSymLink(),
             .hidden      = entry.isHidden()});
    }
    return true;
}

/* The whole file, or nullopt when it cannot be read completely. */
std::optional<QByteArray> readWhole(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return std::nullopt;
    }
    QByteArray bytes = file.readAll();
    if (file.error() != QFileDevice::NoError) {
        return std::nullopt;
    }
    return bytes;
}

/* Write @p bytes atomically; the error text, or empty on success. */
QString saveFile(const QString &path, const QByteArray &bytes)
{
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size()) {
        file.cancelWriting();
        return QStringLiteral("Error: Cannot open file for writing: %1").arg(path);
    }
    if (!file.commit()) {
        return QStringLiteral("Error: Cannot finish writing file: %1").arg(path);
    }
    return {};
}

/* The writable spelling of @p raw, or the refusal. */
QString resolveWritable(QSocPathContext *pathContext, const QString &raw, QString *path)
{
    *path = resolvePath(pathContext, raw);
    if (pathContext != nullptr && !pathContext->resolveWritablePath(*path, path)) {
        return QStringLiteral(
            "Error: Access denied. File must be within an allowed directory "
            "(project, working, user, or temp).");
    }
    return {};
}

} // namespace

/* QSocToolFileRead Implementation */

QSocToolFileRead::QSocToolFileRead(QObject *parent, QSocPathContext *pathContext, QLLMService *llm)
    : QSocTool(parent)
    , pathContext(pathContext)
    , llmService(llm)
{}

QSocToolFileRead::~QSocToolFileRead() = default;

QString QSocToolFileRead::getName() const
{
    return "read_file";
}

QString QSocToolFileRead::getDescription() const
{
    return Core::readDescription(llmService != nullptr && llmService->currentSupportsImage());
}

json QSocToolFileRead::getParametersSchema() const
{
    return Core::readSchema();
}

QString QSocToolFileRead::execute(const json &arguments)
{
    if (!arguments.contains("file_path") || !arguments["file_path"].is_string()) {
        return "Error: file_path is required";
    }

    QString filePath
        = resolvePath(pathContext, QString::fromStdString(arguments["file_path"].get<std::string>()));
    /* Canonicalize so read_file / edit_file / write_file key the read-state
     * on one path spelling (./x, x, a/../x all fold together). */
    QFileInfo fileInfo(filePath);
    if (!fileInfo.canonicalFilePath().isEmpty()) {
        filePath = fileInfo.canonicalFilePath();
        fileInfo = QFileInfo(filePath);
    }
    if (!fileInfo.exists()) {
        return Core::fileNotFound(filePath);
    }
    if (!fileInfo.isFile()) {
        return Core::notAFile(filePath);
    }

    Core::Reader reader(Core::readWindow(arguments));
    if (!streamFile(filePath, &reader)) {
        return QString("Error: Cannot read file completely: %1").arg(filePath);
    }

    /* Record a full read so edit_file / write_file can enforce
     * read-before-edit and detect on-disk changes. Partial / offset reads
     * do not qualify because the agent has not seen the entire file. */
    if (const auto whole = reader.wholeFile(); pathContext && whole) {
        pathContext->readState().recordRead(filePath, QString::fromUtf8(*whole));
    }
    return reader.result(filePath, llmService);
}

void QSocToolFileRead::setPathContext(QSocPathContext *pathContext)
{
    this->pathContext = pathContext;
}

void QSocToolFileRead::setLLMService(QLLMService *llm)
{
    llmService = llm;
}

/* QSocToolFileList Implementation */

QSocToolFileList::QSocToolFileList(QObject *parent, QSocPathContext *pathContext)
    : QSocTool(parent)
    , pathContext(pathContext)
{}

QSocToolFileList::~QSocToolFileList() = default;

QString QSocToolFileList::getName() const
{
    return "list_files";
}

QString QSocToolFileList::getDescription() const
{
    return Core::listDescription();
}

json QSocToolFileList::getParametersSchema() const
{
    return Core::listSchema();
}

QString QSocToolFileList::execute(const json &arguments)
{
    const Core::ListCall call    = Core::listCall(arguments);
    const QString        dirPath = resolvePath(pathContext, call.directory);
    if (!QFileInfo(dirPath).isDir()) {
        return Core::directoryNotFound(dirPath);
    }
    return Core::listFiles(dirPath, call, !kWindows, listLocalDir);
}

void QSocToolFileList::setPathContext(QSocPathContext *pathContext)
{
    this->pathContext = pathContext;
}

/* QSocToolFileWrite Implementation */

QSocToolFileWrite::QSocToolFileWrite(QObject *parent, QSocPathContext *pathContext)
    : QSocTool(parent)
    , pathContext(pathContext)
{}

QSocToolFileWrite::~QSocToolFileWrite() = default;

QString QSocToolFileWrite::getName() const
{
    return "write_file";
}

QString QSocToolFileWrite::getDescription() const
{
    return Core::writeDescription(kWriteScope);
}

json QSocToolFileWrite::getParametersSchema() const
{
    return Core::writeSchema();
}

QString QSocToolFileWrite::execute(const json &arguments)
{
    const Core::WriteCall call = Core::writeCall(arguments);
    if (!call.error.isEmpty()) {
        return call.error;
    }
    QString filePath;
    if (const QString refusal = resolveWritable(pathContext, call.path, &filePath);
        !refusal.isEmpty()) {
        return refusal;
    }
    const QFileInfo fileInfo(filePath);

    /* Read-before-overwrite + stale guard for EXISTING files: write_file
     * replaces the whole file, so it must not clobber content the agent
     * never read or a concurrent change. New files need no prior read. */
    const bool existedBefore = fileInfo.exists() && fileInfo.isFile();
    QString    beforeContent;
    if (existedBefore) {
        const auto bytes = readWhole(filePath);
        if (!bytes) {
            return QString("Error: Cannot read file completely before overwrite: %1").arg(filePath);
        }
        beforeContent = QString::fromUtf8(*bytes);
        if (pathContext && !pathContext->readState().wasRead(filePath)) {
            return Core::notReadYet(filePath, QStringLiteral("overwriting"));
        }
        if (pathContext && pathContext->readState().changedSinceRead(filePath, beforeContent)) {
            return Core::changedSinceRead(filePath, QStringLiteral("overwriting"));
        }
    }

    /* Pre-edit snapshot: capture the current content (or mark absent)
     * BEFORE the overwrite so rewind can restore it later. */
    if (fileHistory != nullptr && fileHistory->isPathInScope(filePath)
        && !fileHistory->trackEdit(filePath, existedBefore, beforeContent)) {
        return QString("Error: Cannot save file history before writing: %1").arg(filePath);
    }

    QDir parentDir = fileInfo.absoluteDir();
    if (!parentDir.exists() && !parentDir.mkpath(".")) {
        return QString("Error: Cannot create directory: %1").arg(parentDir.absolutePath());
    }

    const QByteArray bytes = call.content.toUtf8();
    if (const QString failure = saveFile(filePath, bytes); !failure.isEmpty()) {
        return failure;
    }

    /* The written content is now the agent's known state, so a follow-up
     * edit / overwrite in the same session needs no re-read. */
    if (pathContext) {
        pathContext->readState().recordRead(filePath, call.content);
    }

    /* Notify LSP service about the file change. */
    (lspService ? lspService : QLspService::instance())->didSave(filePath);

    return Core::wroteText(bytes.size(), filePath);
}

void QSocToolFileWrite::setPathContext(QSocPathContext *pathContext)
{
    this->pathContext = pathContext;
}

void QSocToolFileWrite::setFileHistory(QSocFileHistory *history)
{
    this->fileHistory = history;
}

/* QSocToolFileEdit Implementation */

QSocToolFileEdit::QSocToolFileEdit(QObject *parent, QSocPathContext *pathContext)
    : QSocTool(parent)
    , pathContext(pathContext)
{}

QSocToolFileEdit::~QSocToolFileEdit() = default;

QString QSocToolFileEdit::getName() const
{
    return "edit_file";
}

QString QSocToolFileEdit::getDescription() const
{
    return Core::editDescription(kWriteScope);
}

json QSocToolFileEdit::getParametersSchema() const
{
    return Core::editSchema();
}

QString QSocToolFileEdit::execute(const json &arguments)
{
    Core::EditCall call = Core::editCall(arguments);
    if (!call.error.isEmpty()) {
        return call.error;
    }
    QString filePath;
    if (const QString refusal = resolveWritable(pathContext, call.path, &filePath);
        !refusal.isEmpty()) {
        return refusal;
    }
    call.path = filePath;
    const QFileInfo fileInfo(filePath);
    if (!fileInfo.exists() || !fileInfo.isFile()) {
        return Core::fileNotFound(filePath);
    }

    const auto bytes = readWhole(filePath);
    if (!bytes) {
        return QString("Error: Cannot read file completely: %1").arg(filePath);
    }
    const QString content = QString::fromUtf8(*bytes);

    /* Read-before-edit + stale-on-disk guard: the agent must have read this
     * exact file via read_file first, and it must not have changed on disk
     * since, so an edit never blindly clobbers content the agent never saw
     * or a concurrent modification. */
    if (pathContext && !pathContext->readState().wasRead(filePath)) {
        return Core::notReadYet(filePath, QStringLiteral("editing"));
    }
    if (pathContext && pathContext->readState().changedSinceRead(filePath, content)) {
        return Core::changedSinceRead(filePath, QStringLiteral("editing"));
    }

    const Core::EditOutcome edit = Core::applyEdit(content, call);
    if (!edit.error.isEmpty()) {
        return edit.error;
    }

    /* Pre-edit snapshot: we just finished reading the original content,
     * so it's the exact pre-mutation state the history store needs. */
    if (fileHistory != nullptr && fileHistory->isPathInScope(filePath)
        && !fileHistory->trackEdit(filePath, true, content)) {
        return QString("Error: Cannot save file history before editing: %1").arg(filePath);
    }

    if (const QString failure = saveFile(filePath, edit.content.toUtf8()); !failure.isEmpty()) {
        return failure;
    }

    /* The agent now knows the post-edit content, so a follow-up edit in the
     * same session is allowed without a re-read. */
    if (pathContext) {
        pathContext->readState().recordRead(filePath, edit.content);
    }

    /* Notify LSP service about the file change. */
    (lspService ? lspService : QLspService::instance())->didSave(filePath);

    return Core::editedText(filePath, edit.count);
}

void QSocToolFileEdit::setPathContext(QSocPathContext *pathContext)
{
    this->pathContext = pathContext;
}

void QSocToolFileEdit::setFileHistory(QSocFileHistory *history)
{
    this->fileHistory = history;
}

#include "moc_qsoctoolfile.cpp"
