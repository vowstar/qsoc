// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCTOOLFILECORE_H
#define QSOCTOOLFILECORE_H

#include <nlohmann/json.hpp>

#include <functional>
#include <optional>
#include <QByteArray>
#include <QList>
#include <QString>

class QLLMService;

/**
 * @brief What read_file, list_files, write_file and edit_file share between
 *        the local machine and a remote host.
 * @details Schemas, descriptions, result and error texts, the line-window
 *          reader, the listing walk and the path spelling rules. A tool only
 *          supplies the transport, so the same call means the same thing on
 *          either side.
 */
namespace QSocToolFileCore {

using json = nlohmann::json;

/** @brief Most bytes one read_file call keeps: an image, or a text window. */
constexpr qsizetype kReadBytesLimit = 16 * 1024 * 1024;

/** @brief Lines read_file returns when the call names no `max_lines`. */
constexpr int kDefaultMaxLines = 500;

/** @brief Entries list_files returns when the call names no `limit`. */
constexpr int kDefaultListLimit = 1000;

/* Schemas and descriptions */

json readSchema();
json listSchema();
json writeSchema();
json editSchema();

/** @brief read_file description; @p image when the model takes images. */
QString readDescription(bool image);
QString listDescription();

/** @param scope The directories a write may land in, as a short phrase. */
QString writeDescription(const QString &scope);
QString editDescription(const QString &scope);

/* Paths */

/**
 * @brief Replace a leading `~` or `~/` with @p home.
 * @details `~user` is left alone. An empty @p home leaves the path alone.
 */
QString expandHome(const QString &path, const QString &home);

/**
 * @brief A local file tool's path argument as an absolute path.
 * @details `~` is @p home; a relative path joins @p base. With @p windows,
 *          backslashes become `/` and the Git Bash `/c/x` and SFTP `/C:/x`
 *          spellings become `C:/x`. The result is lexically cleaned.
 */
QString localPath(const QString &raw, const QString &base, const QString &home, bool windows);

/* Shared texts */

QString fileNotFound(const QString &path);
QString notAFile(const QString &path);
QString directoryNotFound(const QString &path);
QString wroteText(qsizetype bytes, const QString &path);
QString editedText(const QString &path, int count);
/** @param action `editing` or `overwriting`. */
QString notReadYet(const QString &path, const QString &action);
QString changedSinceRead(const QString &path, const QString &action);

/* read_file */

/** @brief The line window a read_file call asks for. */
struct ReadWindow
{
    int offset   = 0;
    int maxLines = kDefaultMaxLines;
};

ReadWindow readWindow(const json &arguments);

/**
 * @brief One streamed read_file: sniffs the leading bytes, then keeps either
 *        the image body or the requested line window, never the whole file.
 */
class Reader
{
public:
    explicit Reader(const ReadWindow &window, qsizetype limit = kReadBytesLimit);

    /** @brief Take the next chunk; false ends the transfer. */
    bool feed(const QByteArray &chunk);

    /** @brief End of file: decide a short file and close its last line. */
    void finish();

    /** @brief The whole file when this read saw all of it from line 0. */
    std::optional<QByteArray> wholeFile() const;

    /**
     * @brief The tool result for @p path.
     * @details An image attachment, the text window with a `[truncated: ...]`
     *          line when more follows, or the refusal for an oversized image
     *          or line.
     */
    QString result(const QString &path, QLLMService *llm) const;

private:
    static constexpr qsizetype kMagicBytes = 16;

    bool sniff();
    bool keepImage(const QByteArray &chunk);
    bool pageText(const QByteArray &chunk);
    bool takeLine(const QByteArray &line);
    bool stop(bool *reason);

    int        m_offset   = 0;
    int        m_maxLines = 0;
    qsizetype  m_limit    = 0;
    QByteArray m_head;
    QString    m_mime;
    QByteArray m_body;
    QByteArray m_pending;
    int        m_lineNum        = 0;
    int        m_emitted        = 0;
    bool       m_sniffed        = false;
    bool       m_stopped        = false;
    bool       m_overLimit      = false;
    bool       m_moreLines      = false;
    bool       m_closedLastLine = false;
};

/* write_file and edit_file */

/** @brief A write_file call; @ref error is set when the call is malformed. */
struct WriteCall
{
    QString path;
    QString content;
    QString error;
};

WriteCall writeCall(const json &arguments);

/** @brief An edit_file call; @ref error is set when the call is malformed. */
struct EditCall
{
    QString path;
    QString oldString;
    QString newString;
    bool    replaceAll = false;
    QString error;
};

EditCall editCall(const json &arguments);

/** @brief The edited content and how many places changed, or the refusal. */
struct EditOutcome
{
    QString content;
    int     count = 0;
    QString error;
};

EditOutcome applyEdit(const QString &content, const EditCall &call);

/* list_files */

/** @brief One entry a directory listing returns. */
struct ListEntry
{
    QString name;
    bool    isDirectory = false;
    bool    isSymlink   = false;
    bool    hidden      = false;
};

/** @brief List one directory by absolute path, or say why not. */
using ListDir = std::function<bool(const QString &dir, QList<ListEntry> *entries, QString *error)>;

/** @brief The options of a list_files call. */
struct ListCall
{
    QString directory;
    QString pattern       = QStringLiteral("*");
    bool    recursive     = false;
    bool    includeHidden = false;
    int     limit         = kDefaultListLimit;
};

/** @brief `directory` (or its old name `directory_path`) and the options. */
ListCall listCall(const json &arguments);

/**
 * @brief The list_files result for @p root.
 * @details Sorted, directories end in `/`, paths relative to @p root. A
 *          recursive walk does not enter hidden directories unless asked, or
 *          follow a symlink. A listing that stops at the limit ends with a
 *          `[truncated: ...]` line.
 * @param caseSensitive Whether `pattern` matches case-sensitively.
 */
QString listFiles(
    const QString &root, const ListCall &call, bool caseSensitive, const ListDir &listDir);

} // namespace QSocToolFileCore

#endif // QSOCTOOLFILECORE_H
