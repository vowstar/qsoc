// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/remote/qsocsshconfigparser.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QTextStream>

namespace {

QString stripInlineComment(const QString &raw)
{
    /* SSH config uses '#' at any position outside a quoted literal to start a
     * comment. We don't honor '#' inside a quoted string. */
    bool inQuote = false;
    for (int i = 0; i < raw.size(); ++i) {
        const QChar c = raw.at(i);
        if (c == QLatin1Char('"')) {
            inQuote = !inQuote;
        } else if (c == QLatin1Char('#') && !inQuote) {
            return raw.left(i);
        }
    }
    return raw;
}

QString unquote(const QString &token)
{
    if (token.size() >= 2 && token.startsWith(QLatin1Char('"'))
        && token.endsWith(QLatin1Char('"'))) {
        return token.mid(1, token.size() - 2);
    }
    return token;
}

QString normalizeKeyword(const QString &k)
{
    return k.toLower();
}

QStringList splitHostPatterns(const QString &raw)
{
    QStringList parts;
    QString     current;
    bool        inQuote = false;
    for (const QChar c : raw) {
        if (c == QLatin1Char('"')) {
            inQuote = !inQuote;
            continue;
        }
        if (!inQuote && c.isSpace()) {
            if (!current.isEmpty()) {
                parts.push_back(current);
                current.clear();
            }
        } else {
            current.append(c);
        }
    }
    if (!current.isEmpty()) {
        parts.push_back(current);
    }
    return parts;
}

} // namespace

QStringList QSocSshConfigParser::tokenizeLine(const QString &line)
{
    /* SSH config tokenization: first whitespace-separated word is the
     * keyword, the rest is the value. An '=' is allowed as a separator
     * between keyword and value. Inner whitespace in the value is preserved
     * except that multiple spaces collapse via split semantics upstream. */
    QString trimmed = stripInlineComment(line).trimmed();
    if (trimmed.isEmpty()) {
        return {};
    }

    /* Find the keyword boundary: first space or '=' character. */
    int boundary = -1;
    for (int i = 0; i < trimmed.size(); ++i) {
        const QChar c = trimmed.at(i);
        if (c.isSpace() || c == QLatin1Char('=')) {
            boundary = i;
            break;
        }
    }
    if (boundary < 0) {
        return {trimmed};
    }
    const QString keyword = trimmed.left(boundary);
    QString       rest    = trimmed.mid(boundary + 1).trimmed();
    while (!rest.isEmpty() && rest.front() == QLatin1Char('=')) {
        rest = rest.mid(1).trimmed();
    }
    return {keyword, rest};
}

QString QSocSshConfigParser::expandTildeOnly(const QString &path)
{
    if (path.startsWith(QLatin1Char('~'))) {
        const QString home = QDir::homePath();
        if (path.size() == 1) {
            return home;
        }
        if (path.at(1) == QLatin1Char('/')) {
            return home + path.mid(1);
        }
    }
    return path;
}

QString QSocSshConfigParser::expandTokens(
    const QString &value,
    const QString &alias,
    const QString &hostname,
    int            port,
    const QString &user)
{
    QString out;
    out.reserve(value.size());
    for (int i = 0; i < value.size(); ++i) {
        const QChar c = value.at(i);
        if (c != QLatin1Char('%') || i + 1 >= value.size()) {
            out.append(c);
            continue;
        }
        const QChar next = value.at(i + 1);
        ++i;
        switch (next.toLatin1()) {
        case '%':
            out.append(QLatin1Char('%'));
            break;
        case 'h':
            out.append(hostname);
            break;
        case 'n':
            out.append(alias);
            break;
        case 'p':
            out.append(QString::number(port));
            break;
        case 'r':
            out.append(user);
            break;
        default:
            /* Unknown token: pass through literally to avoid corrupting values. */
            out.append(QLatin1Char('%'));
            out.append(next);
            break;
        }
    }
    return expandTildeOnly(out);
}

bool QSocSshConfigParser::patternMatch(const QString &pattern, const QString &host)
{
    /* Convert a glob-ish pattern (? and *) into a regex. Negation (leading '!')
     * is handled by the caller. */
    QString regex;
    regex.reserve(pattern.size() * 2 + 2);
    regex.append(QLatin1Char('^'));
    for (const QChar c : pattern) {
        switch (c.toLatin1()) {
        case '*':
            regex.append(QStringLiteral(".*"));
            break;
        case '?':
            regex.append(QLatin1Char('.'));
            break;
        case '.':
        case '\\':
        case '+':
        case '(':
        case ')':
        case '[':
        case ']':
        case '{':
        case '}':
        case '|':
        case '^':
        case '$':
            regex.append(QLatin1Char('\\'));
            regex.append(c);
            break;
        default:
            regex.append(c);
            break;
        }
    }
    regex.append(QLatin1Char('$'));
    QRegularExpression re(regex, QRegularExpression::CaseInsensitiveOption);
    return re.match(host).hasMatch();
}

bool QSocSshConfigParser::blockMatches(const QStringList &patterns, const QString &host)
{
    /* OpenSSH semantics:
     *  - Any positive pattern must match.
     *  - A negated pattern (!) that matches disqualifies the block entirely.
     *  - If all patterns are negated, a block cannot match (we also follow
     *    OpenSSH and refuse those). */
    bool anyPositiveMatch = false;
    bool anyPositive      = false;
    for (const QString &raw : patterns) {
        if (raw.isEmpty()) {
            continue;
        }
        if (raw.startsWith(QLatin1Char('!'))) {
            if (patternMatch(raw.mid(1), host)) {
                return false;
            }
        } else {
            anyPositive = true;
            if (patternMatch(raw, host)) {
                anyPositiveMatch = true;
            }
        }
    }
    return anyPositive && anyPositiveMatch;
}

bool QSocSshConfigParser::parse(const QString &configPath)
{
    QSet<QString> seen;
    const QString abs = QFileInfo(configPath).absoluteFilePath();
    return parseFile(abs, 0, QFileInfo(abs).absolutePath(), seen);
}

bool QSocSshConfigParser::parseFile(
    const QString &absPath, int depth, const QString &parentDir, QSet<QString> &seenFiles)
{
    Q_UNUSED(parentDir);
    if (depth > kMaxIncludeDepth) {
        m_notes.push_back(QStringLiteral("Include depth exceeded at %1").arg(absPath));
        return false;
    }
    if (seenFiles.contains(absPath)) {
        m_notes.push_back(QStringLiteral("Skipping re-included file %1").arg(absPath));
        return true;
    }
    seenFiles.insert(absPath);

    QFile file(absPath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        m_notes.push_back(QStringLiteral("Cannot open config file %1").arg(absPath));
        return false;
    }
    QTextStream stream(&file);

    Block current;
    auto  flush = [&]() {
        if (!current.patterns.isEmpty() || !current.options.isEmpty()) {
            m_blocks.push_back(current);
        }
        current = Block{};
    };

    const QString thisDir = QFileInfo(absPath).absolutePath();

    while (!stream.atEnd()) {
        const QString line   = stream.readLine();
        const auto    tokens = tokenizeLine(line);
        if (tokens.isEmpty()) {
            continue;
        }
        const QString keyword = normalizeKeyword(tokens.at(0));
        const QString value   = tokens.size() > 1 ? tokens.at(1) : QString();

        if (keyword == QLatin1String("host")) {
            flush();
            current.patterns = splitHostPatterns(value);
            continue;
        }
        if (keyword == QLatin1String("match")) {
            m_notes.push_back(QStringLiteral(
                                  "Match directive at %1 is not supported; "
                                  "ignoring block")
                                  .arg(absPath));
            flush();
            current.patterns.clear();
            continue;
        }
        if (keyword == QLatin1String("include")) {
            handleInclude(value, depth + 1, thisDir, seenFiles);
            continue;
        }

        /* Unsupported/ignored directives we still want to track. */
        static const QSet<QString> ignored = {
            QStringLiteral("proxycommand"),
            QStringLiteral("localforward"),
            QStringLiteral("remoteforward"),
            QStringLiteral("dynamicforward"),
            QStringLiteral("canonicalizehostname"),
            QStringLiteral("certificatefile"),
        };
        if (ignored.contains(keyword)) {
            m_notes.push_back(
                QStringLiteral("Ignoring unsupported directive '%1' in %2").arg(keyword, absPath));
            continue;
        }

        current.options.push_back({keyword, unquote(value)});
    }
    flush();
    return true;
}

void QSocSshConfigParser::handleInclude(
    const QString &value, int depth, const QString &parentDir, QSet<QString> &seenFiles)
{
    if (value.isEmpty()) {
        return;
    }

    /* Split the Include argument into space-separated file specs; each spec
     * may contain a glob. */
    const QStringList specs = splitHostPatterns(value);
    for (const QString &spec : specs) {
        QString expanded = expandTildeOnly(unquote(spec));

        /* Resolve relative paths against the including file's directory. */
        if (!QFileInfo(expanded).isAbsolute()) {
            expanded = QDir(parentDir).absoluteFilePath(expanded);
        }

        /* Enforce allowed roots: user's ~/.ssh and the including file's dir. */
        const QString home          = QDir::homePath();
        const QString sshDir        = home + QStringLiteral("/.ssh");
        auto          insideAllowed = [&](const QString &path) {
            const QString p = QFileInfo(path).absoluteFilePath();
            return p.startsWith(sshDir + QLatin1Char('/')) || p == sshDir
                   || p.startsWith(parentDir + QLatin1Char('/')) || p == parentDir;
        };

        /* Expand glob via QDir::entryList. */
        QFileInfo   fi(expanded);
        QStringList matches;
        if (expanded.contains(QLatin1Char('*')) || expanded.contains(QLatin1Char('?'))) {
            const QString     dirPath = fi.absolutePath();
            const QString     pattern = fi.fileName();
            QDir              dir(dirPath);
            const QStringList entries = dir.entryList({pattern}, QDir::Files | QDir::NoSymLinks);
            for (const QString &e : entries) {
                matches.push_back(dir.absoluteFilePath(e));
            }
        } else {
            matches.push_back(fi.absoluteFilePath());
        }

        for (const QString &m : matches) {
            if (!insideAllowed(m)) {
                m_notes.push_back(
                    QStringLiteral("Rejecting Include outside allowed roots: %1").arg(m));
                continue;
            }
            if (static_cast<int>(seenFiles.size()) >= kMaxIncludedFiles) {
                m_notes.push_back(QStringLiteral("Max included files reached; stopping"));
                return;
            }
            parseFile(m, depth, QFileInfo(m).absolutePath(), seenFiles);
        }
    }
}

QStringList QSocSshConfigParser::listMenuHosts() const
{
    QStringList   out;
    QSet<QString> seenAliases;
    for (const Block &b : m_blocks) {
        for (const QString &raw : b.patterns) {
            if (raw.isEmpty()) {
                continue;
            }
            if (raw.startsWith(QLatin1Char('!'))) {
                continue;
            }
            if (raw.contains(QLatin1Char('*')) || raw.contains(QLatin1Char('?'))) {
                continue;
            }
            if (seenAliases.contains(raw)) {
                continue;
            }
            seenAliases.insert(raw);
            out.push_back(raw);
        }
    }
    return out;
}

QSocSshHostConfig QSocSshConfigParser::resolve(const QString &alias) const
{
    QSocSshHostConfig cfg;
    cfg.alias    = alias;
    cfg.hostname = alias;

    /* First pass: gather candidate values from matching blocks in file order.
     * Scalars: first value wins. IdentityFile: accumulates until
     * IdentitiesOnly=yes is set, at which point earlier implicit defaults
     * are cleared and only subsequent explicit IdentityFile entries apply
     * (approximated here by a simple "mark seen" approach). */

    bool hostnameSet       = false;
    bool portSet           = false;
    bool userSet           = false;
    bool strictSet         = false;
    bool identitiesOnlySet = false;
    bool knownHostsSet     = false;
    bool addKeysToAgentSet = false;
    bool matchedAnyBlock   = false;

    /* Raw IdentityFile values deferred until scalars are resolved, so token
     * expansion can see the final hostname/port/user regardless of directive
     * ordering inside a block. */
    QStringList rawIdentityFiles;
    QString     rawKnownHosts;

    for (const Block &block : m_blocks) {
        if (!blockMatches(block.patterns, alias)) {
            continue;
        }
        matchedAnyBlock = true;
        for (const auto &kv : block.options) {
            const QString &keyword = kv.first;
            const QString  raw     = kv.second;

            if (keyword == QLatin1String("hostname") && !hostnameSet) {
                cfg.hostname = expandTokens(raw, alias, alias, cfg.port, cfg.user);
                hostnameSet  = true;
            } else if (keyword == QLatin1String("port") && !portSet) {
                bool      ok = false;
                const int p  = raw.toInt(&ok);
                if (ok && p > 0 && p < 65536) {
                    cfg.port = p;
                    portSet  = true;
                }
            } else if (keyword == QLatin1String("user") && !userSet) {
                cfg.user = raw;
                userSet  = true;
            } else if (keyword == QLatin1String("identityfile")) {
                rawIdentityFiles.push_back(raw);
            } else if (keyword == QLatin1String("identitiesonly") && !identitiesOnlySet) {
                cfg.identitiesOnly = (raw.toLower() == QLatin1String("yes"));
                identitiesOnlySet  = true;
            } else if (keyword == QLatin1String("userknownhostsfile") && !knownHostsSet) {
                rawKnownHosts = raw;
                knownHostsSet = true;
            } else if (keyword == QLatin1String("stricthostkeychecking") && !strictSet) {
                const QString v = raw.toLower();
                if (v == QLatin1String("yes") || v == QLatin1String("ask")) {
                    cfg.strictHostKey = QSocSshHostConfig::StrictHostKey::Yes;
                } else if (v == QLatin1String("accept-new")) {
                    cfg.strictHostKey = QSocSshHostConfig::StrictHostKey::AcceptNew;
                } else if (v == QLatin1String("no") || v == QLatin1String("off")) {
                    cfg.strictHostKey = QSocSshHostConfig::StrictHostKey::No;
                }
                strictSet = true;
            } else if (keyword == QLatin1String("addkeystoagent") && !addKeysToAgentSet) {
                const QString v = raw.toLower();
                cfg.addKeysToAgent
                    = (v == QLatin1String("yes") || v == QLatin1String("ask")
                       || v == QLatin1String("confirm"));
                addKeysToAgentSet = true;
            }
        }
    }

    /* Second pass: expand tokens using the settled scalars. */
    for (const QString &raw : rawIdentityFiles) {
        const QString path = expandTokens(raw, alias, cfg.hostname, cfg.port, cfg.user);
        if (!cfg.identityFiles.contains(path)) {
            cfg.identityFiles.push_back(path);
        }
    }
    if (!rawKnownHosts.isEmpty()) {
        cfg.userKnownHostsFile = expandTildeOnly(
            expandTokens(rawKnownHosts, alias, cfg.hostname, cfg.port, cfg.user));
    }

    cfg.fromConfig = matchedAnyBlock;
    return cfg;
}
