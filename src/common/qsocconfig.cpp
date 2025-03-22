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
    QString userConfigPath = QDir::home().absoluteFilePath(CONFIG_FILE_USER);
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
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();

    /* List of supported environment variables */
    QStringList envVars = {"QSOC_AI_PROVIDER", "QSOC_API_KEY", "QSOC_AI_MODEL", "QSOC_API_URL"};

    /* Load each environment variable if it exists */
    for (const QString &var : envVars) {
        if (env.contains(var)) {
            /* Convert to lowercase key for consistency */
            QString key = var.mid(5).toLower(); /* Remove "QSOC_" prefix */
            setValue(key, env.value(var));
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
            for (const auto &it : config) {
                QString key = QString::fromStdString(it.first.as<std::string>());

                /* Process scalar (string) values */
                if (it.second.IsScalar()) {
                    QString value = QString::fromStdString(it.second.as<std::string>());

                    /* Set value if not already set or if override is true */
                    if (override || !hasKey(key)) {
                        setValue(key, value);
                    }
                }
                /* Process nested maps for provider-specific configurations */
                else if (it.second.IsMap()) {
                    /* Process each key-value pair in the nested map */
                    for (const auto &subIt : it.second) {
                        try {
                            QString subKey = QString::fromStdString(subIt.first.as<std::string>());
                            if (subIt.second.IsScalar()) {
                                /* Create composite key in format "provider.key" */
                                QString compositeKey = key + "." + subKey;
                                QString value        = QString::fromStdString(
                                    subIt.second.as<std::string>());

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
    QString projectPath = projectManager->getProjectPath();
    if (projectPath.isEmpty()) {
        return;
    }

    /* Load from project-level config */
    QString projectConfigPath = QDir(projectPath).filePath(CONFIG_FILE_PROJECT);
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
    QFileInfo fileInfo(filePath);
    QDir      directory = fileInfo.dir();

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
    out << "# This file contains configuration settings for the QSoc application.\n";
    out << "# Uncomment and modify the settings below as needed.\n\n";

    out << "# Configuration Priority Order\n";
    out << "# ---------------------------\n";
    out << "# Settings are applied in the following order of precedence (highest to\n";
    out << "# lowest):\n";
    out << "# 1. Environment variables (QSOC_API_KEY, QSOC_AI_PROVIDER,\n";
    out << "#    QSOC_AI_MODEL, QSOC_API_URL)\n";
    out << "# 2. Global settings in this file\n";
    out << "# 3. Provider-specific settings in this file\n\n";

    out << "# Global Configuration\n";
    out << "# -------------------\n";
    out << "# Global settings have MEDIUM priority (overridden by environment variables\n";
    out << "# but override provider-specific settings).\n";

    out << "# Choose your AI provider by uncommenting one of the following:\n";
    out << "# ai_provider: deepseek   # DeepSeek AI\n";
    out << "# ai_provider: openai     # OpenAI\n";
    out << "# ai_provider: groq       # Groq\n";
    out << "# ai_provider: claude     # Anthropic Claude\n";
    out << "# ai_provider: ollama     # Ollama (local)\n\n";

    out << "# Global API key (used if provider-specific key is not set)\n";
    out << "# api_key: your_api_key_here\n\n";

    out << "# Global model selection (used if provider-specific model is not set)\n";
    out << "# ai_model: gpt-4o-mini\n\n";

    out << "# Global API URL (used if provider-specific URL is not set)\n";
    out << "# api_url: https://custom-api-endpoint.example.com/v1/chat/completions\n\n";

    out << "# Provider-specific Configuration\n";
    out << "# ------------------------------\n";
    out << "# You can specify settings for each provider separately using nested format.\n";
    out << "# Note: Provider-specific settings have the LOWEST priority and will\n";
    out << "# be overridden by global settings and environment variables.\n\n";

    out << "# DeepSeek configuration\n";
    out << "# deepseek:\n";
    out << "#   api_key: your_deepseek_api_key_here\n";
    out << "#   api_url: https://api.deepseek.com/v1/chat/completions\n";
    out << "#   ai_model: deepseek-chat\n\n";

    out << "# OpenAI configuration\n";
    out << "# openai:\n";
    out << "#   api_key: your_openai_api_key_here\n";
    out << "#   api_url: https://api.openai.com/v1/chat/completions\n";
    out << "#   ai_model: gpt-4o-mini\n\n";

    out << "# Groq configuration\n";
    out << "# groq:\n";
    out << "#   api_key: your_groq_api_key_here\n";
    out << "#   api_url: https://api.groq.com/openai/v1/chat/completions\n";
    out << "#   ai_model: mixtral-8x7b-32768\n\n";

    out << "# Claude configuration\n";
    out << "# claude:\n";
    out << "#   api_key: your_claude_api_key_here\n";
    out << "#   api_url: https://api.anthropic.com/v1/messages\n";
    out << "#   ai_model: claude-3-5-sonnet-20241022\n\n";

    out << "# Ollama configuration\n";
    out << "# ollama:\n";
    out << "#   api_url: http://localhost:11434/api/generate\n";
    out << "#   ai_model: llama3\n\n";

    /* Add network proxy configuration section */
    out << "# Network Proxy Configuration\n";
    out << "# -------------------------\n";
    out << "# proxy_type: system     # Use system proxy settings (default)\n";
    out << "# proxy_type: none       # No proxy\n";
    out << "# proxy_type: default    # Use application proxy\n";
    out << "# proxy_type: socks5     # Use SOCKS5 proxy\n";
    out << "# proxy_type: http       # Use HTTP proxy\n";
    out << "# proxy_host: 127.0.0.1  # Proxy server hostname or IP\n";
    out << "# proxy_port: 1080       # Proxy server port\n";
    out << "# proxy_user: username   # Username for proxy authentication (if required)\n";
    out << "# proxy_password: pass   # Password for proxy authentication (if required)\n";

    file.close();

    qDebug() << "Created template config file:" << filePath;
    return true;
}
