// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "common/qlspconfigloader.h"

#include "common/qlspprocessbackend.h"
#include "common/qlspservice.h"
#include "common/qsocconfig.h"

#include <QDebug>
#include <QString>
#include <QStringList>

#include <yaml-cpp/yaml.h>

void QLspConfigLoader::loadAndRegister(QLspService *service, QSocConfig *config)
{
    if (!service || !config)
        return;

    YAML::Node serversNode = config->getYamlNode("lsp.servers");
    if (!serversNode.IsDefined() || !serversNode.IsMap())
        return;

    for (const auto &item : serversNode) {
        QString name;
        try {
            name = QString::fromStdString(item.first.as<std::string>());

            const YAML::Node node = item.second;
            if (!node.IsMap())
                continue;

            if (!node["command"]) {
                qWarning() << "LSP server" << name << "missing required 'command'";
                continue;
            }
            if (!node["extensions"] || !node["extensions"].IsSequence()) {
                qWarning() << "LSP server" << name << "missing required 'extensions' list";
                continue;
            }

            QString     command = QString::fromStdString(node["command"].as<std::string>());
            QStringList args;
            if (node["args"] && node["args"].IsSequence()) {
                for (const auto &arg : node["args"]) {
                    args.append(QString::fromStdString(arg.as<std::string>()));
                }
            }
            QStringList exts;
            for (const auto &ext : node["extensions"]) {
                exts.append(QString::fromStdString(ext.as<std::string>()));
            }
            if (exts.isEmpty()) {
                qWarning() << "LSP server" << name << "has empty extensions list";
                continue;
            }

            auto *backend = new QLspProcessBackend(command, args, exts, service);
            service->addBackend(backend, /* overrideExisting */ true);

            qInfo() << "Registered external LSP server:" << name << "command:" << command
                    << "extensions:" << exts;
        } catch (const YAML::Exception &err) {
            qWarning() << "Failed to parse LSP server config" << name << ":" << err.what();
        }
    }
}
