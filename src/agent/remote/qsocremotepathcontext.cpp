// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/remote/qsocremotepathcontext.h"

namespace {

constexpr QChar kSep = QLatin1Char('/');

} // namespace

QSocRemotePathContext::QSocRemotePathContext(QString root, QString cwd, QStringList writableDirs)
    : m_root(lexicalNormalize(std::move(root)))
    , m_cwd(lexicalNormalize(cwd.isEmpty() ? m_root : cwd))
    , m_writableDirs(std::move(writableDirs))
{
    for (QString &dir : m_writableDirs) {
        dir = lexicalNormalize(dir);
    }
}

void QSocRemotePathContext::setRoot(const QString &root)
{
    m_root = lexicalNormalize(root);
    if (m_cwd.isEmpty()) {
        m_cwd = m_root;
    }
}

void QSocRemotePathContext::setCwd(const QString &cwd)
{
    m_cwd = lexicalNormalize(cwd.isEmpty() ? m_root : cwd);
}

void QSocRemotePathContext::setWritableDirs(const QStringList &dirs)
{
    m_writableDirs.clear();
    m_writableDirs.reserve(dirs.size());
    for (const QString &dir : dirs) {
        m_writableDirs.push_back(lexicalNormalize(dir));
    }
}

QStringList QSocRemotePathContext::splitPosix(const QString &path)
{
    QStringList parts;
    QString     current;
    current.reserve(path.size());
    for (const QChar ch : path) {
        if (ch == kSep) {
            if (!current.isEmpty()) {
                parts.push_back(current);
                current.clear();
            }
        } else {
            current.append(ch);
        }
    }
    if (!current.isEmpty()) {
        parts.push_back(current);
    }
    return parts;
}

QString QSocRemotePathContext::joinPosix(const QStringList &parts, bool absolute)
{
    QString out;
    if (absolute) {
        out.append(kSep);
    }
    for (int idx = 0; idx < parts.size(); ++idx) {
        if (idx != 0) {
            out.append(kSep);
        }
        out.append(parts.at(idx));
    }
    return out.isEmpty() ? QStringLiteral("/") : out;
}

QString QSocRemotePathContext::lexicalNormalize(const QString &path)
{
    if (path.isEmpty()) {
        return QStringLiteral("/");
    }
    const bool  absolute = path.startsWith(kSep);
    QStringList parts    = splitPosix(path);
    QStringList out;
    for (const QString &seg : parts) {
        if (seg == QStringLiteral(".")) {
            continue;
        }
        if (seg == QStringLiteral("..")) {
            if (!out.isEmpty() && out.last() != QStringLiteral("..")) {
                out.removeLast();
            } else if (!absolute) {
                out.push_back(seg);
            }
            /* Absolute paths silently clamp at root: /.. == / */
            continue;
        }
        out.push_back(seg);
    }
    return joinPosix(out, absolute);
}

QString QSocRemotePathContext::normalize(const QString &path) const
{
    if (path.isEmpty()) {
        return m_cwd.isEmpty() ? m_root : m_cwd;
    }
    if (path.startsWith(kSep)) {
        return lexicalNormalize(path);
    }
    const QString base = m_cwd.isEmpty() ? m_root : m_cwd;
    return lexicalNormalize(base + kSep + path);
}

bool QSocRemotePathContext::isWritable(const QString &normalizedPath) const
{
    if (normalizedPath.isEmpty() || !normalizedPath.startsWith(kSep)) {
        return false;
    }
    for (const QString &dir : m_writableDirs) {
        if (dir.isEmpty()) {
            continue;
        }
        if (normalizedPath == dir) {
            return true;
        }
        if (dir == QStringLiteral("/")) {
            return true;
        }
        if (normalizedPath.startsWith(dir + kSep)) {
            return true;
        }
    }
    return false;
}

QString QSocRemotePathContext::resolveCwdRequest(const QString &requested) const
{
    const QString candidate = normalize(requested);
    if (m_root.isEmpty() || m_root == QStringLiteral("/")) {
        return candidate;
    }
    if (candidate == m_root) {
        return candidate;
    }
    if (candidate.startsWith(m_root + kSep)) {
        return candidate;
    }
    return m_root;
}
