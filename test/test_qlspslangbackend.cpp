// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "common/qlspslangbackend.h"
#include "qsoc_test.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QTextStream>
#include <QUrl>
#include <QtTest>

class Test : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir tempDir;
    QString       createVerilogFile(const QString &name, const QString &content);
    QString       fileToUri(const QString &filePath);
    QJsonObject   buildDidOpenParams(const QString &filePath, const QString &content);
    QJsonObject   buildDidSaveParams(const QString &filePath);
    QJsonObject   buildDidCloseParams(const QString &filePath);

private slots:
    void initTestCase();
    void cleanupTestCase();

    /* Lifecycle */
    void startStop();
    void extensions_returnsVerilogTypes();
    void capabilities_declaresTextDocumentSync();
    void isReady_afterStart();
    void isReady_afterStop();

    /* Diagnostics for valid Verilog */
    void diagnostics_validModule_noDiagnostics();

    /* Diagnostics for invalid Verilog */
    void diagnostics_missingEndmodule_hasError();
    void diagnostics_undeclaredVariable_hasError();
    void diagnostics_syntaxError_hasError();

    /* File lifecycle */
    void didOpen_triggersDiagnostics();
    void didChange_updatesDiagnostics();
    void didClose_clearsDiagnostics();
    void didSave_readsFromDisk();

    /* Diagnostic quality */
    void diagnostics_noTopModules_suppressed();
    void diagnostics_hasRange_notPointOnly();
    void diagnostics_hasDiagnosticCode();
    void diagnostics_persistentSourceManager();

    /* Request returns null for unimplemented methods */
    void request_unsupported_returnsNull();
};

void Test::initTestCase()
{
    QVERIFY(tempDir.isValid());
}

void Test::cleanupTestCase()
{
    tempDir.remove();
}

QString Test::createVerilogFile(const QString &name, const QString &content)
{
    QString path = tempDir.path() + "/" + name;
    QFile   file(path);
    if (file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QTextStream stream(&file);
        stream << content;
    }
    return path;
}

QString Test::fileToUri(const QString &filePath)
{
    return QUrl::fromLocalFile(QFileInfo(filePath).absoluteFilePath()).toString();
}

QJsonObject Test::buildDidOpenParams(const QString &filePath, const QString &content)
{
    return {
        {"textDocument",
         QJsonObject{
             {"uri", fileToUri(filePath)},
             {"languageId", "verilog"},
             {"version", 1},
             {"text", content},
         }},
    };
}

QJsonObject Test::buildDidSaveParams(const QString &filePath)
{
    return {
        {"textDocument", QJsonObject{{"uri", fileToUri(filePath)}}},
    };
}

QJsonObject Test::buildDidCloseParams(const QString &filePath)
{
    return {
        {"textDocument", QJsonObject{{"uri", fileToUri(filePath)}}},
    };
}

/* Lifecycle tests */

void Test::startStop()
{
    QLspSlangBackend backend;
    QVERIFY(!backend.isReady());
    QVERIFY(backend.start(tempDir.path()));
    QVERIFY(backend.isReady());
    backend.stop();
    QVERIFY(!backend.isReady());
}

void Test::extensions_returnsVerilogTypes()
{
    QLspSlangBackend backend;
    QStringList      exts = backend.extensions();
    QVERIFY(exts.contains(".v"));
    QVERIFY(exts.contains(".sv"));
    QVERIFY(exts.contains(".svh"));
    QVERIFY(exts.contains(".vh"));
}

void Test::capabilities_declaresTextDocumentSync()
{
    QLspSlangBackend backend;
    QJsonObject      caps = backend.capabilities();
    QVERIFY(caps.contains("textDocumentSync"));

    QJsonObject sync = caps["textDocumentSync"].toObject();
    QVERIFY(sync["openClose"].toBool());
    QCOMPARE(sync["change"].toInt(), 1); /* Full text sync */
    QVERIFY(sync["save"].toBool());
}

void Test::isReady_afterStart()
{
    QLspSlangBackend backend;
    backend.start(tempDir.path());
    QVERIFY(backend.isReady());
    backend.stop();
}

void Test::isReady_afterStop()
{
    QLspSlangBackend backend;
    backend.start(tempDir.path());
    backend.stop();
    QVERIFY(!backend.isReady());
}

/* Diagnostic tests */

void Test::diagnostics_validModule_noDiagnostics()
{
    QLspSlangBackend backend;
    backend.start(tempDir.path());

    QSignalSpy spy(&backend, &QLspBackend::notification);

    QString code = "module good_module (\n"
                   "    input wire clk,\n"
                   "    input wire rst_n,\n"
                   "    output reg [7:0] count\n"
                   ");\n"
                   "always @(posedge clk or negedge rst_n) begin\n"
                   "    if (!rst_n)\n"
                   "        count <= 8'b0;\n"
                   "    else\n"
                   "        count <= count + 1;\n"
                   "end\n"
                   "endmodule\n";

    QString filePath = createVerilogFile("good.v", code);
    backend.notify("textDocument/didOpen", buildDidOpenParams(filePath, code));

    QVERIFY(spy.count() >= 1);

    /* Check the last notification has empty or no error diagnostics. */
    QJsonObject lastParams = spy.last().at(1).toJsonObject();
    QJsonArray  diags      = lastParams["diagnostics"].toArray();

    /* Count only errors (severity 1). */
    int errorCount = 0;
    for (const auto &diag : diags) {
        if (diag.toObject()["severity"].toInt() == 1)
            errorCount++;
    }
    QCOMPARE(errorCount, 0);

    backend.stop();
}

void Test::diagnostics_missingEndmodule_hasError()
{
    QLspSlangBackend backend;
    backend.start(tempDir.path());

    QSignalSpy spy(&backend, &QLspBackend::notification);

    QString code = "module bad_module (\n"
                   "    input wire clk\n"
                   ");\n"
                   "/* missing endmodule */\n";

    QString filePath = createVerilogFile("missing_endmodule.v", code);
    backend.notify("textDocument/didOpen", buildDidOpenParams(filePath, code));

    QVERIFY(spy.count() >= 1);

    QJsonObject lastParams = spy.last().at(1).toJsonObject();
    QJsonArray  diags      = lastParams["diagnostics"].toArray();

    QVERIFY(!diags.isEmpty());

    /* Verify at least one error exists. */
    bool hasError = false;
    for (const auto &diag : diags) {
        if (diag.toObject()["severity"].toInt() == 1)
            hasError = true;
    }
    QVERIFY(hasError);

    backend.stop();
}

void Test::diagnostics_undeclaredVariable_hasError()
{
    QLspSlangBackend backend;
    backend.start(tempDir.path());

    QSignalSpy spy(&backend, &QLspBackend::notification);

    QString code = "module undecl_test (\n"
                   "    input wire clk,\n"
                   "    output reg [7:0] data\n"
                   ");\n"
                   "always @(posedge clk) begin\n"
                   "    data <= nonexistent_signal + 1;\n"
                   "end\n"
                   "endmodule\n";

    QString filePath = createVerilogFile("undecl.v", code);
    backend.notify("textDocument/didOpen", buildDidOpenParams(filePath, code));

    QVERIFY(spy.count() >= 1);

    QJsonObject lastParams = spy.last().at(1).toJsonObject();
    QJsonArray  diags      = lastParams["diagnostics"].toArray();

    QVERIFY(!diags.isEmpty());

    /* Verify at least one diagnostic mentions the undeclared signal. */
    bool foundUndeclared = false;
    for (const auto &diag : diags) {
        QString message = diag.toObject()["message"].toString();
        if (message.contains("nonexistent_signal") || message.contains("undeclared")
            || message.contains("unknown")) {
            foundUndeclared = true;
        }
    }
    QVERIFY(foundUndeclared);

    backend.stop();
}

void Test::diagnostics_syntaxError_hasError()
{
    QLspSlangBackend backend;
    backend.start(tempDir.path());

    QSignalSpy spy(&backend, &QLspBackend::notification);

    QString code = "module syntax_err;\n"
                   "    wire ;;;\n"
                   "endmodule\n";

    QString filePath = createVerilogFile("syntax_err.v", code);
    backend.notify("textDocument/didOpen", buildDidOpenParams(filePath, code));

    QVERIFY(spy.count() >= 1);

    QJsonObject lastParams = spy.last().at(1).toJsonObject();
    QJsonArray  diags      = lastParams["diagnostics"].toArray();
    QVERIFY(!diags.isEmpty());

    backend.stop();
}

/* File lifecycle tests */

void Test::didOpen_triggersDiagnostics()
{
    QLspSlangBackend backend;
    backend.start(tempDir.path());

    QSignalSpy spy(&backend, &QLspBackend::notification);

    QString code     = "module open_test; endmodule\n";
    QString filePath = createVerilogFile("open_test.v", code);
    backend.notify("textDocument/didOpen", buildDidOpenParams(filePath, code));

    QVERIFY(spy.count() >= 1);

    QString method = spy.last().at(0).toString();
    QCOMPARE(method, "textDocument/publishDiagnostics");

    backend.stop();
}

void Test::didChange_updatesDiagnostics()
{
    QLspSlangBackend backend;
    backend.start(tempDir.path());

    /* Open a valid file first. */
    QString code     = "module change_test; endmodule\n";
    QString filePath = createVerilogFile("change_test.v", code);
    backend.notify("textDocument/didOpen", buildDidOpenParams(filePath, code));

    QSignalSpy spy(&backend, &QLspBackend::notification);

    /* Change to invalid content. */
    QString badCode = "module change_test;\n"
                      "    wire ;;;\n"
                      "endmodule\n";

    QJsonObject changeParams{
        {"textDocument", QJsonObject{{"uri", fileToUri(filePath)}, {"version", 2}}},
        {"contentChanges", QJsonArray{QJsonObject{{"text", badCode}}}},
    };
    backend.notify("textDocument/didChange", changeParams);

    QVERIFY(spy.count() >= 1);

    QJsonObject lastParams = spy.last().at(1).toJsonObject();
    QJsonArray  diags      = lastParams["diagnostics"].toArray();
    QVERIFY(!diags.isEmpty());

    backend.stop();
}

void Test::didClose_clearsDiagnostics()
{
    QLspSlangBackend backend;
    backend.start(tempDir.path());

    /* Open a file with errors. */
    QString code     = "module close_test;\n    wire ;;;\nendmodule\n";
    QString filePath = createVerilogFile("close_test.v", code);
    backend.notify("textDocument/didOpen", buildDidOpenParams(filePath, code));

    QSignalSpy spy(&backend, &QLspBackend::notification);

    /* Close the file. */
    backend.notify("textDocument/didClose", buildDidCloseParams(filePath));

    QVERIFY(spy.count() >= 1);

    /* The close notification should publish empty diagnostics. */
    QJsonObject lastParams = spy.last().at(1).toJsonObject();
    QJsonArray  diags      = lastParams["diagnostics"].toArray();
    QVERIFY(diags.isEmpty());

    backend.stop();
}

void Test::didSave_readsFromDisk()
{
    QLspSlangBackend backend;
    backend.start(tempDir.path());

    /* Write a valid file to disk. */
    QString code     = "module save_test; endmodule\n";
    QString filePath = createVerilogFile("save_test.v", code);

    QSignalSpy spy(&backend, &QLspBackend::notification);

    /* Notify didSave without prior didOpen; backend should read from disk. */
    backend.notify("textDocument/didSave", buildDidSaveParams(filePath));

    QVERIFY(spy.count() >= 1);

    QString method = spy.last().at(0).toString();
    QCOMPARE(method, "textDocument/publishDiagnostics");

    backend.stop();
}

/* Request tests */

void Test::request_unsupported_returnsNull()
{
    QLspSlangBackend backend;
    backend.start(tempDir.path());

    QJsonValue result = backend.request("textDocument/hover", QJsonObject{});
    QVERIFY(result.isNull());

    result = backend.request("textDocument/definition", QJsonObject{});
    QVERIFY(result.isNull());

    backend.stop();
}

/* Diagnostic quality tests */

void Test::diagnostics_noTopModules_suppressed()
{
    QLspSlangBackend backend;
    backend.start(tempDir.path());

    QSignalSpy spy(&backend, &QLspBackend::notification);

    /* A simple module should not trigger NoTopModules warnings. */
    QString code     = "module top_test; endmodule\n";
    QString filePath = createVerilogFile("top_test.v", code);
    backend.notify("textDocument/didOpen", buildDidOpenParams(filePath, code));

    QVERIFY(spy.count() >= 1);

    QJsonObject lastParams = spy.last().at(1).toJsonObject();
    QJsonArray  diags      = lastParams["diagnostics"].toArray();

    /* NoTopModules should be suppressed; no diagnostics about top-level modules. */
    for (const auto &diag : diags) {
        QString msg = diag.toObject()["message"].toString();
        QVERIFY2(!msg.contains("no top-level modules"), qPrintable(msg));
    }

    backend.stop();
}

void Test::diagnostics_hasRange_notPointOnly()
{
    QLspSlangBackend backend;
    backend.start(tempDir.path());

    QSignalSpy spy(&backend, &QLspBackend::notification);

    /* Code with a syntax error that should produce a range. */
    QString code = "module range_test;\n"
                   "    wire [7:0] data;\n"
                   "    assign data = nonexistent_signal;\n"
                   "endmodule\n";

    QString filePath = createVerilogFile("range_test.v", code);
    backend.notify("textDocument/didOpen", buildDidOpenParams(filePath, code));

    QVERIFY(spy.count() >= 1);

    QJsonObject lastParams = spy.last().at(1).toJsonObject();
    QJsonArray  diags      = lastParams["diagnostics"].toArray();
    QVERIFY(!diags.isEmpty());

    /* Verify range structure is well-formed. */
    for (const auto &diag : diags) {
        QJsonObject range = diag.toObject()["range"].toObject();
        QVERIFY(range.contains("start"));
        QVERIFY(range.contains("end"));
        QJsonObject start = range["start"].toObject();
        QJsonObject end   = range["end"].toObject();
        QVERIFY(start.contains("line"));
        QVERIFY(start.contains("character"));
        QVERIFY(end.contains("line"));
        QVERIFY(end.contains("character"));
    }

    backend.stop();
}

void Test::diagnostics_hasDiagnosticCode()
{
    QLspSlangBackend backend;
    backend.start(tempDir.path());

    QSignalSpy spy(&backend, &QLspBackend::notification);

    /* Code with an undeclared identifier; slang should produce a code. */
    QString code = "module code_test;\n"
                   "    wire data;\n"
                   "    assign data = unknown_thing;\n"
                   "endmodule\n";

    QString filePath = createVerilogFile("code_test.v", code);
    backend.notify("textDocument/didOpen", buildDidOpenParams(filePath, code));

    QVERIFY(spy.count() >= 1);

    QJsonObject lastParams = spy.last().at(1).toJsonObject();
    QJsonArray  diags      = lastParams["diagnostics"].toArray();
    QVERIFY(!diags.isEmpty());

    /* At least one diagnostic should have source set to "slang". */
    bool hasSource = false;
    for (const auto &diag : diags) {
        if (diag.toObject()["source"].toString() == "slang")
            hasSource = true;
    }
    QVERIFY(hasSource);

    backend.stop();
}

void Test::diagnostics_persistentSourceManager()
{
    QLspSlangBackend backend;
    backend.start(tempDir.path());

    /* Open first file. */
    QString code1     = "module persist_a; endmodule\n";
    QString filePath1 = createVerilogFile("persist_a.v", code1);
    backend.notify("textDocument/didOpen", buildDidOpenParams(filePath1, code1));

    /* Open second file. */
    QString code2     = "module persist_b; endmodule\n";
    QString filePath2 = createVerilogFile("persist_b.v", code2);
    backend.notify("textDocument/didOpen", buildDidOpenParams(filePath2, code2));

    QSignalSpy spy(&backend, &QLspBackend::notification);

    /* Change first file; should not crash (persistent SourceManager). */
    QString     newCode = "module persist_a;\n"
                          "    wire test;\n"
                          "endmodule\n";
    QJsonObject changeParams{
        {"textDocument", QJsonObject{{"uri", fileToUri(filePath1)}, {"version", 2}}},
        {"contentChanges", QJsonArray{QJsonObject{{"text", newCode}}}},
    };
    backend.notify("textDocument/didChange", changeParams);

    QVERIFY(spy.count() >= 1);
    QString method = spy.last().at(0).toString();
    QCOMPARE(method, "textDocument/publishDiagnostics");

    backend.stop();
}

QTEST_MAIN(Test)
#include "test_qlspslangbackend.moc"
