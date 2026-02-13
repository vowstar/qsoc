// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 Huang Rui <vowstar@gmail.com>

#include "common/qsocconfig.h"

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
const QString QSocConfig::CONFIG_FILE_SYSTEM  = "/etc/qsoc/qsoc.yml";
const QString QSocConfig::CONFIG_FILE_USER    = ".config/qsoc/qsoc.yml";
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

    /* Check if user config file exists and create template if needed */
    const QString userConfigPath = QDir::home().absoluteFilePath(CONFIG_FILE_USER);
    if (!QFile::exists(userConfigPath)) {
        createTemplateConfig(userConfigPath);
    }

    /* Load in order of priority (lowest to highest) */
#ifdef Q_OS_LINUX
    /* System-level config (Linux only) - lowest priority */
    loadFromYamlFile(CONFIG_FILE_SYSTEM);
#endif

    /* User-level config */
    loadFromYamlFile(userConfigPath);

    /* Project-level config (if project manager available) */
    loadFromProjectYaml();

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

    /* Agent-specific environment variables with compound keys */
    const QMap<QString, QString> agentEnvVars
        = {{"QSOC_AGENT_TEMPERATURE", "agent.temperature"},
           {"QSOC_AGENT_MAX_TOKENS", "agent.max_tokens"},
           {"QSOC_AGENT_MAX_ITERATIONS", "agent.max_iterations"},
           {"QSOC_AGENT_SYSTEM_PROMPT", "agent.system_prompt"}};

    for (auto iter = agentEnvVars.constBegin(); iter != agentEnvVars.constEnd(); ++iter) {
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
                            qWarning() << "Failed to parse nested config item:" << e.what();
                        }
                    }
                }
            }
        }
    } catch (const YAML::Exception &e) {
        qWarning() << "Failed to load config from" << filePath << ":" << e.what();
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

bool QSocConfig::createTemplateConfig(const QString &filePath)
{
    /* Create directory if it doesn't exist */
    const QFileInfo fileInfo(filePath);
    const QDir      directory = fileInfo.dir();

    if (!directory.exists()) {
        if (!directory.mkpath(".")) {
            qWarning() << "Failed to create directory for config file:" << directory.path();
            return false;
        }
    }

    /* Create template config file with comments */
    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        qWarning() << "Failed to create template config file:" << filePath;
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
    out << "#   timeout: 30000\n\n";

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
    out << "# Agent Configuration\n";
    out << "# =============================================================================\n\n";

    out << "# agent:\n";
    out << "#   temperature: 0.2          # LLM temperature (0.0-1.0)\n";
    out << "#   max_tokens: 128000        # Maximum context tokens\n";
    out << "#   max_iterations: 100       # Safety limit for iterations\n";
    out << "#   system_prompt: |          # Custom system prompt\n";
    out << "#     You are a helpful assistant.\n\n";

    out << "# =============================================================================\n";
    out << "# Web Search & Fetch Configuration\n";
    out << "# =============================================================================\n\n";

    out << "# web:\n";
    out << "#   search_api_url: http://localhost:8080  # SearXNG API URL\n";
    out << "#   search_api_key:                        # SearXNG API key (optional)\n";

    file.close();

    qDebug() << "Created template config file:" << filePath;
    return true;
}
