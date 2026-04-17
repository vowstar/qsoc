// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "common/qlspslangbackend.h"

#include <QFile>
#include <QJsonArray>
#include <QUrl>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include <slang/ast/Compilation.h>
#include <slang/diagnostics/CompilationDiags.h>
#include <slang/diagnostics/DiagnosticClient.h>
#include <slang/diagnostics/DiagnosticEngine.h>
#include <slang/syntax/SyntaxTree.h>
#include <slang/text/SourceManager.h>
#include <slang/util/SmallVector.h>

namespace {

/* Diagnostic client that collects diagnostics into a QJsonArray.
   The DiagnosticEngine calls report() for each processed diagnostic
   with fully resolved severity, formatted message, and source ranges. */
class LspDiagnosticCollector : public slang::DiagnosticClient
{
public:
    LspDiagnosticCollector(const slang::SourceManager &srcMgr, slang::BufferID filterBuf)
        : srcMgr(srcMgr)
        , filterBuf(filterBuf)
    {}

    void report(const slang::ReportedDiagnostic &diag) override
    {
        /* Suppress NoTopModules; this fires constantly in single-file mode. */
        if (diag.originalDiagnostic.code == slang::diag::NoTopModules)
            return;

        /* Skip notes and ignored diagnostics. */
        if (diag.severity == slang::DiagnosticSeverity::Note)
            return;
        if (diag.severity == slang::DiagnosticSeverity::Ignored)
            return;

        /* Only include diagnostics originating in the target file. */
        auto origLoc = srcMgr.getFullyOriginalLoc(diag.location);
        if (origLoc.buffer() != filterBuf)
            return;

        /* Map severity: Error/Fatal=1, Warning=2, others=3 (Information). */
        int lspSeverity = 3;
        switch (diag.severity) {
        case slang::DiagnosticSeverity::Fatal:
        case slang::DiagnosticSeverity::Error:
            lspSeverity = 1;
            break;
        case slang::DiagnosticSeverity::Warning:
            lspSeverity = 2;
            break;
        default:
            lspSeverity = 3;
            break;
        }

        /* Map source ranges through macro expansions. */
        slang::SmallVector<slang::SourceRange> mappedRanges;
        if (diagEngine) {
            diagEngine->mapSourceRanges(diag.location, diag.ranges, mappedRanges);
        }

        /* Build range from mapped ranges if available, otherwise point location. */
        int startLine = std::max(0, static_cast<int>(srcMgr.getLineNumber(diag.location)) - 1);
        int startCol  = std::max(0, static_cast<int>(srcMgr.getColumnNumber(diag.location)) - 1);
        int endLine   = startLine;
        int endCol    = startCol;

        if (!mappedRanges.empty()) {
            auto rangeStart = mappedRanges[0].start();
            auto rangeEnd   = mappedRanges[0].end();
            for (size_t idx = 1; idx < mappedRanges.size(); ++idx) {
                if (mappedRanges[idx].start() < rangeStart)
                    rangeStart = mappedRanges[idx].start();
                if (mappedRanges[idx].end() > rangeEnd)
                    rangeEnd = mappedRanges[idx].end();
            }
            /* Include the diagnostic location in the span. */
            if (diag.location < rangeStart)
                rangeStart = diag.location;
            if (diag.location > rangeEnd)
                rangeEnd = diag.location;

            if (srcMgr.isFileLoc(rangeStart) && srcMgr.isFileLoc(rangeEnd)) {
                startLine = std::max(0, static_cast<int>(srcMgr.getLineNumber(rangeStart)) - 1);
                startCol  = std::max(0, static_cast<int>(srcMgr.getColumnNumber(rangeStart)) - 1);
                endLine   = std::max(0, static_cast<int>(srcMgr.getLineNumber(rangeEnd)) - 1);
                endCol    = std::max(0, static_cast<int>(srcMgr.getColumnNumber(rangeEnd)) - 1);
            }
        }

        QString message = QString::fromUtf8(
            diag.formattedMessage.data(), static_cast<int>(diag.formattedMessage.size()));

        QJsonObject range{
            {"start", QJsonObject{{"line", startLine}, {"character", startCol}}},
            {"end", QJsonObject{{"line", endLine}, {"character", endCol}}},
        };

        QJsonObject entry{
            {"severity", lspSeverity},
            {"range", range},
            {"message", message},
            {"source", "slang"},
        };

        /* Attach option name as diagnostic code if available. */
        if (diagEngine) {
            std::string_view optionName = diagEngine->getOptionName(diag.originalDiagnostic.code);
            if (!optionName.empty()) {
                entry["code"]
                    = QString::fromUtf8(optionName.data(), static_cast<int>(optionName.size()));
            }
        }

        /* Collect macro expansion chain as relatedInformation. */
        QJsonArray related;
        for (auto expIt = diag.expansionLocs.rbegin(); expIt != diag.expansionLocs.rend(); ++expIt) {
            slang::SourceLocation expLoc    = *expIt;
            std::string_view      macroName = srcMgr.getMacroName(expLoc);
            QString               expMsg = macroName.empty() ? QStringLiteral("expanded from here")
                                                             : QString("expanded from macro '%1'")
                                                     .arg(
                                                         QString::fromUtf8(
                                                             macroName.data(),
                                                             static_cast<int>(macroName.size())));

            /* Map expansion location to original source. */
            auto origExpLoc = srcMgr.getFullyOriginalLoc(expLoc);
            if (!srcMgr.isFileLoc(origExpLoc))
                continue;

            int     expLine = std::max(0, static_cast<int>(srcMgr.getLineNumber(origExpLoc)) - 1);
            int     expCol  = std::max(0, static_cast<int>(srcMgr.getColumnNumber(origExpLoc)) - 1);
            QString expUri  = QString::fromStdString(std::string(srcMgr.getFileName(origExpLoc)));

            related.append(
                QJsonObject{
                    {"location",
                     QJsonObject{
                         {"uri", expUri},
                         {"range",
                          QJsonObject{
                              {"start", QJsonObject{{"line", expLine}, {"character", expCol}}},
                              {"end", QJsonObject{{"line", expLine}, {"character", expCol}}},
                          }},
                     }},
                    {"message", expMsg},
                });
        }

        /* Collect notes as relatedInformation. */
        QJsonObject mainLocation{
            {"uri", QString::fromStdString(std::string(srcMgr.getFileName(diag.location)))},
            {"range", range},
        };

        for (const auto &note : diag.originalDiagnostic.notes) {
            QString noteMsg;
            if (diagEngine)
                noteMsg = QString::fromStdString(diagEngine->formatMessage(note));

            if (note.location == slang::SourceLocation::NoLocation) {
                /* Notes without a location use the parent diagnostic position
                   if the code explicitly allows it. */
                if (note.code.showNoteWithNoLocation()) {
                    related.append(
                        QJsonObject{
                            {"location", mainLocation},
                            {"message", noteMsg},
                        });
                }
                continue;
            }
            if (!srcMgr.isFileLoc(note.location))
                continue;

            int noteLine = std::max(0, static_cast<int>(srcMgr.getLineNumber(note.location)) - 1);
            int noteCol  = std::max(0, static_cast<int>(srcMgr.getColumnNumber(note.location)) - 1);
            QString noteUri = QString::fromStdString(std::string(srcMgr.getFileName(note.location)));

            related.append(
                QJsonObject{
                    {"location",
                     QJsonObject{
                         {"uri", noteUri},
                         {"range",
                          QJsonObject{
                              {"start", QJsonObject{{"line", noteLine}, {"character", noteCol}}},
                              {"end", QJsonObject{{"line", noteLine}, {"character", noteCol}}},
                          }},
                     }},
                    {"message", noteMsg},
                });
        }
        if (!related.isEmpty()) {
            entry["relatedInformation"] = related;
        }

        collected.append(entry);
    }

    QJsonArray collected;

    /* Set by buildDiagnostics before issuing. */
    slang::DiagnosticEngine *diagEngine = nullptr;

private:
    const slang::SourceManager &srcMgr;
    slang::BufferID             filterBuf;
};

} /* anonymous namespace */

QLspSlangBackend::QLspSlangBackend(QObject *parent)
    : QLspBackend(parent)
{}

QLspSlangBackend::~QLspSlangBackend()
{
    stop();
}

bool QLspSlangBackend::start(const QString &workspaceFolder)
{
    workspace = workspaceFolder;
    ready     = true;
    return true;
}

void QLspSlangBackend::stop()
{
    ready = false;
    files.clear();
    workspace.clear();
}

bool QLspSlangBackend::isReady() const
{
    return ready;
}

QStringList QLspSlangBackend::extensions() const
{
    return {".v", ".sv", ".svh", ".vh"};
}

QJsonValue QLspSlangBackend::request(const QString &method, const QJsonObject &params)
{
    Q_UNUSED(method)
    Q_UNUSED(params)
    return QJsonValue();
}

void QLspSlangBackend::notify(const QString &method, const QJsonObject &params)
{
    if (method == "textDocument/didOpen") {
        handleDidOpen(params);
    } else if (method == "textDocument/didChange") {
        handleDidChange(params);
    } else if (method == "textDocument/didSave") {
        handleDidSave(params);
    } else if (method == "textDocument/didClose") {
        handleDidClose(params);
    }
}

void QLspSlangBackend::handleDidOpen(const QJsonObject &params)
{
    QJsonObject doc  = params["textDocument"].toObject();
    QString     uri  = doc["uri"].toString();
    QString     text = doc["text"].toString();

    FileState &state = files[uri];
    state.sourceText = text.toStdString();

    recompileAndDiagnose(uri);
}

void QLspSlangBackend::handleDidChange(const QJsonObject &params)
{
    QJsonObject doc = params["textDocument"].toObject();
    QString     uri = doc["uri"].toString();

    QJsonArray changes = params["contentChanges"].toArray();
    if (changes.isEmpty())
        return;

    /* Full text sync: take the last content change. */
    QString    text  = changes.last().toObject()["text"].toString();
    FileState &state = files[uri];
    state.sourceText = text.toStdString();

    recompileAndDiagnose(uri);
}

void QLspSlangBackend::handleDidSave(const QJsonObject &params)
{
    QJsonObject doc = params["textDocument"].toObject();
    QString     uri = doc["uri"].toString();

    if (files.contains(uri)) {
        /* File is already tracked with up-to-date content from didChange.
           A preceding didChange already triggered recompilation, so skip
           redundant work here. */
        return;
    }

    /* Not yet tracked; read from disk for initial diagnostics. */
    QString filePath = QUrl(uri).toLocalFile();
    QFile   file(filePath);
    if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        FileState &state = files[uri];
        state.sourceText = QString::fromUtf8(file.readAll()).toStdString();
        file.close();
    } else {
        return;
    }

    recompileAndDiagnose(uri);
}

void QLspSlangBackend::handleDidClose(const QJsonObject &params)
{
    QJsonObject doc = params["textDocument"].toObject();
    QString     uri = doc["uri"].toString();

    files.remove(uri);

    /* Publish empty diagnostics to clear stale entries. */
    emit notification(
        "textDocument/publishDiagnostics", QJsonObject{{"uri", uri}, {"diagnostics", QJsonArray()}});
}

void QLspSlangBackend::recompileAndDiagnose(const QString &uri)
{
    QJsonArray diags = buildDiagnostics(uri);
    emit       notification(
        "textDocument/publishDiagnostics", QJsonObject{{"uri", uri}, {"diagnostics", diags}});
}

QJsonArray QLspSlangBackend::buildDiagnostics(const QString &filterUri)
{
    if (!files.contains(filterUri))
        return {};

    /* Create a fresh SourceManager per compilation. slang SourceManager
       does not support replacing buffers in older versions, so we rebuild
       it each time to avoid stale buffer issues. */
    slang::SourceManager localSourceManager;
    slang::BufferID      filterBuf{};

    /* Parse all tracked files into syntax trees. */
    std::vector<std::shared_ptr<slang::syntax::SyntaxTree>> trees;

    for (auto iter = files.constBegin(); iter != files.constEnd(); ++iter) {
        QString     filePath = QUrl(iter.key()).toLocalFile();
        std::string pathStr  = filePath.toStdString();
        auto        tree     = slang::syntax::SyntaxTree::fromText(
            iter->sourceText, localSourceManager, pathStr, pathStr);
        if (!tree)
            continue;

        if (iter.key() == filterUri) {
            filterBuf = tree->root().getFirstToken().location().buffer();
        }
        trees.emplace_back(std::move(tree));
    }

    /* Create compilation with flags suitable for single-file analysis. */
    slang::ast::CompilationOptions compOptions;
    compOptions.flags |= slang::ast::CompilationFlags::AllowTopLevelIfacePorts;

    slang::ast::Compilation compilation(compOptions);
    for (auto &tree : trees) {
        compilation.addSyntaxTree(tree);
    }
    compilation.getRoot();

    /* Set up diagnostic engine with pragma support and our collector. */
    slang::DiagnosticEngine engine(localSourceManager);

    auto collector        = std::make_shared<LspDiagnosticCollector>(localSourceManager, filterBuf);
    collector->diagEngine = &engine;
    engine.addClient(collector);

    /* Apply pragma-based diagnostic mappings. */
    auto pragmaDiags = engine.setMappingsFromPragmas(filterBuf);
    for (const auto &diag : pragmaDiags) {
        engine.issue(diag);
    }

    /* Issue parse diagnostics from all syntax trees. */
    for (const auto &tree : trees) {
        for (const auto &diag : tree->diagnostics()) {
            engine.issue(diag);
        }
    }

    /* Issue semantic diagnostics from the compilation. */
    for (const auto &diag : compilation.getSemanticDiagnostics()) {
        engine.issue(diag);
    }

    return collector->collected;
}
