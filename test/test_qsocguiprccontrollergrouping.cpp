// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "gui/prcwindow/prccontrollergrouping.h"

#include <QApplication>
#include <QtTest>

using namespace PrcLibrary;

/**
 * @brief Controller grouping tests.
 * @details The netlist export writes one entry per controller. It used to take
 *          the first assignment it found and emit a single entry, so a diagram
 *          holding two clock controllers lost one of them silently.
 */
class Test : public QObject
{
    Q_OBJECT

private:
    static std::shared_ptr<PrcPrimitiveItem> clockInput(const QString &controller)
    {
        auto item         = std::make_shared<PrcPrimitiveItem>(ClockInput);
        auto params       = std::get<ClockInputParams>(item->params());
        params.controller = controller;
        item->setParams(params);
        return item;
    }

private slots:
    void unassignedPrimitivesFallBack()
    {
        const PrcItemList items{clockInput(QString())};
        QCOMPARE(
            PrcControllerGrouping::controllerNames(items, "clock_ctrl"), QStringList{"clock_ctrl"});
    }

    void distinctControllersAreAllReported()
    {
        const PrcItemList
            items{clockInput("clock_ctrl"), clockInput("clock_ctrl_gpu"), clockInput("clock_ctrl")};

        const QStringList names = PrcControllerGrouping::controllerNames(items, "clock_ctrl");
        QCOMPARE(names.size(), 2);
        QCOMPARE(names.at(0), QString("clock_ctrl"));
        QCOMPARE(names.at(1), QString("clock_ctrl_gpu"));
    }

    void itemsArePartitionedByController()
    {
        const PrcItemList
            items{clockInput("clock_ctrl"), clockInput("clock_ctrl_gpu"), clockInput("clock_ctrl")};

        QCOMPARE(
            PrcControllerGrouping::itemsOfController(items, "clock_ctrl", "clock_ctrl").size(), 2);
        QCOMPARE(
            PrcControllerGrouping::itemsOfController(items, "clock_ctrl_gpu", "clock_ctrl").size(),
            1);
        QCOMPARE(
            PrcControllerGrouping::itemsOfController(items, "clock_ctrl_none", "clock_ctrl").size(),
            0);
    }

    void unassignedItemsJoinTheFallbackGroup()
    {
        const PrcItemList items{clockInput(QString()), clockInput("clock_ctrl_gpu")};

        QCOMPARE(
            PrcControllerGrouping::itemsOfController(items, "clock_ctrl", "clock_ctrl").size(), 1);
    }

    void nullItemsDoNotCrash()
    {
        const PrcItemList items{nullptr, clockInput("clock_ctrl_gpu")};

        QCOMPARE(PrcControllerGrouping::controllerNames(items, "clock_ctrl").size(), 2);
    }
};

QTEST_MAIN(Test)
#include "test_qsocguiprccontrollergrouping.moc"
