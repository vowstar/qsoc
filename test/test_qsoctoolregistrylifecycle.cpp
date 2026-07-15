// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/qsoctool.h"
#include "qsoc_test.h"

#include <nlohmann/json.hpp>

#include <QCoreApplication>
#include <QEvent>
#include <QPointer>
#include <QThread>
#include <QtTest>

#include <functional>
#include <new>
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
        const QString name     = name_;
        const auto    callback = std::exchange(callback_, {});
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

class TestToolRegistryLifecycle : public QObject
{
    Q_OBJECT

private slots:
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
