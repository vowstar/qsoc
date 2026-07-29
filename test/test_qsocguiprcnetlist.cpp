// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "common/qsocgenerateprimitiveclock.h"
#include "gui/prcwindow/prcprimitiveitem.h"
#include "gui/prcwindow/prcwindow.h"

#include <qschematic/items/wire.hpp>

#include <yaml-cpp/yaml.h>

#include <QDir>
#include <QFile>
#include <QHashFunctions>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QTextStream>
#include <QtTest>

#include <algorithm>

using namespace PrcLibrary;

namespace {

class TestPrcNetlist : public QObject
{
    Q_OBJECT

private:
    static QStringList sourceNames()
    {
        return {
            "clk_zeta",
            "clk_alpha",
            "clk_kappa",
            "clk_beta",
            "clk_omega",
            "clk_delta",
            "clk_eta",
            "clk_gamma",
        };
    }

    static std::shared_ptr<PrcConnector> outputConnector(
        const std::shared_ptr<PrcPrimitiveItem> &item)
    {
        for (const auto &connector : item->connectors()) {
            auto prcConnector = std::dynamic_pointer_cast<PrcConnector>(connector);
            if (prcConnector && prcConnector->text() == "out") {
                return prcConnector;
            }
        }
        return nullptr;
    }

    static std::shared_ptr<PrcConnector> freeInputConnector(
        const std::shared_ptr<PrcPrimitiveItem> &item)
    {
        for (const auto &connector : item->connectors()) {
            auto prcConnector = std::dynamic_pointer_cast<PrcConnector>(connector);
            if (prcConnector
                && (prcConnector->text() == "in" || prcConnector->text().startsWith("in_"))
                && !prcConnector->isConnected()) {
                return prcConnector;
            }
        }
        return nullptr;
    }

    static bool populateClockScene(PrcWindow &window)
    {
        auto target                 = std::make_shared<PrcPrimitiveItem>(ClockTarget, "clk_out");
        auto targetParams           = std::get<ClockTargetParams>(target->params());
        targetParams.name           = "clk_out";
        targetParams.freq           = "100MHz";
        targetParams.controller     = "clock_ctrl";
        targetParams.select         = "clk_select";
        targetParams.mux.configured = true;
        target->setParams(targetParams);
        target->setPos(600.0, 300.0);
        if (!window.prcScene().addItem(target)) {
            return false;
        }

        const QStringList names = sourceNames();
        for (int index = 0; index < names.size(); ++index) {
            const QString &name         = names.at(index);
            auto           source       = std::make_shared<PrcPrimitiveItem>(ClockInput, name);
            auto           sourceParams = std::get<ClockInputParams>(source->params());
            sourceParams.name           = name;
            sourceParams.freq           = "100MHz";
            sourceParams.controller     = "clock_ctrl";
            source->setParams(sourceParams);
            source->setPos(0.0, index * 100.0);
            if (!window.prcScene().addItem(source)) {
                return false;
            }

            const auto output = outputConnector(source);
            const auto input  = freeInputConnector(target);
            if (!output || !input) {
                return false;
            }

            auto wire = std::make_shared<QSchematic::Items::Wire>();
            wire->append_point(output->scenePos());
            wire->append_point(input->scenePos());
            if (!window.prcScene().addWire(wire)) {
                return false;
            }
        }
        return true;
    }

    static QByteArray readFile(const QString &path)
    {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            return {};
        }
        return file.readAll();
    }

private slots:
    void exportChild()
    {
        if (qEnvironmentVariable("QSOC_PRC_EXPORT_CHILD") != "1") {
            return;
        }

        const QString outputPath = qEnvironmentVariable("QSOC_PRC_EXPORT_PATH");
        QVERIFY(!outputPath.isEmpty());
        const QString hashMode = qEnvironmentVariable("QSOC_PRC_EXPORT_HASH_MODE");
#if QT_VERSION >= QT_VERSION_CHECK(6, 2, 0)
        const qulonglong hashSeed = QHashSeed::globalSeed();
#else
        const qulonglong hashSeed = static_cast<qulonglong>(qGlobalQHashSeed());
#endif
        if (hashMode == "zero") {
            QCOMPARE(hashSeed, qulonglong(0));
        } else {
            QCOMPARE(hashMode, QString("random"));
            QVERIFY(hashSeed != 0);
        }

        PrcWindow window;
        QVERIFY(populateClockScene(window));
        QVERIFY(window.exportNetlist(outputPath));
    }

    void clockLinksAreStableAcrossProcesses()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());

        /* The wiring order in populateClockScene fills successive input
           connectors; that physical order, not the names, is the contract. */
        const QStringList expectedSources = sourceNames();

        QByteArray yamlBaseline;
        QByteArray verilogBaseline;
        for (int run = 0; run < 8; ++run) {
            const QString runPath = directory.filePath(QString("run_%1").arg(run));
            QVERIFY(QDir().mkpath(runPath));
            const QString outputPath = QDir(runPath).filePath("clock_export.soc_net");

            QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
            environment.insert("QSOC_PRC_EXPORT_CHILD", "1");
            environment.insert("QSOC_PRC_EXPORT_PATH", outputPath);
            environment.insert("QSOC_PRC_EXPORT_HASH_MODE", run == 0 ? "zero" : "random");
            environment.insert("QT_QPA_PLATFORM", "offscreen");
            environment.insert("TMPDIR", runPath);
            if (run == 0) {
                environment.insert("QT_HASH_SEED", "0");
            } else {
                environment.remove("QT_HASH_SEED");
            }

            QProcess process;
            process.setProcessEnvironment(environment);
            process.setProcessChannelMode(QProcess::MergedChannels);
            process.setWorkingDirectory(runPath);
            process.start(QCoreApplication::applicationFilePath(), {"exportChild"});
            QVERIFY(process.waitForStarted(5000));
            const bool stopped = process.waitForFinished(30000);
            if (!stopped) {
                process.kill();
                process.waitForFinished(1000);
            }
            const QByteArray childOutput = process.readAll();
            QVERIFY2(stopped, childOutput.constData());
            QVERIFY2(
                process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0,
                childOutput.constData());

            const QByteArray bytes = readFile(outputPath);
            QVERIFY(!bytes.isEmpty());
            if (yamlBaseline.isEmpty()) {
                yamlBaseline = bytes;
            } else {
                QCOMPARE(bytes, yamlBaseline);
            }

            const YAML::Node root  = YAML::Load(std::string(bytes.constData(), bytes.size()));
            const YAML::Node links = root["clock"][0]["target"]["clk_out"]["link"];
            QVERIFY(links && links.IsMap());

            QStringList actualSources;
            for (auto it = links.begin(); it != links.end(); ++it) {
                actualSources << QString::fromStdString(it->first.as<std::string>());
            }
            QCOMPARE(actualSources, expectedSources);

            QSocClockPrimitive primitive;
            QString            verilog;
            QTextStream        stream(&verilog);
            QVERIFY(primitive.generateClockController(root["clock"][0], stream));

            QStringList reversedSources = expectedSources;
            std::reverse(reversedSources.begin(), reversedSources.end());
            for (QString &source : reversedSources) {
                source = "clk_clk_out_from_" + source;
            }
            QString compactVerilog = verilog;
            compactVerilog.remove(QRegularExpression("\\s+"));
            const QString expectedConnection = ".clk_in({" + reversedSources.join(',') + "})";
            QVERIFY2(
                compactVerilog.contains(expectedConnection),
                qPrintable("Missing stable mux mapping: " + expectedConnection));

            const QByteArray verilogBytes = verilog.toUtf8();
            if (verilogBaseline.isEmpty()) {
                verilogBaseline = verilogBytes;
            } else {
                QCOMPARE(verilogBytes, verilogBaseline);
            }
        }
    }
};

} // namespace

QTEST_MAIN(TestPrcNetlist)

#include "test_qsocguiprcnetlist.moc"
