// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "common/qlspservice.h"

#include <QFileInfo>
#include <QUrl>

QLspService *QLspService::instance()
{
    static QLspService service;
    return &service;
}

QLspService::QLspService(QObject *parent)
    : QObject(parent)
{}

QLspService::~QLspService()
{
    stopAll();
}

void QLspService::addBackend(QLspBackend *backend)
{
    if (!backend)
        return;

    backend->setParent(this);
    backends.append(backend);

    /* Map each extension to this backend (first-registered wins). */
    for (const QString &ext : backend->extensions()) {
        QString lower = ext.toLower();
        if (!extMap.contains(lower)) {
            extMap.insert(lower, backend);
        }
    }

    connect(backend, &QLspBackend::notification, this, &QLspService::onBackendNotification);
}

void QLspService::startAll(const QString &workspaceFolder)
{
    for (QLspBackend *b : backends) {
        b->start(workspaceFolder);
    }
}

void QLspService::stopAll()
{
    for (QLspBackend *b : backends) {
        b->stop();
    }
    fileVersions.clear();
    diagCache.clear();
    diagPending.clear();
    openFiles.clear();
}

QLspBackend *QLspService::backendFor(const QString &filePath) const
{
    QFileInfo info(filePath);
    QString   ext = info.suffix();
    if (!ext.isEmpty())
        ext.prepend('.');
    ext = ext.toLower();

    return extMap.value(ext, nullptr);
}

bool QLspService::isAvailable() const
{
    for (const QLspBackend *b : backends) {
        if (b->isReady())
            return true;
    }
    return false;
}

/* File synchronization */

void QLspService::didOpen(const QString &filePath, const QString &content)
{
    QLspBackend *b = backendFor(filePath);
    if (!b || !b->isReady())
        return;

    QString uri = toUri(filePath);
    if (openFiles.contains(uri))
        return;

    fileVersions[uri] = 1;
    openFiles.insert(uri);

    QJsonObject params{
        {"textDocument",
         QJsonObject{
             {"uri", uri},
             {"languageId", QFileInfo(filePath).suffix()},
             {"version", 1},
             {"text", content},
         }},
    };
    b->notify("textDocument/didOpen", params);
}

void QLspService::didChange(const QString &filePath, const QString &content)
{
    QLspBackend *b = backendFor(filePath);
    if (!b || !b->isReady())
        return;

    QString uri = toUri(filePath);
    if (!openFiles.contains(uri)) {
        didOpen(filePath, content);
        return;
    }

    int         version = ++fileVersions[uri];
    QJsonObject params{
        {"textDocument", QJsonObject{{"uri", uri}, {"version", version}}},
        {"contentChanges", QJsonArray{QJsonObject{{"text", content}}}},
    };
    b->notify("textDocument/didChange", params);
}

void QLspService::didSave(const QString &filePath)
{
    QLspBackend *b = backendFor(filePath);
    if (!b || !b->isReady())
        return;

    QString uri = toUri(filePath);

    /* Read the current file content from disk. */
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return;
    QString content = QString::fromUtf8(file.readAll());
    file.close();

    if (!openFiles.contains(uri)) {
        /* File was not tracked yet; open it to trigger initial diagnostics. */
        didOpen(filePath, content);
        return;
    }

    /* File is already open; update content and notify save. */
    didChange(filePath, content);

    QJsonObject params{
        {"textDocument", QJsonObject{{"uri", uri}}},
    };
    b->notify("textDocument/didSave", params);
}

void QLspService::didClose(const QString &filePath)
{
    QLspBackend *b = backendFor(filePath);
    if (!b || !b->isReady())
        return;

    QString uri = toUri(filePath);
    if (!openFiles.contains(uri))
        return;

    QJsonObject params{
        {"textDocument", QJsonObject{{"uri", uri}}},
    };
    b->notify("textDocument/didClose", params);

    openFiles.remove(uri);
    fileVersions.remove(uri);
}

/* Query convenience wrappers */

void QLspService::ensureFileOpen(const QString &filePath)
{
    QString uri = toUri(filePath);
    if (openFiles.contains(uri))
        return;

    QFile file(filePath);
    if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        didOpen(filePath, QString::fromUtf8(file.readAll()));
        file.close();
    }
}

QJsonValue QLspService::definition(const QString &filePath, int line, int character)
{
    QLspBackend *b = backendFor(filePath);
    if (!b || !b->isReady())
        return QJsonValue();

    ensureFileOpen(filePath);
    return b->request("textDocument/definition", buildPositionParams(filePath, line, character));
}

QJsonValue QLspService::hover(const QString &filePath, int line, int character)
{
    QLspBackend *b = backendFor(filePath);
    if (!b || !b->isReady())
        return QJsonValue();

    ensureFileOpen(filePath);
    return b->request("textDocument/hover", buildPositionParams(filePath, line, character));
}

QJsonArray QLspService::references(const QString &filePath, int line, int character)
{
    QLspBackend *b = backendFor(filePath);
    if (!b || !b->isReady())
        return QJsonArray();

    ensureFileOpen(filePath);

    QJsonObject params = buildPositionParams(filePath, line, character);
    params["context"]  = QJsonObject{{"includeDeclaration", true}};

    QJsonValue result = b->request("textDocument/references", params);
    return result.isArray() ? result.toArray() : QJsonArray();
}

QJsonArray QLspService::documentSymbol(const QString &filePath)
{
    QLspBackend *b = backendFor(filePath);
    if (!b || !b->isReady())
        return QJsonArray();

    ensureFileOpen(filePath);

    QJsonValue result = b->request("textDocument/documentSymbol", buildDocumentParams(filePath));
    return result.isArray() ? result.toArray() : QJsonArray();
}

QJsonArray QLspService::diagnostics(const QString &filePath) const
{
    QString uri = toUri(filePath);
    return diagCache.value(uri, QJsonArray());
}

QMap<QString, QJsonArray> QLspService::drainPendingDiagnostics()
{
    QMap<QString, QJsonArray> result;
    for (const QString &uri : diagPending) {
        if (diagCache.contains(uri)) {
            result.insert(fromUri(uri), diagCache[uri]);
        }
    }
    diagPending.clear();
    return result;
}

/* Internal helpers */

void QLspService::onBackendNotification(const QString &method, const QJsonObject &params)
{
    if (method != "textDocument/publishDiagnostics")
        return;

    QString    uri   = params["uri"].toString();
    QJsonArray diags = params["diagnostics"].toArray();

    diagCache[uri] = diags;
    diagPending.insert(uri);

    emit diagnosticsUpdated(fromUri(uri), diags);
}

QJsonObject QLspService::buildPositionParams(const QString &filePath, int line, int character) const
{
    return {
        {"textDocument", QJsonObject{{"uri", toUri(filePath)}}},
        {"position", QJsonObject{{"line", line - 1}, {"character", character - 1}}},
    };
}

QJsonObject QLspService::buildDocumentParams(const QString &filePath) const
{
    return {
        {"textDocument", QJsonObject{{"uri", toUri(filePath)}}},
    };
}

QString QLspService::toUri(const QString &filePath)
{
    QFileInfo info(filePath);
    QString   absolute = info.absoluteFilePath();
    return QUrl::fromLocalFile(absolute).toString();
}

QString QLspService::fromUri(const QString &uri)
{
    return QUrl(uri).toLocalFile();
}
