// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 Huang Rui <vowstar@gmail.com>

#include "common/qsocconfig.h"
#include "common/qsocconsole.h"
#include "common/qsocpaths.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QNetworkProxyFactory>
#include <QProcessEnvironment>

#include <fstream>
#include <yaml-cpp/yaml.h>

/* Define static constants */
const QString QSocConfig::CONFIG_FILE_NAME    = "qsoc.yml";
const QString QSocConfig::CONFIG_FILE_PROJECT = ".qsoc.yml";

QSocConfig::QSocConfig(QObject *parent, QSocProjectManager *projectManager)
    : QObject(parent)
    , projectManager(projectManager)
{
    loadConfig();
}

QSocConfig::~QSocConfig() = default;

void QSocConfig::setProjectManager(QSocProjectManager *projectManager)
{
    /* Only reload if project manager changes */
    if (this->projectManager != projectManager) {
        this->projectManager = projectManager;

        /* If valid project manager is provided, reload the configuration */
        if (projectManager) {
            loadConfig();
        }
    }
}

QSocProjectManager *QSocConfig::getProjectManager()
{
    return projectManager;
}

void QSocConfig::loadConfig()
{
    /* Clear existing configuration */
    configValues.clear();

    /* Seed user config template if missing, so first-run users have a
     * commented starting point at ~/.config/qsoc/qsoc.yml. */
    const QString userConfigPath = QDir(QSocPaths::userRoot()).filePath(CONFIG_FILE_NAME);
    if (!QFile::exists(userConfigPath)) {
        createTemplateConfig(userConfigPath);
    }

    /* Walk candidate roots in reverse (low to high priority) so later
     * layers override earlier keys via override=true. Project override is
     * handled separately below because its file lives at .qsoc.yml, not
     * .qsoc/qsoc.yml. */
    const QStringList roots = QSocPaths::resourceDirs(QString());
    for (qsizetype i = roots.size() - 1; i >= 0; --i) {
        loadFromYamlFile(QDir(roots.at(i)).filePath(CONFIG_FILE_NAME), true);
    }

    /* Project-level config - overrides all resource-dir layers */
    loadFromProjectYaml(true);

    /* Environment variables - highest priority */
    loadFromEnvironment();
}

void QSocConfig::loadFromEnvironment()
{
    /* Load from environment variables (highest priority) */
    const QProcessEnvironment env = QProcessEnvironment::systemEnvironment();

    /* List of supported environment variables with direct key mapping */
    const QStringList envVars
        = {"QSOC_AI_PROVIDER", "QSOC_API_KEY", "QSOC_AI_MODEL", "QSOC_API_URL"};

    /* Load each environment variable if it exists */
    for (const QString &var : envVars) {
        if (env.contains(var)) {
            /* Convert to lowercase key for consistency */
            const QString key = var.mid(5).toLower(); /* Remove "QSOC_" prefix */
            setValue(key, env.value(var));
        }
    }

    /* Compound key environment variables (highest priority) */
    const QMap<QString, QString> compoundEnvVars
        = {/* LLM endpoint config */
           {"QSOC_LLM_URL", "llm.url"},
           {"QSOC_LLM_KEY", "llm.key"},
           {"QSOC_LLM_MODEL", "llm.model"},
           {"QSOC_LLM_TIMEOUT", "llm.timeout"},
           {"QSOC_LLM_MAX_OUTPUT_TOKENS", "llm.max_output_tokens"},
           /* Agent config */
           {"QSOC_AGENT_TEMPERATURE", "agent.temperature"},
           {"QSOC_AGENT_MAX_TOKENS", "agent.max_tokens"},
           {"QSOC_AGENT_MAX_ITERATIONS", "agent.max_iterations"},
           {"QSOC_AGENT_SYSTEM_PROMPT", "agent.system_prompt"},
           {"QSOC_AGENT_AUTO_LOAD_MEMORY", "agent.auto_load_memory"},
           {"QSOC_AGENT_MEMORY_MAX_CHARS", "agent.memory_max_chars"}};

    for (auto iter = compoundEnvVars.constBegin(); iter != compoundEnvVars.constEnd(); ++iter) {
        if (env.contains(iter.key())) {
            setValue(iter.value(), env.value(iter.key()));
        }
    }

    /* Web-specific environment variables */
    const QMap<QString, QString> webEnvVars
        = {{"QSOC_WEB_SEARCH_API_URL", "web.search_api_url"},
           {"QSOC_WEB_SEARCH_API_KEY", "web.search_api_key"}};

    for (auto iter = webEnvVars.constBegin(); iter != webEnvVars.constEnd(); ++iter) {
        if (env.contains(iter.key())) {
            setValue(iter.value(), env.value(iter.key()));
        }
    }
}

void QSocConfig::loadFromYamlFile(const QString &filePath, bool override)
{
    /* Check if file exists */
    if (!QFile::exists(filePath)) {
        return;
    }

    YAML::Node config;
    try {
        config = YAML::LoadFile(filePath.toStdString());

        /* If the YAML is valid, process all key-value pairs */
        if (config.IsMap()) {
            for (const auto &item : config) {
                const QString key = QString::fromStdString(item.first.as<std::string>());

                /* Process scalar (string) values */
                if (item.second.IsScalar()) {
                    const QString value = QString::fromStdString(item.second.as<std::string>());

                    /* Set value if not already set or if override is true */
                    if (override || !hasKey(key)) {
                        setValue(key, value);
                    }
                }
                /* Process nested maps for provider-specific configurations */
                else if (item.second.IsMap()) {
                    /* Process each key-value pair in the nested map */
                    for (const auto &subItem : item.second) {
                        try {
                            const QString subKey = QString::fromStdString(
                                subItem.first.as<std::string>());
                            if (subItem.second.IsScalar()) {
                                /* Create composite key in format "provider.key" */
                                const QString compositeKey = key + "." + subKey;
                                const QString value        = QString::fromStdString(
                                    subItem.second.as<std::string>());

                                /* Set value if not already set or if override is true */
                                if (override || !hasKey(compositeKey)) {
                                    setValue(compositeKey, value);
                                }
                            }
                        } catch (const YAML::Exception &e) {
                            QSocConsole::warn()
                                << "Failed to parse nested config item:" << e.what();
                        }
                    }
                }
            }
        }
    } catch (const YAML::Exception &e) {
        QSocConsole::warn() << "Failed to load config from" << filePath << ":" << e.what();
    }
}

void QSocConfig::loadFromProjectYaml(bool override)
{
    /* Skip if project manager is not available */
    if (!projectManager) {
        return;
    }

    /* Get project path */
    const QString projectPath = projectManager->getProjectPath();
    if (projectPath.isEmpty()) {
        return;
    }

    /* Load from project-level config */
    const QString projectConfigPath = QDir(projectPath).filePath(CONFIG_FILE_PROJECT);
    loadFromYamlFile(projectConfigPath, override);
}

QString QSocConfig::getValue(const QString &key, const QString &defaultValue) const
{
    /* Direct access for existing format */
    if (configValues.contains(key)) {
        return configValues.value(key);
    }

    return defaultValue;
}

void QSocConfig::setValue(const QString &key, const QString &value)
{
    configValues[key] = value;
}

bool QSocConfig::hasKey(const QString &key) const
{
    return configValues.contains(key);
}

QMap<QString, QString> QSocConfig::getAllValues() const
{
    return configValues;
}

QList<McpServerConfig> QSocConfig::mcpServers() const
{
    return McpServerConfig::parseList(getYamlNode("mcp.servers"));
}

YAML::Node QSocConfig::getYamlNode(const QString &dotPath) const
{
    QStringList parts = dotPath.split('.');

    /* Helper: traverse a YAML node by path segments */
    auto resolve = [&](const QString &filePath) -> YAML::Node {
        if (!QFile::exists(filePath)) {
            return {};
        }
        try {
            YAML::Node root = YAML::LoadFile(filePath.toStdString());
            YAML::Node node = root;
            for (const QString &part : parts) {
                if (!node.IsMap() || !node[part.toStdString()]) {
                    return {};
                }
                node = node[part.toStdString()];
            }
            return node;
        } catch (const YAML::Exception &) {
            return {};
        }
    };

    /* Project config has higher priority */
    if (projectManager) {
        QString projectPath = projectManager->getProjectPath();
        if (!projectPath.isEmpty()) {
            YAML::Node node = resolve(QDir(projectPath).filePath(CONFIG_FILE_PROJECT));
            if (node.IsDefined() && !node.IsNull()) {
                return node;
            }
        }
    }

    /* Walk the remaining layers (env → user → system) in priority order. */
    const QStringList roots = QSocPaths::resourceDirs(QString());
    for (const QString &root : roots) {
        YAML::Node node = resolve(QDir(root).filePath(CONFIG_FILE_NAME));
        if (node.IsDefined() && !node.IsNull()) {
            return node;
        }
    }

    return {};
}

bool QSocConfig::createTemplateConfig(const QString &filePath)
{
    /* Create directory if it doesn't exist */
    const QFileInfo fileInfo(filePath);
    const QDir      directory = fileInfo.dir();

    if (!directory.exists()) {
        if (!directory.mkpath(".")) {
            QSocConsole::warn() << "Failed to create directory for config file:"
                                << directory.path();
            return false;
        }
    }

    /* Create template config file with comments */
    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QSocConsole::warn() << "Failed to create template config file:" << filePath;
        return false;
    }

    QTextStream out(&file);

    out << "# QSoc Configuration File\n";
    out << "# Uncomment and modify the settings below as needed.\n\n";

    out << "# =============================================================================\n";
    out << "# LLM Configuration\n";
    out << "# =============================================================================\n";
    out << "# All LLM providers use OpenAI Chat Completions format.\n";
    out << "# Configure URL, key (if needed), and model name.\n\n";

    out << "# llm:\n";
    out << "#   url: https://api.deepseek.com/v1/chat/completions\n";
    out << "#   key: sk-xxx\n";
    out << "#   model: deepseek-chat\n";
    out << "#   timeout: 30000\n";
    out << "#   max_output_tokens: 8192   # Max output tokens (0 or omit = API default)\n\n";

    out << "# Common endpoints:\n";
    out << "# - DeepSeek:  https://api.deepseek.com/v1/chat/completions\n";
    out << "# - OpenAI:    https://api.openai.com/v1/chat/completions\n";
    out << "# - Groq:      https://api.groq.com/openai/v1/chat/completions\n";
    out << "# - Ollama:    http://localhost:11434/v1/chat/completions\n\n";

    out << "# =============================================================================\n";
    out << "# Network Proxy Configuration\n";
    out << "# =============================================================================\n\n";

    out << "# proxy:\n";
    out << "#   type: system       # system | none | http | socks5\n";
    out << "#   host: 127.0.0.1\n";
    out << "#   port: 7890\n";
    out << "#   user: optional\n";
    out << "#   password: optional\n\n";

    out << "# =============================================================================\n";
    out << "# LSP Configuration\n";
    out << "# =============================================================================\n";
    out << "# External Language Server Protocol servers. They override the built-in\n";
    out << "# slang backend for matching file extensions. Each server runs as a child\n";
    out << "# process communicating over JSON-RPC stdio.\n\n";

    out << "# lsp:\n";
    out << "#   servers:\n";
    out << "#     verible:\n";
    out << "#       command: verible-verilog-ls\n";
    out << "#       extensions: [.v, .sv, .svh]\n";
    out << "#     vhdl-ls:\n";
    out << "#       command: vhdl_ls\n";
    out << "#       extensions: [.vhd, .vhdl]\n\n";

    out << "# =============================================================================\n";
    out << "# Agent Configuration\n";
    out << "# =============================================================================\n\n";

    out << "# agent:\n";
    out << "#   temperature: 0.2          # LLM temperature (0.0-1.0)\n";
    out << "#   max_tokens: 128000        # Maximum context tokens\n";
    out << "#   max_iterations: 100       # Safety limit for iterations\n";
    out << "#   auto_load_memory: true    # Auto-inject memory into system prompt\n";
    out << "#   memory_max_chars: 24000   # Max chars for memory in system prompt\n";
    out << "#   system_prompt: |          # Custom system prompt\n";
    out << "#     You are a helpful assistant.\n\n";

    out << "# =============================================================================\n";
    out << "# Web Search & Fetch Configuration\n";
    out << "# =============================================================================\n\n";

    out << "# web:\n";
    out << "#   search_api_url: http://localhost:8080  # SearXNG API URL\n";
    out << "#   search_api_key:                        # SearXNG API key (optional)\n";

    file.close();

    QSocConsole::debug() << "Created template config file:" << filePath;
    return true;
}
