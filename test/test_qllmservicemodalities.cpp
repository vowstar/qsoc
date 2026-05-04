// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "common/qllmservice.h"
#include "common/qsocconfig.h"
#include "qsoc_test.h"

#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtCore>
#include <QtTest>

namespace {

/* Write the yaml under a fresh XDG_CONFIG_HOME so QSocConfig picks it up
 * without touching the developer's real ~/.config/qsoc/qsoc.yml. The
 * caller is responsible for keeping the temp dir alive for the lifetime
 * of any QSocConfig that reads it. */
class ScopedConfig
{
public:
    explicit ScopedConfig(const QByteArray &yaml)
    {
        if (!tempDir.isValid()) {
            qFatal("ScopedConfig: failed to create temp dir");
        }
        const QString qsocDir = tempDir.filePath(QStringLiteral("qsoc"));
        if (!QDir().mkpath(qsocDir)) {
            qFatal("ScopedConfig: mkpath failed for %s", qPrintable(qsocDir));
        }
        const QString yamlPath = QDir(qsocDir).filePath(QStringLiteral("qsoc.yml"));
        QFile         file(yamlPath);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            qFatal("ScopedConfig: open failed: %s", qPrintable(file.errorString()));
        }
        const qint64 written = file.write(yaml);
        if (written != yaml.size()) {
            qFatal(
                "ScopedConfig: short write %lld of %lld",
                static_cast<long long>(written),
                static_cast<long long>(yaml.size()));
        }
        file.close();
        previousXdg = qEnvironmentVariable("XDG_CONFIG_HOME");
        qputenv("XDG_CONFIG_HOME", tempDir.path().toUtf8());
    }
    ~ScopedConfig()
    {
        if (previousXdg.isEmpty()) {
            qunsetenv("XDG_CONFIG_HOME");
        } else {
            qputenv("XDG_CONFIG_HOME", previousXdg.toUtf8());
        }
    }
    ScopedConfig(const ScopedConfig &)            = delete;
    ScopedConfig &operator=(const ScopedConfig &) = delete;

private:
    QTemporaryDir tempDir;
    QString       previousXdg;
};

class TestQLLMServiceModalities : public QObject
{
    Q_OBJECT

private slots:
    /* No modalities block at all -> all defaults: text-only, with the
     * canonical 5000-tok hard cap and 1568-px resize target. */
    void modelWithoutModalitiesBlockIsTextOnly()
    {
        const QByteArray yaml = R"(
llm:
  model: textonly-model
  models:
    textonly-model:
      url: https://example.invalid/v1/chat/completions
      timeout: 1000
)";
        ScopedConfig     scope(yaml);

        auto *config = new QSocConfig(this, nullptr);
        auto *llm    = new QLLMService(this, nullptr);
        llm->setConfig(config);

        const auto cfg = llm->getCurrentModelConfig();
        QCOMPARE(cfg.id, QStringLiteral("textonly-model"));
        QCOMPARE(cfg.acceptsImage, false);
        QCOMPARE(cfg.imageMaxTokens, 5000);
        QCOMPARE(cfg.imageMaxDimension, 1568);
        QVERIFY(cfg.imageProviderHint.isEmpty());
        QCOMPARE(llm->currentSupportsImage(), false);
    }

    /* image: true with no other knobs -> opt-in + defaults preserved. */
    void modelWithImageTrueOnlyKeepsDefaultThresholds()
    {
        const QByteArray yaml = R"(
llm:
  model: vision-default
  models:
    vision-default:
      url: https://example.invalid/v1/chat/completions
      modalities:
        image: true
)";
        ScopedConfig     scope(yaml);

        auto *config = new QSocConfig(this, nullptr);
        auto *llm    = new QLLMService(this, nullptr);
        llm->setConfig(config);

        const auto cfg = llm->getCurrentModelConfig();
        QCOMPARE(cfg.acceptsImage, true);
        QCOMPARE(cfg.imageMaxTokens, 5000);
        QCOMPARE(cfg.imageMaxDimension, 1568);
        QVERIFY(cfg.imageProviderHint.isEmpty());
        QCOMPARE(llm->currentSupportsImage(), true);
    }

    /* Full modalities block flows through every field. */
    void modelWithFullModalitiesBlockReadsAllFields()
    {
        const QByteArray yaml = R"(
llm:
  model: vision-tuned
  models:
    vision-tuned:
      url: https://example.invalid/v1/chat/completions
      modalities:
        image: true
        image_max_tokens: 8000
        image_max_dimension: 1024
        image_provider_hint: openai_high
)";
        ScopedConfig     scope(yaml);

        auto *config = new QSocConfig(this, nullptr);
        auto *llm    = new QLLMService(this, nullptr);
        llm->setConfig(config);

        const auto cfg = llm->getCurrentModelConfig();
        QCOMPARE(cfg.acceptsImage, true);
        QCOMPARE(cfg.imageMaxTokens, 8000);
        QCOMPARE(cfg.imageMaxDimension, 1024);
        QCOMPARE(cfg.imageProviderHint, QStringLiteral("openai_high"));
    }

    /* Explicit image: false should win over defaults too, and crucially
     * over a hypothetical future "auto-detect from id" pathway. Locks in
     * the contract that user config is authoritative. */
    void modelWithImageFalseStaysFalse()
    {
        const QByteArray yaml = R"(
llm:
  model: forced-text
  models:
    forced-text:
      url: https://example.invalid/v1/chat/completions
      modalities:
        image: false
)";
        ScopedConfig     scope(yaml);

        auto *config = new QSocConfig(this, nullptr);
        auto *llm    = new QLLMService(this, nullptr);
        llm->setConfig(config);

        QCOMPARE(llm->currentSupportsImage(), false);
    }

    /* Switching the active model swaps the modality view. */
    void switchingActiveModelUpdatesCapability()
    {
        const QByteArray yaml = R"(
llm:
  model: textonly
  models:
    textonly:
      url: https://example.invalid/v1/chat/completions
    visionable:
      url: https://example.invalid/v1/chat/completions
      modalities:
        image: true
        image_max_tokens: 4000
)";
        ScopedConfig     scope(yaml);

        auto *config = new QSocConfig(this, nullptr);
        auto *llm    = new QLLMService(this, nullptr);
        llm->setConfig(config);

        QCOMPARE(llm->currentSupportsImage(), false);
        QVERIFY(llm->setCurrentModel(QStringLiteral("visionable")));
        QCOMPARE(llm->currentSupportsImage(), true);
        QCOMPARE(llm->getCurrentModelConfig().imageMaxTokens, 4000);
        QVERIFY(llm->setCurrentModel(QStringLiteral("textonly")));
        QCOMPARE(llm->currentSupportsImage(), false);
    }
};

} // namespace

QSOC_TEST_MAIN(TestQLLMServiceModalities)
#include "test_qllmservicemodalities.moc"
