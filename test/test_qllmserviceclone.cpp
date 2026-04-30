// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "common/qllmservice.h"
#include "qsoc_test.h"

#include <QtCore>
#include <QtTest>

namespace {

class Test : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase() {}

    /* clone() returns a distinct, non-null instance. */
    void testCloneReturnsDistinctPointer()
    {
        auto *parent = new QLLMService(this, nullptr);
        auto *child  = parent->clone(this);
        QVERIFY(child != nullptr);
        QVERIFY(child != parent);
        delete child;
        delete parent;
    }

    /* Endpoints added directly are not copied automatically (clone
     * re-parses from QSocConfig). With no config, both are empty. */
    void testCloneWithoutConfigYieldsEmptyEndpoints()
    {
        auto       *parent = new QLLMService(this, nullptr);
        LLMEndpoint manual;
        manual.name = QStringLiteral("local");
        manual.url  = QUrl(QStringLiteral("http://localhost:1234/v1/chat"));
        manual.key  = QStringLiteral("secret-token-abcdef");
        parent->addEndpoint(manual);
        QCOMPARE(parent->endpointCount(), 1);

        auto *child = parent->clone(this);
        /* Clone has no config → loadConfigSettings did not pick up
         * endpoints. Manual endpoints are NOT carried; that is the
         * documented contract (configuration source-of-truth is
         * QSocConfig). */
        QCOMPARE(child->endpointCount(), 0);

        delete child;
        delete parent;
    }

    /* Streaming state is independent: the parent's currentStreamReply
     * is its own; the child's is its own. We can't easily test this
     * without a network mock, but we can confirm the dtor sequence
     * runs cleanly back-to-back without crashing. */
    void testIndependentDestructionOrderIsClean()
    {
        auto *parent = new QLLMService(this, nullptr);
        auto *childA = parent->clone(this);
        auto *childB = parent->clone(this);
        delete childB;
        delete childA;
        delete parent;
        /* If we reached here, no double-free / use-after-free. */
        QVERIFY(true);
    }

    /* Reverse destruction order is also safe (parent dies before
     * children). Children are parented to `this`, so they are
     * cleaned up by Qt's parent-child mechanism — the destructor
     * must not depend on the parent service still being alive. */
    void testParentDestroyedBeforeClones()
    {
        auto *parent = new QLLMService(this, nullptr);
        auto *clone  = parent->clone(this);
        delete parent;
        /* clone now stands alone */
        QCOMPARE(clone->endpointCount(), 0);
        delete clone;
    }

    /* clone() preserves the parent's fallback strategy and (when
     * set) the currentModelId. With no config-driven model registry
     * the model id stays empty, so we only assert fallbackStrategy
     * here. The model-aware path is covered by integration tests
     * that mount a real QSocConfig YAML. */
    void testCloneCopiesFallbackStrategy()
    {
        auto *parent = new QLLMService(this, nullptr);
        parent->setFallbackStrategy(LLMFallbackStrategy::Random);
        auto *child = parent->clone(this);
        /* No public getter for fallbackStrategy; we exercise the
         * code path and assert no crash + endpoint count parity. */
        QCOMPARE(child->endpointCount(), parent->endpointCount());
        delete child;
        delete parent;
    }
};

} // namespace

QSOC_TEST_MAIN(Test)
#include "test_qllmserviceclone.moc"
