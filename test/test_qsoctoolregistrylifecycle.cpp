// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/qsoctool.h"
#include "qsoc_test.h"

#include <nlohmann/json.hpp>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEvent>
#include <QEventLoop>
#include <QPointer>
#include <QProcess>
#include <QThread>
#include <QTimer>
#include <QtTest>

#include <functional>
#include <new>
#include <thread>
#include <utility>

using json = nlohmann::json;

namespace {

class NameCallbackTool final : public QSocTool
{
public:
    explicit NameCallbackTool(QString name = QStringLiteral("probe"), QObject *parent = nullptr)
        : QSocTool(parent)
        , name_(std::move(name))
    {}

    QString getName() const override
    {
        const QString &name     = name_;
        const auto     callback = std::exchange(callback_, {});
        if (callback) {
            callback();
        }
        return name;
    }

    QString getDescription() const override { return QStringLiteral("Name callback probe"); }
    json    getParametersSchema() const override { return {{"type", "object"}}; }
    QString execute(const json &) override { return QStringLiteral("executed"); }

    void setCallback(std::function<void()> callback) { callback_ = std::move(callback); }

private:
    QString                       name_;
    mutable std::function<void()> callback_;
};

class OutputTool : public QSocTool
{
public:
    QString getName() const override { return QStringLiteral("output_probe"); }
    QString getDescription() const override { return QStringLiteral("Output probe"); }
    json    getParametersSchema() const override { return {{"type", "object"}}; }
    QString execute(const json &) override
    {
        QPointer<QSocToolCallContext> context(currentCallContext());
        context->reportOutput(QStringLiteral("first"));
        if (!context.isNull())
            context->reportOutput(QStringLiteral("second"));
        return QStringLiteral("executed");
    }
};

class RequestContextTool final : public QSocTool
{
public:
    QString getName() const override { return QStringLiteral("request_context"); }
    QString getDescription() const override { return QStringLiteral("Request context probe"); }
    json    getParametersSchema() const override { return {{"type", "object"}}; }
    QString execute(const json &) override
    {
        const auto options = imageAttachmentOptions();
        effort             = options.reasoningEffort;
        action();
        return options.stop.stop_requested() && currentCallContext()->isCancellationRequested()
                   ? QStringLiteral("cancelled")
                   : QStringLiteral("active");
    }
    QString               effort;
    std::function<void()> action;
};

class ProcessWaitTool final : public QSocTool
{
public:
    QString getName() const override { return QStringLiteral("process_wait"); }
    QString getDescription() const override { return QStringLiteral("Generated process wait"); }
    json    getParametersSchema() const override { return {{"type", "object"}}; }
    QString execute(const json &) override
    {
        auto *context = currentCallContext();
        if (context->isCancellationRequested())
            return QStringLiteral("cancelled before start");
        QProcess process;
        auto     environment = QProcessEnvironment::systemEnvironment();
        environment.insert(QStringLiteral("QSOC_TEST_PROCESS_WAIT"), QStringLiteral("1"));
        process.setProcessEnvironment(environment);
        QEventLoop loop;
        QTimer     deadline;
        deadline.setSingleShot(true);
        QElapsedTimer elapsed;
        std::jthread  worker;
        connect(context, &QSocToolCallContext::cancellationRequested, &loop, [&] {
            ++signalCount;
            process.kill();
        });
        connect(&process, &QProcess::finished, &loop, &QEventLoop::quit);
        connect(&process, &QProcess::errorOccurred, &loop, &QEventLoop::quit);
        connect(&deadline, &QTimer::timeout, &loop, [&] { process.kill(); });
        connect(&process, &QProcess::started, &loop, [&] {
            elapsed.start();
            deadline.start(2000);
            if (crossThread) {
                worker = std::jthread([stop = stop] {
                    std::this_thread::sleep_for(std::chrono::milliseconds(40));
                    stop->request_stop();
                });
            } else {
                QTimer::singleShot(40, &loop, [this] { stop->request_stop(); });
            }
        });
        process.start(QCoreApplication::applicationFilePath(), {QStringLiteral("processWaitChild")});
        loop.exec();
        elapsedMs = elapsed.isValid() ? elapsed.elapsed() : -1;
        return context->isCancellationRequested() && process.state() == QProcess::NotRunning
                   ? QStringLiteral("cancelled")
                   : QStringLiteral("failed");
    }
    std::stop_source *stop        = nullptr;
    bool              crossThread = false;
    int               signalCount = 0;
    qint64            elapsedMs   = -1;
};

class TestToolRegistryLifecycle : public QObject
{
    Q_OBJECT

private slots:
    void processWaitChild()
    {
        if (qEnvironmentVariableIntValue("QSOC_TEST_PROCESS_WAIT") == 1)
            QTest::qWait(10000);
    }

    void parentStopWakesProcessWait_data()
    {
        QTest::addColumn<bool>("crossThread");
        QTest::newRow("same-thread") << false;
        QTest::newRow("cross-thread") << true;
    }

    void parentStopWakesProcessWait()
    {
        QFETCH(bool, crossThread);
        std::stop_source stop;
        ProcessWaitTool  tool;
        tool.stop        = &stop;
        tool.crossThread = crossThread;
        QSocToolRegistry registry;
        registry.registerTool(&tool);
        QCOMPARE(
            registry.executeTool("process_wait", {}, nullptr, {}, {}, "high", stop.get_token()),
            QStringLiteral("cancelled"));
        QCOMPARE(tool.signalCount, 1);
        QVERIFY(tool.elapsedMs >= 0 && tool.elapsedMs < 1500);
    }

    void queuedParentStopDoesNotOutliveContext()
    {
        std::stop_source   stop;
        RequestContextTool tool;
        tool.action = [&] { stop.request_stop(); };
        QSocToolRegistry registry;
        registry.registerTool(&tool);
        QCOMPARE(
            registry.executeTool("request_context", {}, nullptr, {}, {}, "high", stop.get_token()),
            QStringLiteral("cancelled"));
        QCoreApplication::processEvents();
        QCOMPARE(tool.effort, QStringLiteral("high"));
    }

    void imageRequestContextInheritsEffortAndCancellation_data()
    {
        QTest::addColumn<bool>("parentStop");
        QTest::newRow("parent-stop") << true;
        QTest::newRow("registry-stop") << false;
    }

    void imageRequestContextInheritsEffortAndCancellation()
    {
        QFETCH(bool, parentStop);
        QSocToolRegistry   registry;
        RequestContextTool tool;
        QObject            owner;
        std::stop_source   stop;
        tool.action = [&] {
            if (parentStop)
                stop.request_stop();
            else
                registry.abortCalls(&owner);
        };
        registry.registerTool(&tool);
        QCOMPARE(
            registry.executeTool("request_context", {}, &owner, {}, {}, "high", stop.get_token()),
            QStringLiteral("cancelled"));
        QCOMPARE(tool.effort, QStringLiteral("high"));
    }

    void nestedOutputKeepsItsOwnerAndStopsAfterCancellation()
    {
        QSocToolRegistry registry;
        registry.registerTool(new OutputTool);
        QObject     outer;
        QObject     inner;
        QStringList outerOutput;
        QStringList innerOutput;
        registry.executeTool("output_probe", {}, &outer, [&](const QString &text) {
            outerOutput.append(text);
            registry.executeTool("output_probe", {}, &inner, [&](const QString &part) {
                innerOutput.append(part);
            });
            registry.abortCalls(&outer);
        });
        QCOMPARE(outerOutput, QStringList({"first"}));
        QCOMPARE(innerOutput, QStringList({"first", "second"}));
    }

    void toolNameCallbackDeletion_data()
    {
        QTest::addColumn<bool>("deleteRegistry");
        QTest::newRow("tool") << false;
        QTest::newRow("registry") << true;
    }

    void toolNameCallbackDeletion()
    {
        QFETCH(bool, deleteRegistry);

        auto                      *registry = new QSocToolRegistry;
        QPointer<QSocToolRegistry> registryGuard(registry);
        if (deleteRegistry) {
            NameCallbackTool tool;
            tool.setCallback([registry]() { delete registry; });
            registry->registerTool(&tool);
            QVERIFY(registryGuard.isNull());
            return;
        }

        auto              *tool = new NameCallbackTool;
        QPointer<QSocTool> toolGuard(tool);
        tool->setCallback([tool]() { delete tool; });
        registry->registerTool(tool);
        QVERIFY(toolGuard.isNull());
        QCOMPARE(registry->count(), 0);
        delete registry;
    }

    void unregistrationDoesNotReadToolMetadata()
    {
        QSocToolRegistry registry;
        NameCallbackTool tool;
        registry.registerTool(&tool);
        bool nameRead = false;
        tool.setCallback([&nameRead]() { nameRead = true; });

        QVERIFY(registry.unregisterTool(&tool));

        QVERIFY(!nameRead);
        QCOMPARE(registry.count(), 0);
    }

    void reentrantToolRegistrationKeepsNewerEntry()
    {
        QSocToolRegistry registry;
        NameCallbackTool newer;
        NameCallbackTool older;
        older.setCallback([&]() { registry.registerTool(&newer); });

        registry.registerTool(&older);

        QCOMPARE(registry.count(), 1);
        QCOMPARE(registry.getTool(QStringLiteral("probe")), &newer);
    }

    void unrelatedReentrantRegistrationKeepsBothEntries()
    {
        QSocToolRegistry registry;
        NameCallbackTool inner(QStringLiteral("inner"));
        NameCallbackTool outer(QStringLiteral("outer"));
        outer.setCallback([&]() { registry.registerTool(&inner); });

        registry.registerTool(&outer);

        QCOMPARE(registry.count(), 2);
        QCOMPARE(registry.getTool(QStringLiteral("inner")), &inner);
        QCOMPARE(registry.getTool(QStringLiteral("outer")), &outer);
    }

    void queuedDestroyedCallbackKeepsReusedAddress()
    {
        QSocToolRegistry registry;
        void            *storage = ::operator new(sizeof(NameCallbackTool));
        auto            *oldTool = new (storage) NameCallbackTool;
        registry.registerTool(oldTool);
        auto *worker = QThread::create([oldTool]() { oldTool->~NameCallbackTool(); });
        oldTool->moveToThread(worker);
        worker->start();
        const bool stopped = worker->wait();
        delete worker;

        auto *replacement = new (storage) NameCallbackTool;
        registry.registerTool(replacement);
        QCoreApplication::sendPostedEvents(&registry, QEvent::MetaCall);
        const bool kept = registry.getTool(QStringLiteral("probe")) == replacement;
        replacement->~NameCallbackTool();
        ::operator delete(storage);

        QVERIFY(stopped);
        QVERIFY(kept);
    }
};

} // namespace

QSOC_TEST_MAIN(TestToolRegistryLifecycle)
#include "test_qsoctoolregistrylifecycle.moc"
