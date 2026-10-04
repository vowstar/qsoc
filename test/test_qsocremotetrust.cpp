// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/remote/qsocagentremote.h"
#include "agent/remote/qsocsftpclient.h"
#include "agent/remote/qsocsshsession.h"
#include "qsoc_test.h"
#include "qsoc_test_sshd.h"

#include <QDir>
#include <QFile>
#include <QtTest>

/*
 * What a remote binding trusts before it uses anything the host sends: the
 * host key it accepts and what it records about it. Every case connects to a
 * loopback sshd through the production connect path, with HOME redirected to
 * the fixture so the ssh config and known_hosts are written at runtime and the
 * real ~/.ssh is never read or written.
 */

class Test : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();
    void cleanup();
    void strictYesRefusesAnUnknownKey();
    void acceptNewSavesTheKeyForTheNextConnect();
    void anUnsetPolicyRefusesAnUnknownKeyWithoutAFrontend();
    void anUnsetPolicyAsksAndSavesOnlyAConfirmedKey();
    void noPolicyWarnsAndSavesNothing();
    void aChangedKeyIsRefusedUnderEveryPolicy_data();
    void aChangedKeyIsRefusedUnderEveryPolicy();
    void aKnownKeyAfterAnUnparsableLineIsStillKnown();
    void aRevokedKeyIsRefusedUnderEveryPolicy_data();
    void aRevokedKeyIsRefusedUnderEveryPolicy();

private:
    static constexpr const char *kAlias = "trust-box";

    QString    home() const { return m_fixture.root() + QStringLiteral("/client_home"); }
    QString    knownHosts() const { return home() + QStringLiteral("/.ssh/known_hosts"); }
    QByteArray hostEntryName() const
    {
        return QByteArrayLiteral("[127.0.0.1]:") + QByteArray::number(m_fixture.port());
    }

    /** @brief Write ~/.ssh/config for kAlias with @p policy lines appended. */
    bool writeConfig(const QString &policy) const
    {
        QFile config(home() + QStringLiteral("/.ssh/config"));
        if (!config.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
            return false;
        }
        const QByteArray text = QStringLiteral(
                                    "Host %1\n"
                                    "  HostName 127.0.0.1\n"
                                    "  Port %2\n"
                                    "  User %3\n"
                                    "  IdentityFile %4\n"
                                    "  IdentitiesOnly yes\n")
                                    .arg(QString::fromLatin1(kAlias))
                                    .arg(m_fixture.port())
                                    .arg(m_fixture.user(), m_fixture.keyPath())
                                    .toUtf8()
                                + policy.toUtf8();
        return config.write(text) == text.size();
    }

    /** @brief Connect kAlias once; the transport is released before returning. */
    bool dial(
        QString *error, QSocSshSession::HostKeyConfirm confirm = {}, QStringList *notices = nullptr)
    {
        QObject          parent;
        AgentRemoteState state;
        const bool       ok = connectAgentSshSession(
            QString::fromLatin1(kAlias),
            &parent,
            &state,
            error,
            {},
            {},
            QDeadlineTimer(30000),
            confirm);
        if (notices != nullptr) {
            *notices = state.hostKeyNotices;
        }
        discardAgentRemoteState(&state);
        return ok;
    }

    /** @brief A known_hosts line for the fixture's address holding another key. */
    QByteArray foreignKeyLine() const
    {
        return hostEntryName() + ' ' + slurp(m_fixture.keyPath() + QStringLiteral(".pub"));
    }

    bool writeKnownHosts(const QByteArray &content) const
    {
        QFile file(knownHosts());
        return file.open(QIODevice::WriteOnly | QIODevice::Truncate)
               && file.write(content) == content.size();
    }

    static QByteArray slurp(const QString &path)
    {
        QFile file(path);
        return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
    }

    QSocTestSshd m_fixture;
    QByteArray   m_oldHome;
    bool         m_hadHome = false;
};

void Test::initTestCase()
{
    m_fixture.start();
    QVERIFY(QDir().mkpath(home() + QStringLiteral("/.ssh")));
    m_hadHome = qEnvironmentVariableIsSet("HOME");
    m_oldHome = qgetenv("HOME");
    QVERIFY(qputenv("HOME", home().toUtf8()));
    QCOMPARE(QDir::homePath(), home());
}

void Test::cleanupTestCase()
{
    if (m_hadHome) {
        qputenv("HOME", m_oldHome);
    } else {
        qunsetenv("HOME");
    }
    m_fixture.stop();
    QVERIFY2(m_fixture.removeRoot(), "the fixture root could not be removed");
}

void Test::cleanup()
{
    QFile::remove(knownHosts());
    QFile::remove(home() + QStringLiteral("/.ssh/config"));
}

/* Counterexample: every target was dialled with accept-new forced on, so a
 * user's `StrictHostKeyChecking yes` let an unknown host through. */
void Test::strictYesRefusesAnUnknownKey()
{
    QSOC_REQUIRE_SSHD(m_fixture);
    QVERIFY(writeConfig(QStringLiteral("  StrictHostKeyChecking yes\n")));

    QString error;
    QVERIFY2(!dial(&error), "an unknown host key was accepted under StrictHostKeyChecking yes");
    QVERIFY2(error.contains(QStringLiteral("known_hosts")), qPrintable(error));
    QVERIFY(!QFile::exists(knownHosts()));
}

/* Counterexample: accept-new admitted the key but never wrote it, so every
 * later connect was a first contact again. */
void Test::acceptNewSavesTheKeyForTheNextConnect()
{
    QSOC_REQUIRE_SSHD(m_fixture);
    QVERIFY(writeConfig(QStringLiteral("  StrictHostKeyChecking accept-new\n")));

    QString error;
    QVERIFY2(dial(&error), qPrintable(error));
    const QByteArray saved = slurp(knownHosts());
    QVERIFY2(saved.startsWith(hostEntryName() + ' '), saved.constData());

    /* The saved line must be one a strict check accepts. */
    QVERIFY(writeConfig(QStringLiteral("  StrictHostKeyChecking yes\n")));
    QVERIFY2(dial(&error), qPrintable(error));
    QCOMPARE(slurp(knownHosts()), saved);
}

/* OpenSSH's default is ask; with nobody to ask, the answer is no. */
void Test::anUnsetPolicyRefusesAnUnknownKeyWithoutAFrontend()
{
    QSOC_REQUIRE_SSHD(m_fixture);
    QVERIFY(writeConfig(QString()));

    QString error;
    QVERIFY2(!dial(&error), "an unknown host key was accepted with nobody to confirm it");
    QVERIFY2(error.contains(QStringLiteral("accept-new")), qPrintable(error));
    QVERIFY(!QFile::exists(knownHosts()));
}

void Test::anUnsetPolicyAsksAndSavesOnlyAConfirmedKey()
{
    QSOC_REQUIRE_SSHD(m_fixture);
    QVERIFY(writeConfig(QString()));

    QString prompt;
    QString error;
    QVERIFY(!dial(&error, [&](const QString &text) {
        prompt = text;
        return false;
    }));
    QVERIFY2(prompt.contains(QString::fromLatin1(hostEntryName())), qPrintable(prompt));
    QVERIFY2(prompt.contains(QStringLiteral("SHA256:")), qPrintable(prompt));
    QVERIFY(!QFile::exists(knownHosts()));

    QStringList notices;
    QVERIFY2(dial(&error, [](const QString &) { return true; }, &notices), qPrintable(error));
    QVERIFY(slurp(knownHosts()).startsWith(hostEntryName() + ' '));
    QCOMPARE(notices.size(), 1);
    const QString saved = notices.value(0);
    QVERIFY2(saved.contains(knownHosts()), qPrintable(saved));

    /* Known now, so nobody is asked again. */
    bool asked = false;
    QVERIFY2(dial(&error, [&](const QString &) { return asked = true; }), qPrintable(error));
    QVERIFY(!asked);
}

void Test::noPolicyWarnsAndSavesNothing()
{
    QSOC_REQUIRE_SSHD(m_fixture);
    QVERIFY(writeConfig(QStringLiteral("  StrictHostKeyChecking no\n")));

    QString     error;
    QStringList notices;
    QVERIFY2(dial(&error, {}, &notices), qPrintable(error));
    QCOMPARE(notices.size(), 1);
    const QString warning = notices.value(0);
    QVERIFY2(warning.startsWith(QStringLiteral("Warning:")), qPrintable(warning));
    QVERIFY(!QFile::exists(knownHosts()));
}

void Test::aChangedKeyIsRefusedUnderEveryPolicy_data()
{
    QTest::addColumn<QString>("policy");
    QTest::newRow("yes") << QStringLiteral("  StrictHostKeyChecking yes\n");
    QTest::newRow("ask") << QString();
    QTest::newRow("accept-new") << QStringLiteral("  StrictHostKeyChecking accept-new\n");
    QTest::newRow("no") << QStringLiteral("  StrictHostKeyChecking no\n");
}

void Test::aChangedKeyIsRefusedUnderEveryPolicy()
{
    QSOC_REQUIRE_SSHD(m_fixture);
    QFETCH(QString, policy);
    QVERIFY(writeConfig(policy));
    const QByteArray recorded = foreignKeyLine();
    QVERIFY(writeKnownHosts(recorded));

    QString error;
    bool    asked = false;
    QVERIFY2(
        !dial(&error, [&](const QString &) { return asked = true; }),
        "a host whose key changed was accepted");
    QVERIFY(!asked);
    QVERIFY2(error.contains(QStringLiteral("changed")), qPrintable(error));
    QCOMPARE(slurp(knownHosts()), recorded);
}

void Test::aKnownKeyAfterAnUnparsableLineIsStillKnown()
{
    QSOC_REQUIRE_SSHD(m_fixture);
    QVERIFY(writeConfig(QStringLiteral("  StrictHostKeyChecking accept-new\n")));
    QString error;
    QVERIFY2(dial(&error), qPrintable(error));
    const QByteArray saved = slurp(knownHosts());

    /* A line libssh2 cannot represent must not hide the keys after it. */
    QVERIFY(writeKnownHosts(QByteArrayLiteral("@cert-authority * ssh-rsa AAAA\n") + saved));
    QVERIFY(writeConfig(QStringLiteral("  StrictHostKeyChecking yes\n")));
    QVERIFY2(dial(&error), qPrintable(error));
}

void Test::aRevokedKeyIsRefusedUnderEveryPolicy_data()
{
    aChangedKeyIsRefusedUnderEveryPolicy_data();
}

/* Counterexample: marker lines were skipped, so a key the user revoked still
 * matched its ordinary line and connected. */
void Test::aRevokedKeyIsRefusedUnderEveryPolicy()
{
    QSOC_REQUIRE_SSHD(m_fixture);
    QFETCH(QString, policy);
    QVERIFY(writeConfig(policy));
    const QByteArray hostKey  = slurp(m_fixture.root() + QStringLiteral("/host_rsa.pub"));
    const QByteArray recorded = "@revoked * " + hostKey + hostEntryName() + ' ' + hostKey;
    QVERIFY(writeKnownHosts(recorded));

    QString error;
    bool    asked = false;
    QVERIFY2(
        !dial(&error, [&](const QString &) { return asked = true; }),
        "a revoked host key was accepted");
    QVERIFY(!asked);
    QVERIFY2(error.contains(QStringLiteral("revoked")), qPrintable(error));
    QCOMPARE(slurp(knownHosts()), recorded);
}

QSOC_TEST_MAIN(Test)
#include "test_qsocremotetrust.moc"
