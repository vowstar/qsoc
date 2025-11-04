// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2023-2025 Huang Rui <vowstar@gmail.com>

#include "gui/schematicwindow/schematicwindow.h"

#include <QtTest>
#include <qschematic/scene.hpp>

class TestSchematicWindow : public QObject
{
    Q_OBJECT

private slots:
    void generateUniqueInstanceName_emptyScene();
    void generateUniqueInstanceName_basicPattern();
    void getExistingInstanceNames_emptyScene();
};

void TestSchematicWindow::generateUniqueInstanceName_emptyScene()
{
    QSchematic::Scene scene;

    // Empty scene should generate u_counter_0
    QString name = SchematicWindow::generateUniqueInstanceName(scene, "counter");
    QCOMPARE(name, QString("u_counter_0"));
}

void TestSchematicWindow::generateUniqueInstanceName_basicPattern()
{
    QSchematic::Scene scene;

    // Verify naming pattern: u_<modulename>_<number>
    QString name1 = SchematicWindow::generateUniqueInstanceName(scene, "adder");
    QVERIFY(name1.startsWith("u_adder_"));
    QVERIFY(name1.endsWith("_0"));
}

void TestSchematicWindow::getExistingInstanceNames_emptyScene()
{
    QSchematic::Scene scene;

    QSet<QString> names = SchematicWindow::getExistingInstanceNames(scene);
    QVERIFY(names.isEmpty());
}

QTEST_MAIN(TestSchematicWindow)
#include "test_qsocguischematicwindow.moc"
