// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/remote/qsocsshpubderive.h"
#include "qsoc_test.h"

#include <QDir>
#include <QFile>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtCore>
#include <QtTest>

struct TestApp
{
    static auto &instance()
    {
        static auto                   argc      = 1;
        static char                   appName[] = "qsoc";
        static std::array<char *, 1>  argv      = {{appName}};
        static const QCoreApplication app       = QCoreApplication(argc, argv.data());
        return app;
    }
};

class Test : public QObject
{
    Q_OBJECT

private:
    /* Every key is generated on the spot inside a QTemporaryDir and
     * disappears when the test exits. Nothing cryptographic is ever
     * committed to the repository. */
    static bool generateKey(const QString &dir, const QString &name, const QStringList &keygenArgs)
    {
        const QString sshKeygen = QStandardPaths::findExecutable(QStringLiteral("ssh-keygen"));
        if (sshKeygen.isEmpty()) {
            return false;
        }
        QProcess    process;
        QStringList args = keygenArgs;
        args.append(QStringLiteral("-N"));
        args.append(QString());
        args.append(QStringLiteral("-f"));
        args.append(QDir(dir).absoluteFilePath(name));
        process.setProgram(sshKeygen);
        process.setArguments(args);
        process.start();
        if (!process.waitForFinished(15000)) {
            process.kill();
            process.waitForFinished();
            return false;
        }
        return process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
    }

    /* Read the .pub that ssh-keygen wrote; this is the ground-truth we
     * compare our derivation against. Strip the trailing comment field
     * so the match does not depend on the default user@host suffix. */
    static QString readPubTruth(const QString &privPath)
    {
        QFile file(privPath + QStringLiteral(".pub"));
        if (!file.open(QIODevice::ReadOnly)) {
            return {};
        }
        const QByteArray raw = file.readAll().trimmed();
        file.close();
        const QList<QByteArray> parts = raw.split(' ');
        if (parts.size() < 2) {
            return {};
        }
        return QString::fromLatin1(parts.at(0)) + QLatin1Char(' ')
               + QString::fromLatin1(parts.at(1));
    }

    static QString stripComment(const QString &line)
    {
        const QStringList parts = line.split(QLatin1Char(' '));
        if (parts.size() < 2) {
            return line;
        }
        return parts.at(0) + QLatin1Char(' ') + parts.at(1);
    }

    static bool haveSshKeygen()
    {
        return !QStandardPaths::findExecutable(QStringLiteral("ssh-keygen")).isEmpty();
    }

private slots:
    void initTestCase() { TestApp::instance(); }

    void rsaPemRoundTrip()
    {
        if (!haveSshKeygen()) {
            QSKIP("ssh-keygen not available, skipping.");
        }
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        QVERIFY(generateKey(
            tmp.path(),
            QStringLiteral("priv"),
            {QStringLiteral("-t"),
             QStringLiteral("rsa"),
             QStringLiteral("-b"),
             QStringLiteral("2048"),
             QStringLiteral("-m"),
             QStringLiteral("PEM")}));
        const QString priv = QDir(tmp.path()).absoluteFilePath(QStringLiteral("priv"));
        const QString want = readPubTruth(priv);
        QVERIFY(!want.isEmpty());
        QVERIFY(QFile::remove(priv + QStringLiteral(".pub")));
        const QString got = stripComment(QSocSshPubDerive::fromPrivateKeyFile(priv));
        QCOMPARE(got, want);
    }

    void ecdsaP256RoundTrip()
    {
        if (!haveSshKeygen()) {
            QSKIP("ssh-keygen not available, skipping.");
        }
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        QVERIFY(generateKey(
            tmp.path(),
            QStringLiteral("priv"),
            {QStringLiteral("-t"),
             QStringLiteral("ecdsa"),
             QStringLiteral("-b"),
             QStringLiteral("256"),
             QStringLiteral("-m"),
             QStringLiteral("PEM")}));
        const QString priv = QDir(tmp.path()).absoluteFilePath(QStringLiteral("priv"));
        const QString want = readPubTruth(priv);
        QVERIFY(!want.isEmpty());
        QVERIFY(QFile::remove(priv + QStringLiteral(".pub")));
        const QString got = stripComment(QSocSshPubDerive::fromPrivateKeyFile(priv));
        QCOMPARE(got, want);
    }

    void ed25519OpensshContainer()
    {
        if (!haveSshKeygen()) {
            QSKIP("ssh-keygen not available, skipping.");
        }
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        QVERIFY(generateKey(
            tmp.path(), QStringLiteral("priv"), {QStringLiteral("-t"), QStringLiteral("ed25519")}));
        const QString priv = QDir(tmp.path()).absoluteFilePath(QStringLiteral("priv"));
        const QString want = readPubTruth(priv);
        QVERIFY(!want.isEmpty());
        QVERIFY(QFile::remove(priv + QStringLiteral(".pub")));
        const QString got = stripComment(QSocSshPubDerive::fromPrivateKeyFile(priv));
        QCOMPARE(got, want);
    }

    void encryptedKeyIsRefused()
    {
        if (!haveSshKeygen()) {
            QSKIP("ssh-keygen not available, skipping.");
        }
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const QString sshKeygen = QStandardPaths::findExecutable(QStringLiteral("ssh-keygen"));
        QProcess      process;
        process.setProgram(sshKeygen);
        process.setArguments(
            {QStringLiteral("-t"),
             QStringLiteral("ed25519"),
             QStringLiteral("-N"),
             QStringLiteral("correcthorsebatterystaple"),
             QStringLiteral("-f"),
             QDir(tmp.path()).absoluteFilePath(QStringLiteral("priv"))});
        process.start();
        QVERIFY(process.waitForFinished(15000));
        QCOMPARE(process.exitCode(), 0);
        const QString priv = QDir(tmp.path()).absoluteFilePath(QStringLiteral("priv"));
        QVERIFY(QFile::remove(priv + QStringLiteral(".pub")));
        const QString got = QSocSshPubDerive::fromPrivateKeyFile(priv);
        QVERIFY(got.isEmpty());
    }

    void garbageInputReturnsEmpty()
    {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const QString priv = QDir(tmp.path()).absoluteFilePath(QStringLiteral("garbage"));
        QFile         file(priv);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(QByteArray(256, '\x42'));
        file.close();
        QVERIFY(QSocSshPubDerive::fromPrivateKeyFile(priv).isEmpty());
    }

    void missingFileReturnsEmpty()
    {
        QVERIFY(QSocSshPubDerive::fromPrivateKeyFile(QStringLiteral("/no/such/path")).isEmpty());
    }
};

QTEST_APPLESS_MAIN(Test)
#include "test_qsocsshpubderive.moc"
