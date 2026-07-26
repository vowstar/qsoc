// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "common/qsocyamlutils.h"
#include "qsoc_test.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTextStream>
#include <QtCore>
#include <QtTest>

/**
 * @brief Document version guard tests.
 * @details The GUI editors read the serialization version before they replace
 *          the document they already hold. A file written by a different
 *          release used to load as an empty canvas and be overwritten on the
 *          next save.
 */
class TestQSocCommonQSocYamlUtils : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir tempDir;

    QString writeFile(const QString &name, const QString &content)
    {
        const QString path = QDir(tempDir.path()).filePath(name);
        QFile         file(path);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
            return {};
        }
        QTextStream stream(&file);
        stream << content;
        file.close();
        return path;
    }

private slots:
    void initTestCase() { QVERIFY(tempDir.isValid()); }

    void readsTheDeclaredVersion()
    {
        const QString path = writeFile("v2.soc_prc", R"(qschematic:
  "-version": 2
  scene:
    rect:
      height: 3000
)");
        QVERIFY(!path.isEmpty());

        int     version = 0;
        QString error;
        QVERIFY(QSocYamlUtils::readDocumentVersion(path, "qschematic", version, error));
        QCOMPARE(version, 2);
        QVERIFY(error.isEmpty());
    }

    void reportsAForeignVersion()
    {
        const QString path = writeFile("v3.soc_prc", R"(qschematic:
  "-version": 3
  scene:
    rect:
      height: 3000
)");
        QVERIFY(!path.isEmpty());

        int     version = 0;
        QString error;
        QVERIFY(QSocYamlUtils::readDocumentVersion(path, "qschematic", version, error));
        QCOMPARE(version, 3);
    }

    void failsWithoutTheRootSection()
    {
        const QString path = writeFile("other.soc_prc", "something_else:\n  value: 1\n");
        QVERIFY(!path.isEmpty());

        int     version = 0;
        QString error;
        QVERIFY(!QSocYamlUtils::readDocumentVersion(path, "qschematic", version, error));
        QVERIFY(!error.isEmpty());
    }

    void failsWithoutAVersion()
    {
        const QString path = writeFile("noversion.soc_prc", "qschematic:\n  scene:\n    rect:\n");
        QVERIFY(!path.isEmpty());

        int     version = 0;
        QString error;
        QVERIFY(!QSocYamlUtils::readDocumentVersion(path, "qschematic", version, error));
        QVERIFY(!error.isEmpty());
    }

    void failsOnMalformedYaml()
    {
        const QString path = writeFile("broken.soc_prc", "qschematic:\n  - [unclosed\n");
        QVERIFY(!path.isEmpty());

        int     version = 0;
        QString error;
        QVERIFY(!QSocYamlUtils::readDocumentVersion(path, "qschematic", version, error));
        QVERIFY(!error.isEmpty());
    }

    void failsOnAMissingFile()
    {
        int     version = 0;
        QString error;
        QVERIFY(!QSocYamlUtils::readDocumentVersion(
            QDir(tempDir.path()).filePath("absent.soc_prc"), "qschematic", version, error));
        QVERIFY(!error.isEmpty());
    }
};

QSOC_TEST_MAIN(TestQSocCommonQSocYamlUtils)
#include "test_qsoccommonqsocyamlutils.moc"
