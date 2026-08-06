// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "gui/schematicwindow/schematicitemfactory.h"
#include "gui/schematicwindow/schematicmodule.h"
#include "gui/schematicwindow/schematicscenerestorecommand.h"
#include "gui/undo/snapshotcommand.h"

#include <qschematic/items/itemfactory.hpp>
#include <qschematic/items/node.hpp>
#include <qschematic/scene.hpp>

#include <yaml-cpp/yaml.h>

#include <QApplication>
#include <QUndoStack>
#include <QtTest>

/**
 * @brief Scene snapshot tests.
 * @details Bulk edits are undone by swapping the whole scene content. That
 *          only works if a capture and restore round trip reproduces the
 *          scene, and if restoring never touches the stack the command
 *          lives on.
 */
class Test : public QObject
{
    Q_OBJECT

private slots:
    void roundTripPreservesItems()
    {
        QSchematic::Scene scene;
        for (int index = 0; index < 3; ++index) {
            auto node = std::make_shared<QSchematic::Items::Node>();
            node->setPos(index * 40, index * 20);
            scene.addItem(node);
        }
        QCOMPARE(scene.items().count(), 3);

        const gpds::container document = scene.to_container();

        scene.restore_from_container({});
        QCOMPARE(scene.items().count(), 0);

        scene.restore_from_container(document);
        QCOMPARE(scene.items().count(), 3);
    }

    void roundTripPreservesSchematicModules()
    {
        /* Reconstructing a module needs the custom factory the editor
           installs; without it a restored document comes back empty. */
        auto factoryFunc = std::bind(&SchematicItemFactory::from_container, std::placeholders::_1);
        QSchematic::Items::Factory::instance().setCustomItemsFactory(factoryFunc);

        const YAML::Node moduleYaml = YAML::Load(R"(
port:
  clk:
    type: logic
    direction: in
  dout:
    type: logic[7:0]
    direction: out
)");

        QSchematic::Scene scene;
        auto              module = std::make_shared<SchematicModule>("alu", moduleYaml);
        module->setInstanceName("u_alu_0");
        scene.addItem(module);
        QCOMPARE(scene.items().count(), 1);

        const gpds::container document = scene.to_container();

        scene.restore_from_container(document);
        QCOMPARE(scene.items().count(), 1);

        auto restored = std::dynamic_pointer_cast<SchematicModule>(scene.items().first());
        QVERIFY(restored != nullptr);
        QCOMPARE(restored->instanceName(), QString("u_alu_0"));
        QCOMPARE(restored->moduleName(), QString("alu"));
    }

    void roundTripPreservesSeveralModules()
    {
        auto factoryFunc = std::bind(&SchematicItemFactory::from_container, std::placeholders::_1);
        QSchematic::Items::Factory::instance().setCustomItemsFactory(factoryFunc);

        const YAML::Node moduleYaml = YAML::Load(R"(
port:
  clk:
    type: logic
    direction: in
  dout:
    type: logic[7:0]
    direction: out
)");

        QSchematic::Scene scene;
        for (int index = 0; index < 2; ++index) {
            auto module = std::make_shared<SchematicModule>("alu", moduleYaml);
            module->setInstanceName(QString("u_alu_%1").arg(index));
            module->setPos(index * 300, 0);
            scene.addItem(module);
        }
        QCOMPARE(scene.items().count(), 2);

        const gpds::container document = scene.to_container();

        scene.restore_from_container(document);
        QCOMPARE(scene.items().count(), 2);
    }

    void restoreFromContainerLeavesTheStackAlone()
    {
        QSchematic::Scene scene;
        auto              node = std::make_shared<QSchematic::Items::Node>();
        scene.addItem(node);

        /* A marker command stands in for a restore command mid-undo. */
        scene.undoStack()->push(new QUndoCommand("marker"));
        QCOMPARE(scene.undoStack()->count(), 1);

        scene.restore_from_container({});
        QCOMPARE(scene.items().count(), 0);
        QCOMPARE(scene.undoStack()->count(), 1);
    }

    void sceneRestoreCommandSwapsContentAndEditorState()
    {
        QSchematic::Scene scene;
        scene.addItem(std::make_shared<QSchematic::Items::Node>());
        const gpds::container before = scene.to_container();

        /* The bulk operation runs before the command is pushed. */
        scene.addItem(std::make_shared<QSchematic::Items::Node>());
        QCOMPARE(scene.items().count(), 2);

        int         begun = 0;
        QStringList applied;
        scene.undoStack()->push(new SchematicSceneRestoreCommand(
            &scene,
            before,
            {"before.soc_net"},
            {"after.soc_net"},
            [&]() { ++begun; },
            [&](const QStringList &files) { applied = files; },
            "bulk edit"));

        /* push() calls redo(), but the operation already ran. */
        QCOMPARE(begun, 0);
        QCOMPARE(scene.items().count(), 2);

        scene.undoStack()->undo();
        QCOMPARE(begun, 1);
        QCOMPARE(scene.items().count(), 1);
        QCOMPARE(applied, QStringList{"before.soc_net"});

        scene.undoStack()->redo();
        QCOMPARE(begun, 2);
        QCOMPARE(scene.items().count(), 2);
        QCOMPARE(applied, QStringList{"after.soc_net"});
    }

    void snapshotCommandSkipsTheFirstRedo()
    {
        int applied   = 0;
        int lastValue = 0;

        QUndoStack stack;
        stack.push(new SnapshotCommand<int>(
            1,
            2,
            [&](const int &value) {
                ++applied;
                lastValue = value;
            },
            "bulk edit"));

        /* push() calls redo(), but the operation already ran. */
        QCOMPARE(applied, 0);

        stack.undo();
        QCOMPARE(applied, 1);
        QCOMPARE(lastValue, 1);

        stack.redo();
        QCOMPARE(applied, 2);
        QCOMPARE(lastValue, 2);
    }

    void snapshotScopePushesOnlyWhenNotCancelled()
    {
        QUndoStack stack;
        int        document = 0;

        {
            SnapshotScope<int> scope(
                &stack,
                [&]() { return document; },
                [&](const int &value) { document = value; },
                "kept");
            document = 7;
        }
        QCOMPARE(stack.count(), 1);

        {
            SnapshotScope<int> scope(
                &stack,
                [&]() { return document; },
                [&](const int &value) { document = value; },
                "dropped");
            document = 9;
            scope.cancel();
        }
        QCOMPARE(stack.count(), 1);

        stack.undo();
        QCOMPARE(document, 0);
    }
};

QTEST_MAIN(Test)
#include "test_qsocguischematicsnapshot.moc"
