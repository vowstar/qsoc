// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "common/qsocpaths.h"
#include "qsoc_test.h"

#include <QDir>
#include <QtTest>

class TestQSocPaths : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void envRoot_unsetReturnsEmpty();
    void envRoot_setReturnsValue();

    void projectRoot_emptyInputReturnsEmpty();
    void projectRoot_appendsDotQsoc();

    void userRoot_defaultsToHomeDotConfig();
    void userRoot_honorsXdgConfigHome();

    void systemRoot_platformSpecific();

    void resourceDirs_orderAndContents();
    void resourceDirs_emptySubdirReturnsRoots();
    void resourceDirs_dedupesIdenticalRoots();
    void resourceDirs_skipsEmptyLayers();

private:
    QByteArray savedQsocHome;
    QByteArray savedXdg;
    bool       hadQsocHome = false;
    bool       hadXdg      = false;
};

void TestQSocPaths::init()
{
    hadQsocHome = qEnvironmentVariableIsSet("QSOC_HOME");
    if (hadQsocHome) {
        savedQsocHome = qgetenv("QSOC_HOME");
    }
    hadXdg = qEnvironmentVariableIsSet("XDG_CONFIG_HOME");
    if (hadXdg) {
        savedXdg = qgetenv("XDG_CONFIG_HOME");
    }
    qunsetenv("QSOC_HOME");
    qunsetenv("XDG_CONFIG_HOME");
}

void TestQSocPaths::cleanup()
{
    qunsetenv("QSOC_HOME");
    qunsetenv("XDG_CONFIG_HOME");
    if (hadQsocHome) {
        qputenv("QSOC_HOME", savedQsocHome);
    }
    if (hadXdg) {
        qputenv("XDG_CONFIG_HOME", savedXdg);
    }
}

void TestQSocPaths::envRoot_unsetReturnsEmpty()
{
    QCOMPARE(QSocPaths::envRoot(), QString());
}

void TestQSocPaths::envRoot_setReturnsValue()
{
    qputenv("QSOC_HOME", "/tmp/qsoc-env-root");
    QCOMPARE(QSocPaths::envRoot(), QString("/tmp/qsoc-env-root"));
}

void TestQSocPaths::projectRoot_emptyInputReturnsEmpty()
{
    QCOMPARE(QSocPaths::projectRoot(QString()), QString());
}

void TestQSocPaths::projectRoot_appendsDotQsoc()
{
    const QString result = QSocPaths::projectRoot("/home/user/proj");
    QCOMPARE(result, QString("/home/user/proj/.qsoc"));
}

void TestQSocPaths::userRoot_defaultsToHomeDotConfig()
{
    const QString expected = QDir::homePath() + "/.config/qsoc";
    QCOMPARE(QSocPaths::userRoot(), expected);
}

void TestQSocPaths::userRoot_honorsXdgConfigHome()
{
    qputenv("XDG_CONFIG_HOME", "/tmp/xdg-test");
    QCOMPARE(QSocPaths::userRoot(), QString("/tmp/xdg-test/qsoc"));
}

void TestQSocPaths::systemRoot_platformSpecific()
{
    const QString root = QSocPaths::systemRoot();
    QVERIFY(!root.isEmpty());
#if defined(Q_OS_LINUX)
    QCOMPARE(root, QString("/etc/qsoc"));
#elif defined(Q_OS_MACOS)
    QCOMPARE(root, QString("/Library/Application Support/qsoc"));
#elif defined(Q_OS_WIN)
    QVERIFY(root.endsWith("/qsoc"));
    QVERIFY(root.contains("ProgramData", Qt::CaseInsensitive));
#endif
}

void TestQSocPaths::resourceDirs_orderAndContents()
{
    qputenv("QSOC_HOME", "/tmp/qsoc-env");
    const QStringList dirs = QSocPaths::resourceDirs("skills", "/tmp/myproj");

    /* Expect four layers, env first, system last. */
    QCOMPARE(dirs.size(), 4);
    QCOMPARE(dirs.at(0), QString("/tmp/qsoc-env/skills"));
    QCOMPARE(dirs.at(1), QString("/tmp/myproj/.qsoc/skills"));
    QCOMPARE(dirs.at(2), QSocPaths::userRoot() + "/skills");
    QCOMPARE(dirs.at(3), QSocPaths::systemRoot() + "/skills");
}

void TestQSocPaths::resourceDirs_emptySubdirReturnsRoots()
{
    qputenv("QSOC_HOME", "/tmp/qsoc-env");
    const QStringList dirs = QSocPaths::resourceDirs(QString(), "/tmp/myproj");

    QCOMPARE(dirs.size(), 4);
    QCOMPARE(dirs.at(0), QString("/tmp/qsoc-env"));
    QCOMPARE(dirs.at(1), QString("/tmp/myproj/.qsoc"));
    QCOMPARE(dirs.at(2), QSocPaths::userRoot());
    QCOMPARE(dirs.at(3), QSocPaths::systemRoot());
}

void TestQSocPaths::resourceDirs_dedupesIdenticalRoots()
{
    /* Point QSOC_HOME at the same directory as the user root so the
     * canonical-path dedup must collapse them to a single entry. */
    qputenv("QSOC_HOME", QSocPaths::userRoot().toUtf8());
    const QStringList dirs = QSocPaths::resourceDirs("skills", QString());

    /* Env + user would be identical; project is empty (skipped); system
     * stays distinct. Expect 2 entries after dedup. */
    QCOMPARE(dirs.size(), 2);
    QCOMPARE(dirs.at(0), QSocPaths::userRoot() + "/skills");
    QCOMPARE(dirs.at(1), QSocPaths::systemRoot() + "/skills");
}

void TestQSocPaths::resourceDirs_skipsEmptyLayers()
{
    /* No env, no project → only user + system remain. */
    const QStringList dirs = QSocPaths::resourceDirs("memory", QString());
    QCOMPARE(dirs.size(), 2);
    QCOMPARE(dirs.at(0), QSocPaths::userRoot() + "/memory");
    QCOMPARE(dirs.at(1), QSocPaths::systemRoot() + "/memory");
}

QSOC_TEST_MAIN(TestQSocPaths)
#include "test_qsoccommonqsocpaths.moc"
