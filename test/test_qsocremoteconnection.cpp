// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/remote/qsocagentremote.h"
#include "agent/remote/qsocsftpclient.h"
#include "agent/remote/qsocsshsession.h"
#include "qsoc_test.h"

#include <QtCore>
#include <QtTest>

namespace {

class Test : public QObject
{
    Q_OBJECT

private:
    /* A transport the test owns. Sessions are parented to `scratch` so the
     * number of live ones is observable, which is how the ownership
     * assertions below are made without instrumenting production code. */
    static AgentRemoteState fakeTransport(
        QObject *scratch, const QString &target, const QString &workspace)
    {
        AgentRemoteState state;
        state.session   = new QSocSshSession(scratch);
        state.sftp      = new QSocSftpClient(*state.session);
        state.targetKey = target;
        state.workspace = workspace;
        state.path.setRoot(workspace);
        return state;
    }

private slots:
    /* A reconnect must free the transport it replaces. adopt() overwrites the
     * pointers, so a missing teardown leaks one session, its SFTP channel and
     * its whole ProxyJump chain on every successful reconnect. */
    void reconnectFreesTheTransportItReplaces()
    {
        QObject              scratch;
        QSocRemoteConnection conn;
        conn.adopt(fakeTransport(&scratch, QStringLiteral("u@h:22"), QStringLiteral("/w")));
        QCOMPARE(scratch.children().size(), 1);

        int builds = 0;
        conn.setRebuilder(
            [&scratch, &builds](
                const QString &target, const QString &workspace, AgentRemoteState *out, QString *) {
                ++builds;
                *out = fakeTransport(&scratch, target, workspace);
                return true;
            });

        QString err;
        QCOMPARE(conn.reconnect(&err), QSocRemoteConnection::ReconnectOutcome::Reconnected);
        QCOMPARE(builds, 1);
        QCOMPARE(scratch.children().size(), 1);

        /* Repeating it must not accumulate either. */
        QCOMPARE(conn.reconnect(&err), QSocRemoteConnection::ReconnectOutcome::Reconnected);
        QCOMPARE(builds, 2);
        QCOMPARE(scratch.children().size(), 1);
    }

    /* A failed attempt must leave the caller exactly as it was: the previous
     * transport is still the only one, and it is still bound. */
    void aFailedReconnectKeepsThePreviousTransport()
    {
        QObject              scratch;
        QSocRemoteConnection conn;
        conn.adopt(fakeTransport(&scratch, QStringLiteral("u@h:22"), QStringLiteral("/w")));
        QSocSshSession *before = conn.session();

        int builds = 0;
        conn.setRebuilder(
            [&builds](const QString &, const QString &, AgentRemoteState *, QString *why) {
                ++builds;
                *why = QStringLiteral("connect refused");
                return false;
            });

        QString err;
        QCOMPARE(conn.reconnect(&err), QSocRemoteConnection::ReconnectOutcome::Exhausted);
        QCOMPARE(err, QStringLiteral("connect refused"));
        /* Bounded: it must not retry forever. */
        QVERIFY(builds >= 1);
        QVERIFY(builds <= 4);
        QCOMPARE(conn.lastReconnectAttempts(), builds);
        QCOMPARE(conn.session(), before);
        QCOMPARE(scratch.children().size(), 1);
    }

    /* Nothing to reconnect from is a refusal, not an attempt. */
    void reconnectRefusesWithoutSomethingToRebuildFrom()
    {
        QObject              scratch;
        QSocRemoteConnection bare;
        QString              err;
        QCOMPARE(bare.reconnect(&err), QSocRemoteConnection::ReconnectOutcome::Refused);

        /* Bound, but no rebuilder installed. */
        QSocRemoteConnection noBuilder;
        noBuilder.adopt(fakeTransport(&scratch, QStringLiteral("u@h:22"), QStringLiteral("/w")));
        QCOMPARE(noBuilder.reconnect(&err), QSocRemoteConnection::ReconnectOutcome::Refused);

        /* A rebuilder but no identity to rebuild from. */
        QSocRemoteConnection noIdentity;
        noIdentity.setRebuilder(
            [](const QString &, const QString &, AgentRemoteState *, QString *) { return true; });
        QCOMPARE(noIdentity.reconnect(&err), QSocRemoteConnection::ReconnectOutcome::Refused);
    }

    /* A reconnect must forget every believed file content. The
     * read-before-overwrite guard keys off this, so leaving it populated is
     * what would let an edit land on a stale baseline. */
    void reconnectForgetsBelievedFileContents()
    {
        QObject              scratch;
        QSocRemoteConnection conn;
        conn.adopt(fakeTransport(&scratch, QStringLiteral("u@h:22"), QStringLiteral("/w")));
        conn.path()
            ->readState()
            .recordRead(QStringLiteral("/w/a.sv"), QStringLiteral("module a; endmodule"));
        QVERIFY(conn.path()->readState().wasRead(QStringLiteral("/w/a.sv")));

        conn.setRebuilder(
            [&scratch](
                const QString &target, const QString &workspace, AgentRemoteState *out, QString *) {
                *out = fakeTransport(&scratch, target, workspace);
                return true;
            });

        QString err;
        QCOMPARE(conn.reconnect(&err), QSocRemoteConnection::ReconnectOutcome::Reconnected);
        QVERIFY(!conn.path()->readState().wasRead(QStringLiteral("/w/a.sv")));
        /* The binding's identity survives; only beliefs about content do not. */
        QCOMPARE(conn.target(), QStringLiteral("u@h:22"));
        QCOMPARE(conn.workspace(), QStringLiteral("/w"));
    }

    /* A usable workspace is left alone. */
    void reconnectDoesNothingWhileTheWorkspaceIsUsable()
    {
        QObject              scratch;
        QSocRemoteConnection conn;
        QCOMPARE(conn.isUsable(), false);
        conn.adopt(fakeTransport(&scratch, QStringLiteral("u@h:22"), QStringLiteral("/w")));

        int builds = 0;
        conn.setRebuilder(
            [&builds](const QString &, const QString &, AgentRemoteState *, QString *) {
                ++builds;
                return true;
            });
        /* An unconnected session is not usable, so this fixture cannot assert
         * NotNeeded directly; what it can assert is that the outcome is never
         * silently "fine" while isUsable() says otherwise. */
        QString    err;
        const auto outcome = conn.reconnect(&err);
        QVERIFY(outcome != QSocRemoteConnection::ReconnectOutcome::NotNeeded);
        QCOMPARE(builds, 1);
    }

    /* teardown() must be safe with nothing bound, and must clear identity. */
    void teardownIsSafeAndClearsIdentity()
    {
        QSocRemoteConnection bare;
        bare.teardown();
        QVERIFY(bare.target().isEmpty());
        QVERIFY(bare.workspace().isEmpty());
        QVERIFY(!bare.isUsable());

        QObject              scratch;
        QSocRemoteConnection conn;
        conn.adopt(fakeTransport(&scratch, QStringLiteral("u@h:22"), QStringLiteral("/w")));
        conn.teardown();
        QCOMPARE(scratch.children().size(), 0);
        QVERIFY(conn.target().isEmpty());
        QCOMPARE(conn.session(), static_cast<QSocSshSession *>(nullptr));
    }
};

} // namespace

QSOC_TEST_MAIN(Test)
#include "test_qsocremoteconnection.moc"
