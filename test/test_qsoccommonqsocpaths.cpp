// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "common/qsocpaths.h"
#include "qsoc_test.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QScopeGuard>
#include <QTemporaryDir>
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

    void artifactPathAcceptsLinuxNames_data();
    void artifactPathAcceptsLinuxNames();
    void artifactPathAcceptsContainedPaths();
    void artifactPathAcceptsRelativeOutputRoot();
    void artifactPathRejectsEscapes();
    void artifactPathResolvesRawParent();
    void artifactPathRejectsUnsafeTargets();

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

void TestQSocPaths::artifactPathAcceptsLinuxNames_data()
{
    QTest::addColumn<QString>("requestedPath");
    QTest::newRow("con") << QStringLiteral("CON");
    QTest::newRow("aux") << QStringLiteral("AUX.v");
    QTest::newRow("colon") << QStringLiteral("name:part.v");
    QTest::newRow("trailing-dot") << QStringLiteral("tail.");
    QTest::newRow("question") << QStringLiteral("what?.v");
    QTest::newRow("backslash") << QStringLiteral("back\\slash.v");
    QTest::newRow("leading-space") << QStringLiteral(" leading.v");
    QTest::newRow("trailing-space") << QStringLiteral("trailing.v ");
}

void TestQSocPaths::artifactPathAcceptsLinuxNames()
{
#ifndef Q_OS_LINUX
    QSKIP("This test covers Linux file-name semantics.");
#else
    QFETCH(QString, requestedPath);
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    const auto artifact = QSocPaths::resolveArtifactPath(directory.path(), requestedPath);
    QVERIFY2(artifact.isValid(), qPrintable(artifact.error));

    QFile file(artifact.path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write("stable\n"), 7);
    file.close();

    const auto existing = QSocPaths::resolveArtifactPath(directory.path(), requestedPath);
    QVERIFY2(existing.isValid(), qPrintable(existing.error));
    QCOMPARE(existing.path, artifact.path);
#endif
}

void TestQSocPaths::artifactPathAcceptsContainedPaths()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString nestedPath = QDir(directory.path()).filePath("nested");
    QVERIFY(QDir().mkpath(nestedPath));

    const auto nested = QSocPaths::resolveArtifactPath(directory.path(), "nested/file.v");
    QVERIFY2(nested.isValid(), qPrintable(nested.error));
    QCOMPARE(nested.path, QDir(nestedPath).filePath("file.v"));

    const auto parent = QSocPaths::resolveArtifactPath(directory.path(), "nested/../top.v");
    QVERIFY2(parent.isValid(), qPrintable(parent.error));
    QCOMPARE(parent.path, QDir(directory.path()).canonicalPath() + "/top.v");

    const QString absolutePath = QDir(nestedPath).filePath("absolute.v");
    const auto    absolute     = QSocPaths::resolveArtifactPath(directory.path(), absolutePath);
    QVERIFY2(absolute.isValid(), qPrintable(absolute.error));
    QCOMPARE(absolute.path, absolutePath);
}

void TestQSocPaths::artifactPathAcceptsRelativeOutputRoot()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString originalPath = QDir::currentPath();
    QVERIFY(QDir::setCurrent(directory.path()));
    const auto restorePath = qScopeGuard([&]() { QDir::setCurrent(originalPath); });

    QVERIFY(QDir().mkpath("output/nested"));
    const auto artifact = QSocPaths::resolveArtifactPath("output", "nested/../top.v");
    QVERIFY2(artifact.isValid(), qPrintable(artifact.error));
    QCOMPARE(artifact.path, QDir(directory.path()).canonicalPath() + "/output/top.v");
}

void TestQSocPaths::artifactPathRejectsEscapes()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString outputPath  = QDir(directory.path()).filePath("output");
    const QString outsidePath = QDir(directory.path()).filePath("outside");
    const QString prefixPath  = QDir(directory.path()).filePath("output-other");
    QVERIFY(QDir().mkpath(outputPath));
    QVERIFY(QDir().mkpath(outsidePath));
    QVERIFY(QDir().mkpath(prefixPath));

    QVERIFY(!QSocPaths::resolveArtifactPath(outputPath, "../outside/file.v").isValid());
    QVERIFY(
        !QSocPaths::resolveArtifactPath(outputPath, QDir(outsidePath).filePath("file.v")).isValid());
    QVERIFY(!QSocPaths::resolveArtifactPath(outputPath, "../output-other/file.v").isValid());
    QVERIFY(!QSocPaths::resolveArtifactPath(outputPath, QString()).isValid());
    QVERIFY(!QSocPaths::resolveArtifactPath(outputPath, QString::fromLatin1("embedded\0null.v", 15))
                 .isValid());
}

void TestQSocPaths::artifactPathResolvesRawParent()
{
#ifndef Q_OS_UNIX
    QSKIP("This platform does not provide Unix symbolic-link semantics.");
#else
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString outputPath       = QDir(directory.path()).filePath("output");
    const QString insidePath       = QDir(outputPath).filePath("inside");
    const QString outsidePath      = QDir(directory.path()).filePath("outside");
    const QString outsideChildPath = QDir(outsidePath).filePath("child");
    QVERIFY(QDir().mkpath(insidePath));
    QVERIFY(QDir().mkpath(outsideChildPath));

    const QString outsideLink = QDir(outputPath).filePath("outside-link");
    QVERIFY(QFile::link(outsideChildPath, outsideLink));
    QVERIFY(!QSocPaths::resolveArtifactPath(outputPath, "outside-link/../escaped.v").isValid());
    QVERIFY(!QSocPaths::resolveArtifactPath(outputPath, "outside-link/file.v").isValid());

    const QString insideLink = QDir(outputPath).filePath("inside-link");
    QVERIFY(QFile::link(insidePath, insideLink));
    const auto inside = QSocPaths::resolveArtifactPath(outputPath, "inside-link/file.v");
    QVERIFY2(inside.isValid(), qPrintable(inside.error));
    QCOMPARE(inside.path, QDir(insidePath).filePath("file.v"));
#endif
}

void TestQSocPaths::artifactPathRejectsUnsafeTargets()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    QVERIFY(QDir(directory.path()).mkdir("directory.v"));
    QVERIFY(!QSocPaths::resolveArtifactPath(directory.path(), "directory.v").isValid());
    QVERIFY(!QSocPaths::resolveArtifactPath(directory.path(), "missing/file.v").isValid());

    const QString fileRoot = QDir(directory.path()).filePath("file-root");
    QFile         rootFile(fileRoot);
    QVERIFY(rootFile.open(QIODevice::WriteOnly));
    rootFile.close();
    QVERIFY(!QSocPaths::resolveArtifactPath(fileRoot, "file.v").isValid());
}

QSOC_TEST_MAIN(TestQSocPaths)
#include "test_qsoccommonqsocpaths.moc"
