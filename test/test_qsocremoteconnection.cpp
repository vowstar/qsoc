// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/remote/qsocagentremote.h"
#include "agent/remote/qsocsftpclient.h"
#include "agent/remote/qsocsshsession.h"
#include "qsoc_test.h"

#include <QtCore>
#include <QtTest>

#include <type_traits>
#include <utility>

namespace {

class Test : public QObject
{
    Q_OBJECT

private:
    /* A transport the test owns. Sessions are parented to `scratch` so the
     * number of live ones is observable, which is how the ownership
     * assertions below are made without instrumenting production code.
     * Carries no path: the connection seeds that from the workspace. */
    static AgentRemoteState fakeTransport(
        QObject *scratch, const QString &target, const QString &workspace)
    {
        AgentRemoteState state;
        state.session   = new QSocSshSession(scratch);
        state.sftp      = new QSocSftpClient(*state.session);
        state.targetKey = target;
        state.workspace = workspace;
        return state;
    }

    /* A rebuilder that always produces a real transport. */
    static QSocRemoteConnection::Rebuilder rebuilderOn(QObject *scratch, int *builds = nullptr)
    {
        return
            [scratch, builds](
                const QString &target, const QString &workspace, AgentRemoteState *out, QString *) {
                if (builds != nullptr) {
                    ++(*builds);
                }
                *out = fakeTransport(scratch, target, workspace);
                return true;
            };
    }

private slots:
    /* A reconnect must free the transport it replaces. adopt() overwrites the
     * pointers, so a missing teardown leaks one session, its SFTP channel and
     * its whole ProxyJump chain on every successful reconnect. */
    void reconnectFreesTheTransportItReplaces()
    {
        QObject              scratch;
        QSocRemoteConnection conn;
        QVERIFY(conn.adopt(fakeTransport(&scratch, QStringLiteral("u@h:22"), QStringLiteral("/w"))));
        QCOMPARE(scratch.children().size(), 1);

        int builds = 0;
        conn.setRebuilder(rebuilderOn(&scratch, &builds));

        QString err;
        QCOMPARE(conn.reconnect(&err), QSocRemoteConnection::ReconnectOutcome::Reconnected);
        QCOMPARE(builds, 1);
        QCOMPARE(scratch.children().size(), 1);

        /* Repeating it must not accumulate either. A second reconnect belongs
         * to a later turn, and a turn gets one: driving the object directly
         * there is no turn boundary to supply it. */
        conn.resetReconnectBudget();
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
        QVERIFY(conn.adopt(fakeTransport(&scratch, QStringLiteral("u@h:22"), QStringLiteral("/w"))));
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
        QVERIFY(noBuilder.adopt(
            fakeTransport(&scratch, QStringLiteral("u@h:22"), QStringLiteral("/w"))));
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
        QVERIFY(conn.adopt(fakeTransport(&scratch, QStringLiteral("u@h:22"), QStringLiteral("/w"))));
        conn.path()
            ->readState()
            .recordRead(QStringLiteral("/w/a.sv"), QStringLiteral("module a; endmodule"));
        QVERIFY(conn.path()->readState().wasRead(QStringLiteral("/w/a.sv")));

        conn.setRebuilder(rebuilderOn(&scratch));

        QString err;
        QCOMPARE(conn.reconnect(&err), QSocRemoteConnection::ReconnectOutcome::Reconnected);
        QVERIFY(!conn.path()->readState().wasRead(QStringLiteral("/w/a.sv")));
        /* The binding's identity survives; only beliefs about content do not. */
        QCOMPARE(conn.target(), QStringLiteral("u@h:22"));
        QCOMPARE(conn.workspace(), QStringLiteral("/w"));
    }

    /* An unconnected session is not usable, so it is rebuilt rather than
     * left alone. NotNeeded needs a genuinely live session and is covered
     * against a real sshd, not here. */
    void reconnectRebuildsAnUnconnectedSession()
    {
        QObject              scratch;
        QSocRemoteConnection conn;
        QCOMPARE(conn.isUsable(), false);
        QVERIFY(conn.adopt(fakeTransport(&scratch, QStringLiteral("u@h:22"), QStringLiteral("/w"))));

        int builds = 0;
        conn.setRebuilder(rebuilderOn(&scratch, &builds));
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
        QVERIFY(conn.adopt(fakeTransport(&scratch, QStringLiteral("u@h:22"), QStringLiteral("/w"))));
        conn.teardown();
        QCOMPARE(scratch.children().size(), 0);
        QVERIFY(conn.target().isEmpty());
        QCOMPARE(conn.session(), static_cast<QSocSshSession *>(nullptr));
    }

    /* The remote path has exactly one home. A staging bundle carries none, so
     * the connection is what seeds root, cwd and the writable set from the
     * workspace it adopted. Two containers would mean /cwd writes one and the
     * tools read the other. */
    void theConnectionOwnsTheOnlyPathContext()
    {
        QObject              scratch;
        QSocRemoteConnection conn;
        QVERIFY(conn.adopt(
            fakeTransport(&scratch, QStringLiteral("u@h:22"), QStringLiteral("/srv/work"))));
        QCOMPARE(conn.path()->root(), QStringLiteral("/srv/work"));
        QCOMPARE(conn.path()->cwd(), QStringLiteral("/srv/work"));
        QCOMPARE(conn.path()->writableDirs(), QStringList{QStringLiteral("/srv/work")});
        QCOMPARE(conn.display(), QStringLiteral("u@h:22:/srv/work"));
    }

    /* Every consumer holds a QSocRemoteConnection*, so a copy would double-free
     * the transport and a move would dangle all of them at once. */
    void theConnectionCannotBeCopiedOrMoved()
    {
        static_assert(!std::is_copy_constructible_v<QSocRemoteConnection>);
        static_assert(!std::is_copy_assignable_v<QSocRemoteConnection>);
        static_assert(!std::is_move_constructible_v<QSocRemoteConnection>);
        static_assert(!std::is_move_assignable_v<QSocRemoteConnection>);
        QVERIFY(!std::is_copy_constructible_v<QSocRemoteConnection>);
    }

    /* The generation names one transport. A holder that captured a stale one
     * has to be able to tell, which needs a counter that only ever moves
     * forward. */
    void generationCountsEveryTransportEverBound()
    {
        QObject              scratch;
        QSocRemoteConnection conn;
        QCOMPARE(conn.generation(), QSocRemoteConnection::Generation{0});
        QVERIFY(!conn.isCurrent(0));

        conn.setDirectoryProbe([](QSocSftpClient *, const QString &) { return true; });
        QVERIFY(conn.adopt(fakeTransport(&scratch, QStringLiteral("u@h:22"), QStringLiteral("/w"))));
        QCOMPARE(conn.generation(), QSocRemoteConnection::Generation{1});
        QVERIFY(conn.isCurrent(1));

        int builds = 0;
        conn.setRebuilder(rebuilderOn(&scratch, &builds));
        QString err;
        QCOMPARE(conn.reconnect(&err), QSocRemoteConnection::ReconnectOutcome::Reconnected);
        QCOMPARE(conn.generation(), QSocRemoteConnection::Generation{2});
        QVERIFY(conn.isCurrent(2));
        QVERIFY(!conn.isCurrent(1));

        /* A later turn, so a fresh budget. */
        conn.resetReconnectBudget();
        QCOMPARE(conn.reconnect(&err), QSocRemoteConnection::ReconnectOutcome::Reconnected);
        QCOMPARE(conn.generation(), QSocRemoteConnection::Generation{3});
        QVERIFY(conn.isCurrent(3));
        QVERIFY(!conn.isCurrent(2));
        QVERIFY(!conn.isCurrent(1));
        QCOMPARE(builds, 2);
    }

    /* An attempt that produced nothing bound nothing, so the counter must not
     * move: a holder from before the attempt is still current. */
    void aFailedReconnectDoesNotBumpGeneration()
    {
        QObject              scratch;
        QSocRemoteConnection conn;
        QVERIFY(conn.adopt(fakeTransport(&scratch, QStringLiteral("u@h:22"), QStringLiteral("/w"))));
        QCOMPARE(conn.generation(), QSocRemoteConnection::Generation{1});

        conn.setRebuilder([](const QString &, const QString &, AgentRemoteState *, QString *why) {
            *why = QStringLiteral("connect refused");
            return false;
        });
        QString err;
        QCOMPARE(conn.reconnect(&err), QSocRemoteConnection::ReconnectOutcome::Exhausted);
        QCOMPARE(conn.generation(), QSocRemoteConnection::Generation{1});
        QVERIFY(conn.isCurrent(1));

        /* A rebuilder that reports success without producing a transport is
         * refused, and that is not a bind either. */
        conn.setRebuilder(
            [](const QString &, const QString &, AgentRemoteState *, QString *) { return true; });
        /* A later turn, so a fresh budget. */
        conn.resetReconnectBudget();
        QCOMPARE(conn.reconnect(&err), QSocRemoteConnection::ReconnectOutcome::Exhausted);
        QCOMPARE(conn.generation(), QSocRemoteConnection::Generation{1});
    }

    /* teardown() must not rewind the counter. Rewinding would let the next
     * bind hand out a generation a stale holder still remembers, and that
     * holder would read itself as current. */
    void teardownDoesNotRewindGeneration()
    {
        QObject              scratch;
        QSocRemoteConnection conn;
        QVERIFY(conn.adopt(fakeTransport(&scratch, QStringLiteral("u@h:22"), QStringLiteral("/w"))));
        QCOMPARE(conn.generation(), QSocRemoteConnection::Generation{1});

        conn.teardown();
        QCOMPARE(conn.generation(), QSocRemoteConnection::Generation{1});

        QVERIFY(conn.adopt(fakeTransport(&scratch, QStringLiteral("u@h:22"), QStringLiteral("/w"))));
        QCOMPARE(conn.generation(), QSocRemoteConnection::Generation{2});
        QVERIFY(!conn.isCurrent(1));
        QVERIFY(conn.isCurrent(2));
    }

    /* Without a destructor every connection leaks its whole transport: the CLI
     * one, the loop's own, and one per cached host binding. */
    void aDestroyedConnectionClosesItsTransport()
    {
        QObject scratch;
        {
            QSocRemoteConnection conn;
            QVERIFY(conn.adopt(
                fakeTransport(&scratch, QStringLiteral("u@h:22"), QStringLiteral("/w"))));
            QCOMPARE(scratch.children().size(), 1);
        }
        QCOMPARE(scratch.children().size(), 0);
    }

    /* A half-built bundle must be refused without being consumed, so the
     * caller still owns what it passed and the transport being replaced has
     * not been freed for nothing. */
    void adoptRefusesAnIncompleteTransport()
    {
        QObject              scratch;
        QSocRemoteConnection conn;

        /* Reading a source whose move was refused is the property under test. */
        // cppcheck-suppress-begin accessMoved
        AgentRemoteState noSession;
        noSession.workspace = QStringLiteral("/w");
        QVERIFY(!conn.adopt(std::move(noSession)));
        QCOMPARE(conn.generation(), QSocRemoteConnection::Generation{0});
        QCOMPARE(noSession.workspace, QStringLiteral("/w"));

        AgentRemoteState noSftp;
        noSftp.session   = new QSocSshSession(&scratch);
        noSftp.workspace = QStringLiteral("/w");
        QVERIFY(!conn.adopt(std::move(noSftp)));
        QVERIFY(noSftp.session != nullptr);
        discardAgentRemoteState(&noSftp);

        AgentRemoteState noWorkspace = fakeTransport(&scratch, QStringLiteral("u@h:22"), QString());
        QVERIFY(!conn.adopt(std::move(noWorkspace)));
        QVERIFY(noWorkspace.session != nullptr);
        QVERIFY(noWorkspace.sftp != nullptr);
        discardAgentRemoteState(&noWorkspace);
        // cppcheck-suppress-end accessMoved

        QCOMPARE(conn.generation(), QSocRemoteConnection::Generation{0});
        QVERIFY(conn.path()->root().isEmpty());
        QCOMPARE(scratch.children().size(), 0);
    }

    /* A working directory the host does not confirm must not be kept. Keeping
     * one that is gone points every later relative path at a directory that
     * does not exist, and the host may have rebooted out from under it. */
    void reconnectFallsBackToRootWhenTheWorkingDirectoryIsGone()
    {
        QObject              scratch;
        QSocRemoteConnection conn;
        conn.setDirectoryProbe([](QSocSftpClient *, const QString &) { return false; });
        QVERIFY(conn.adopt(fakeTransport(&scratch, QStringLiteral("u@h:22"), QStringLiteral("/w"))));
        conn.path()->setCwd(QStringLiteral("/w/sub"));
        QCOMPARE(conn.path()->cwd(), QStringLiteral("/w/sub"));

        conn.setRebuilder(rebuilderOn(&scratch));
        QString err;
        QCOMPARE(conn.reconnect(&err), QSocRemoteConnection::ReconnectOutcome::Reconnected);
        QCOMPARE(conn.path()->cwd(), QStringLiteral("/w"));
        QCOMPARE(conn.path()->root(), QStringLiteral("/w"));
        QVERIFY(!conn.lastReconnectKeptCwd());
    }

    /* A working directory that is still there survives, and so does the
     * writable set: both are paths, not handles, and re-seeding them would
     * silently move the user's session back to the workspace root. */
    void reconnectKeepsTheWorkingDirectoryTheHostConfirms()
    {
        QObject              scratch;
        QSocRemoteConnection conn;
        QStringList          asked;
        conn.setDirectoryProbe([&asked](QSocSftpClient *, const QString &dir) {
            asked.append(dir);
            return true;
        });
        QVERIFY(conn.adopt(fakeTransport(&scratch, QStringLiteral("u@h:22"), QStringLiteral("/w"))));
        conn.path()->setCwd(QStringLiteral("/w/sub"));
        conn.path()->setWritableDirs({QStringLiteral("/w"), QStringLiteral("/w/extra")});

        conn.setRebuilder(rebuilderOn(&scratch));
        QString err;
        QCOMPARE(conn.reconnect(&err), QSocRemoteConnection::ReconnectOutcome::Reconnected);
        QCOMPARE(conn.path()->cwd(), QStringLiteral("/w/sub"));
        QCOMPARE(
            conn.path()->writableDirs(),
            QStringList({QStringLiteral("/w"), QStringLiteral("/w/extra")}));
        QVERIFY(conn.lastReconnectKeptCwd());
        /* Verified, not assumed. */
        QCOMPARE(asked, QStringList{QStringLiteral("/w/sub")});
    }

    /* A background job's outcome is only unknown relative to the transport it
     * was launched over, so the ledger has to stamp each id with that
     * transport. It must also allocate nothing: the connection is a value
     * member of long-lived owners. */
    void backgroundJobsAreStampedWithTheGenerationTheyStartedUnder()
    {
        QObject              scratch;
        QSocRemoteConnection conn;
        conn.setDirectoryProbe([](QSocSftpClient *, const QString &) { return true; });
        QVERIFY(conn.adopt(fakeTransport(&scratch, QStringLiteral("u@h:22"), QStringLiteral("/w"))));

        QSocRemoteJobRecord first;
        first.jobId      = QStringLiteral("job-a");
        first.pid        = 4242;
        first.generation = conn.generation();
        QVERIFY(conn.jobs()->note(first));
        QCOMPARE(conn.jobs()->record(first.jobId).generation, QSocRemoteConnection::Generation{1});
        QVERIFY(!conn.jobs()->has(QStringLiteral("nope")));

        conn.setRebuilder(rebuilderOn(&scratch));
        QString err;
        QCOMPARE(conn.reconnect(&err), QSocRemoteConnection::ReconnectOutcome::Reconnected);
        QCOMPARE(conn.generation(), QSocRemoteConnection::Generation{2});
        /* Survives the swap: the job may still be running, and the agent is
         * told to verify it rather than assume either way. */
        QCOMPARE(conn.jobs()->record(first.jobId).generation, QSocRemoteConnection::Generation{1});
        QVERIFY(conn.jobs()->liveJobIds().contains(first.jobId));

        /* A job id names a process on one host under one workspace, so a swap
         * to a different binding must not carry it across. adopt() does not
         * free what it replaces, so a swap is teardown-then-adopt, which is
         * the order the /ssh handler uses. */
        conn.teardown();
        QVERIFY(conn.adopt(
            fakeTransport(&scratch, QStringLiteral("u@other:22"), QStringLiteral("/w"))));
        QVERIFY2(conn.jobs()->liveJobIds().isEmpty(), "a job id survived a swap to a different host");

        /* The ledger is a plain value; only the transport's session is a
         * QObject. */
        QCOMPARE(scratch.children().size(), 1);
    }

    /* COUNTEREXAMPLE. A local working tree needs no probe, so nothing bound
     * is not a reason to refuse a rewind. The shipped gate asked isUsable(),
     * which is false whenever no session is bound, so every files-mode
     * rewind in local mode (and in any session that ran /local after /ssh)
     * took the refusal branch after the conversation had already been
     * rewritten: the user lost the conversation, no file was restored, and
     * the screen said "cancelled". */
    void rewindRefusalIsEmptyForALocalTree()
    {
        /* scratch outlives the connection: it parents the session the
         * connection frees. */
        QObject              scratch;
        QSocRemoteConnection unboundConn;
        QCOMPARE(remoteWorkspaceRewindRefusal(&unboundConn, 500), QString());
        QCOMPARE(remoteWorkspaceRewindRefusal(nullptr, 500), QString());

        /* A binding that went away is a different answer: the tree is remote,
         * so an unusable link must still refuse. */
        QVERIFY(unboundConn.adopt(
            fakeTransport(&scratch, QStringLiteral("u@h:22"), QStringLiteral("/w"))));
        QVERIFY(!unboundConn.isUsable());
        QVERIFY(!remoteWorkspaceRewindRefusal(&unboundConn, 500).isEmpty());

        /* And /local unbinds it again, which is local, not unusable. */
        unboundConn.teardown();
        QCOMPARE(remoteWorkspaceRewindRefusal(&unboundConn, 500), QString());
    }

    /* A connect result nobody adopted still owns a live transport. Every
     * failure path between connect and adopt has to be able to free it
     * without inventing a throwaway connection to do it. */
    void discardingAnUnadoptedConnectResultFreesIt()
    {
        QObject          scratch;
        AgentRemoteState state
            = fakeTransport(&scratch, QStringLiteral("u@h:22"), QStringLiteral("/w"));
        QCOMPARE(scratch.children().size(), 1);

        discardAgentRemoteState(&state);
        QCOMPARE(scratch.children().size(), 0);
        QCOMPARE(state.session, static_cast<QSocSshSession *>(nullptr));
        QCOMPARE(state.sftp, static_cast<QSocSftpClient *>(nullptr));
        QVERIFY(state.jumps.isEmpty());

        /* Idempotent, so a path that already discarded can discard again. */
        discardAgentRemoteState(&state);
        QCOMPARE(scratch.children().size(), 0);
        discardAgentRemoteState(nullptr);
    }
};

} // namespace

QSOC_TEST_MAIN(Test)
#include "test_qsocremoteconnection.moc"
