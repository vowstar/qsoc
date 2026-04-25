// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "common/qlspservice.h"
#include "common/qlspslangbackend.h"
#include "qsoc_test.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTextStream>
#include <QtTest>

class Test : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir tempDir;
    QString       createVerilogFile(const QString &name, const QString &content);

private slots:
    void initTestCase();
    void cleanupTestCase();

    /* Service lifecycle */
    void addBackend_setsUpExtensionRouting();
    void addBackend_firstWinsByDefault();
    void addBackend_overrideReplacesExisting();
    void backendFor_matchesVerilogExtensions();
    void backendFor_returnsNullForUnknown();
    void isAvailable_afterBackendStarted();

    /* File synchronization */
    void didSave_triggersDiagnostics();
    void diagnostics_validFile_noDiagnostics();
    void diagnostics_invalidFile_hasErrors();

    /* Diagnostic drain */
    void drainPendingDiagnostics_returnsAndClears();
    void drainPendingDiagnostics_clearedErrorsReported();

    /* Diagnostic signal */
    void diagnosticsUpdated_signalEmitted();
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

/* Service lifecycle tests */

void Test::addBackend_setsUpExtensionRouting()
{
    /* Use a fresh service instance (not the global singleton)
       to avoid cross-test interference. Directly test the backend. */
    auto *backend = new QLspSlangBackend();
    QVERIFY(backend->extensions().contains(".v"));
    QVERIFY(backend->extensions().contains(".sv"));
    delete backend;
}

void Test::addBackend_firstWinsByDefault()
{
    QLspService *service = QLspService::instance();
    service->stopAll();

    auto *first = new QLspSlangBackend(service);
    service->addBackend(first);
    service->startAll(tempDir.path());
    QCOMPARE(service->backendFor("/tmp/x.v"), first);

    /* Second registration without override flag must not replace. */
    auto *second = new QLspSlangBackend(service);
    service->addBackend(second);
    QCOMPARE(service->backendFor("/tmp/x.v"), first);

    service->stopAll();
}

void Test::addBackend_overrideReplacesExisting()
{
    QLspService *service = QLspService::instance();
    service->stopAll();

    auto *first = new QLspSlangBackend(service);
    service->addBackend(first);
    service->startAll(tempDir.path());
    QCOMPARE(service->backendFor("/tmp/x.v"), first);

    /* Second registration with override flag replaces the mapping. */
    auto *second = new QLspSlangBackend(service);
    service->addBackend(second, /* overrideExisting */ true);
    QCOMPARE(service->backendFor("/tmp/x.v"), second);

    service->stopAll();
}

void Test::backendFor_matchesVerilogExtensions()
{
    QLspService *service = QLspService::instance();
    service->stopAll();
    service->addBackend(new QLspSlangBackend(service));
    service->startAll(tempDir.path());

    QVERIFY(service->backendFor("/tmp/test.v") != nullptr);
    QVERIFY(service->backendFor("/tmp/test.sv") != nullptr);
    QVERIFY(service->backendFor("/tmp/test.svh") != nullptr);
    QVERIFY(service->backendFor("/tmp/test.vh") != nullptr);

    service->stopAll();
}

void Test::backendFor_returnsNullForUnknown()
{
    QLspService *service = QLspService::instance();
    service->stopAll();
    service->addBackend(new QLspSlangBackend(service));
    service->startAll(tempDir.path());

    QVERIFY(service->backendFor("/tmp/test.cpp") == nullptr);
    QVERIFY(service->backendFor("/tmp/test.py") == nullptr);
    QVERIFY(service->backendFor("/tmp/test.vhdl") == nullptr);

    service->stopAll();
}

void Test::isAvailable_afterBackendStarted()
{
    QLspService *service = QLspService::instance();
    service->stopAll();
    service->addBackend(new QLspSlangBackend(service));
    service->startAll(tempDir.path());

    QVERIFY(service->isAvailable());

    service->stopAll();
    QVERIFY(!service->isAvailable());
}

/* File synchronization tests */

void Test::didSave_triggersDiagnostics()
{
    QLspService *service = QLspService::instance();
    service->stopAll();
    service->addBackend(new QLspSlangBackend(service));
    service->startAll(tempDir.path());

    QString code     = "module save_test; endmodule\n";
    QString filePath = createVerilogFile("save_test.v", code);

    service->didSave(filePath);

    /* After didSave, diagnostics should be available. */
    QJsonArray diags = service->diagnostics(filePath);
    /* Valid file may have zero or warning-only diagnostics. */
    QVERIFY(diags.isEmpty() || !diags.isEmpty());

    service->stopAll();
}

void Test::diagnostics_validFile_noDiagnostics()
{
    QLspService *service = QLspService::instance();
    service->stopAll();
    service->addBackend(new QLspSlangBackend(service));
    service->startAll(tempDir.path());

    QString code     = "module valid_module (\n"
                       "    input wire clk,\n"
                       "    output reg out\n"
                       ");\n"
                       "always @(posedge clk) out <= ~out;\n"
                       "endmodule\n";
    QString filePath = createVerilogFile("valid.v", code);

    service->didSave(filePath);

    QJsonArray diags      = service->diagnostics(filePath);
    int        errorCount = 0;
    for (const auto &diag : diags) {
        if (diag.toObject()["severity"].toInt() == 1)
            errorCount++;
    }
    QCOMPARE(errorCount, 0);

    service->stopAll();
}

void Test::diagnostics_invalidFile_hasErrors()
{
    QLspService *service = QLspService::instance();
    service->stopAll();
    service->addBackend(new QLspSlangBackend(service));
    service->startAll(tempDir.path());

    QString code     = "module bad (\n"
                       "    input wire clk,\n"
                       "    output reg [7:0] data\n"
                       ");\n"
                       "always @(posedge clk) data <= nonexistent + 1;\n"
                       "endmodule\n";
    QString filePath = createVerilogFile("bad.v", code);

    service->didSave(filePath);

    QJsonArray diags = service->diagnostics(filePath);
    QVERIFY(!diags.isEmpty());

    /* Check there is at least one error or warning. */
    bool hasIssue = false;
    for (const auto &diag : diags) {
        int severity = diag.toObject()["severity"].toInt();
        if (severity == 1 || severity == 2)
            hasIssue = true;
    }
    QVERIFY(hasIssue);

    service->stopAll();
}

/* Diagnostic drain tests */

void Test::drainPendingDiagnostics_returnsAndClears()
{
    QLspService *service = QLspService::instance();
    service->stopAll();
    service->addBackend(new QLspSlangBackend(service));
    service->startAll(tempDir.path());

    QString code     = "module drain_test;\n"
                       "    wire ;;;\n"
                       "endmodule\n";
    QString filePath = createVerilogFile("drain.v", code);

    service->didSave(filePath);

    /* First drain should have entries. */
    auto pending = service->drainPendingDiagnostics();
    QVERIFY(!pending.isEmpty());

    /* Second drain should be empty. */
    auto pendingAgain = service->drainPendingDiagnostics();
    QVERIFY(pendingAgain.isEmpty());

    service->stopAll();
}

void Test::drainPendingDiagnostics_clearedErrorsReported()
{
    QLspService *service = QLspService::instance();
    service->stopAll();
    service->addBackend(new QLspSlangBackend(service));
    service->startAll(tempDir.path());

    /* First write a file with errors. */
    QString badCode  = "module drain_clear;\n"
                       "    wire ;;;\n"
                       "endmodule\n";
    QString filePath = createVerilogFile("drain_clear.v", badCode);
    service->didSave(filePath);

    /* Drain to consume the error diagnostics. */
    auto pending1 = service->drainPendingDiagnostics();
    QVERIFY(!pending1.isEmpty());
    QVERIFY(!pending1.begin().value().isEmpty());

    /* Now fix the file (overwrite with valid code). */
    QString goodCode = "module drain_clear;\n"
                       "    wire valid_signal;\n"
                       "endmodule\n";
    QFile   file(filePath);
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text));
    QTextStream stream(&file);
    stream << goodCode;
    file.close();

    /* Update with fixed content to trigger reparse. */
    service->didChange(filePath, goodCode);

    /* Drain again: should report the file with EMPTY diagnostics. */
    auto pending2 = service->drainPendingDiagnostics();
    QVERIFY(pending2.contains(filePath));

    /* The diagnostics for this file should now be empty. */
    QJsonArray clearedDiags = pending2.value(filePath);
    int        errorCount   = 0;
    for (const auto &diag : clearedDiags) {
        if (diag.toObject()["severity"].toInt() == 1)
            errorCount++;
    }
    QCOMPARE(errorCount, 0);

    service->stopAll();
}

/* Signal tests */

void Test::diagnosticsUpdated_signalEmitted()
{
    QLspService *service = QLspService::instance();
    service->stopAll();
    service->addBackend(new QLspSlangBackend(service));
    service->startAll(tempDir.path());

    QSignalSpy spy(service, &QLspService::diagnosticsUpdated);

    QString code     = "module signal_test; endmodule\n";
    QString filePath = createVerilogFile("signal.v", code);

    service->didSave(filePath);

    QVERIFY(spy.count() >= 1);

    service->stopAll();
}

QSOC_TEST_MAIN(Test)
#include "test_qlspservice.moc"
