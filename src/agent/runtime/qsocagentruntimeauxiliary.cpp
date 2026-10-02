// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>
#include "agent/qsocagent.h"
#include "agent/runtime/qsocagentruntime_p.h"
#include "agent/services/qsocpredictioncontroller.h"
#include "agent/services/qsocstatusline.h"
#include "common/qsocconfig.h"
#include <QTimer>

void QSocAgentRuntime::wireAuxiliaryServices()
{
    connect(
        d->agent,
        &QSocAgent::imageAttachment,
        this,
        [this](const QString &source, const QString &mime, const QString &body, int width, int height) {
            if (body.size() > 8 * 1024 * 1024)
                return;
            QSocAgentRuntimeEvent event;
            event.kind = QSocAgentRuntimeEvent::Kind::ImagePreview;
            event.text = source;
            event.json
                = {{"mime", mime.toStdString()},
                   {"data", body.toStdString()},
                   {"width", width},
                   {"height", height}};
            emit eventRaised(event);
        });
    auto      *prediction = new QSocPredictionController(this, d->llmService);
    const auto setting    = d->socConfig->getValue("agent.predict_input", "true").toLower();
    prediction->setEnabled(
        setting != "false" && setting != "0" && setting != "off" && setting != "no");
    connect(prediction, &QSocPredictionController::ghostReady, this, [this](const QString &text) {
        QSocAgentRuntimeEvent event;
        event.kind = QSocAgentRuntimeEvent::Kind::InputPrediction;
        event.text = text;
        emit eventRaised(event);
    });
    connect(d->agent, &QSocAgent::runError, prediction, [prediction] { prediction->markError(); });
    connect(d->agent, &QSocAgent::runComplete, prediction, [this, prediction] {
        QTimer::singleShot(0, prediction, [this, prediction] {
            if (!isRunning())
                prediction->requestPrediction(d->agent->getMessages());
        });
    });
    connect(this, &QSocAgentRuntime::sessionChanged, prediction, [prediction] {
        prediction->cancel();
    });
    auto            *statusLine = new QSocStatusLine(this);
    const YAML::Node node       = d->socConfig->getUserYamlNode("agent.status_line");
    try {
        if (node.IsScalar())
            statusLine->setCommand(QString::fromStdString(node.as<std::string>()));
        else if (node.IsMap() && node["command"] && node["command"].IsScalar()) {
            statusLine->setCommand(QString::fromStdString(node["command"].as<std::string>()));
            if (node["timeout_ms"] && node["timeout_ms"].IsScalar())
                statusLine->setTimeoutMs(node["timeout_ms"].as<int>());
        }
    } catch (const YAML::Exception &) {
        statusLine->setCommand({});
    }
    connect(statusLine, &QSocStatusLine::textReady, this, [this](const QString &text) {
        QSocAgentRuntimeEvent event;
        event.kind = QSocAgentRuntimeEvent::Kind::UserStatusLine;
        event.text = text;
        emit eventRaised(event);
    });
    auto refresh = [this, statusLine] {
        statusLine->setWorkingDirectory(workingDirectory());
        statusLine->requestRefresh(statusLinePayload());
    };
    connect(this, &QSocAgentRuntime::statusChanged, statusLine, refresh);
    connect(this, &QSocAgentRuntime::sessionChanged, statusLine, refresh);
    connect(d->agent, &QSocAgent::tokenUsage, statusLine, refresh);
}
