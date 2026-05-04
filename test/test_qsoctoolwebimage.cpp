// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

/* C3 unit test for QSocToolWebFetch's image-mime branch. We exercise the
 * pure decision logic via a friend-style probe helper: the public
 * handleImageResponse path requires a network round-trip, but the
 * relevant decisions (capability gate, token budget, resize) all flow
 * through that one method given a (sourceUrl, contentType, body)
 * triple. Crucially: no real image files are committed to the repo:
 * fixtures are generated at runtime via QImage. */

#include "agent/tool/qsoctoolweb.h"
#include "common/qllmservice.h"
#include "common/qsocconfig.h"
#include "qsoc_test.h"

#include <nlohmann/json.hpp>
#include <QBuffer>
#include <QColor>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QPainter>
#include <QTemporaryDir>
#include <QtTest>

namespace {

class ScopedConfig
{
public:
    explicit ScopedConfig(const QByteArray &yaml)
    {
        if (!tempDir.isValid()) {
            qFatal("ScopedConfig: temp dir invalid");
        }
        const QString qsocDir = tempDir.filePath(QStringLiteral("qsoc"));
        if (!QDir().mkpath(qsocDir)) {
            qFatal("ScopedConfig: mkpath failed");
        }
        QFile file(QDir(qsocDir).filePath(QStringLiteral("qsoc.yml")));
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            qFatal("ScopedConfig: open failed: %s", qPrintable(file.errorString()));
        }
        file.write(yaml);
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

/* Generate a solid-colour PNG of given dimensions in memory. Anchored on
 * QImage so the shape, header, and decode paths are exercised without
 * shipping any binary fixture. */
QByteArray makePng(int width, int height, const QColor &fill)
{
    QImage img(width, height, QImage::Format_RGB32);
    img.fill(fill);
    QByteArray bytes;
    QBuffer    buf(&bytes);
    buf.open(QIODevice::WriteOnly);
    img.save(&buf, "PNG");
    return bytes;
}

/* Drive handleImageResponse directly: it is the pure decision function
 * over (url, mime, bytes, model-config) with no network I/O. Going
 * through execute() would require an HTTP server fixture and would
 * exercise QNetworkAccessManager rather than the image-mime logic we
 * actually care about. */
QString runOn(
    QSocToolWebFetch *tool, const QString &sourceUrl, const QString &mime, const QByteArray &bytes)
{
    return tool->handleImageResponse(sourceUrl, mime, bytes);
}

class TestQSocToolWebImage : public QObject
{
    Q_OBJECT

private slots:
    /* No LLM pointer at all -> must always degrade to text fallback even
     * for valid PNGs. The constructor's optional argument defaulting to
     * nullptr is the remote-mode case. */
    void noLlmFallsBackToText()
    {
        const QByteArray png  = makePng(256, 256, QColor(Qt::red));
        auto            *tool = new QSocToolWebFetch(this, /*config=*/nullptr, /*llm=*/nullptr);
        const QString    result
            = runOn(tool, QStringLiteral("https://e/x.png"), QStringLiteral("image/png"), png);
        delete tool;

        QVERIFY2(result.startsWith(QStringLiteral("[image:")), qPrintable(result.left(200)));
        QVERIFY(result.contains(QStringLiteral("not inlined")));
        QVERIFY(result.contains(QStringLiteral("does not accept images")));
        QVERIFY(!result.contains(QString::fromLatin1(QSocToolWebFetch::attachmentMarkerOpen())));
    }

    /* Text-only model -> same fallback, but reason mentions the model. */
    void textOnlyModelFallsBack()
    {
        const QByteArray yaml = R"(
llm:
  model: textonly
  models:
    textonly:
      url: http://example.invalid/v1/chat/completions
)";
        ScopedConfig     scope(yaml);

        auto *config = new QSocConfig(this, nullptr);
        auto *llm    = new QLLMService(this, nullptr);
        llm->setConfig(config);

        const QByteArray png  = makePng(256, 256, QColor(Qt::green));
        auto            *tool = new QSocToolWebFetch(this, config, llm);
        const QString    result
            = runOn(tool, QStringLiteral("https://e/x.png"), QStringLiteral("image/png"), png);
        delete tool;

        QVERIFY2(result.contains(QStringLiteral("not inlined")), qPrintable(result.left(200)));
    }

    /* Vision model + small image -> attachment marker present, JSON has
     * the expected fields, est_tokens within Anthropic-formula range. */
    void visionModelInlinesSmallImage()
    {
        const QByteArray yaml = R"(
llm:
  model: vision
  models:
    vision:
      url: http://example.invalid/v1/chat/completions
      modalities:
        image: true
        image_max_tokens: 10000
        image_max_dimension: 1568
)";
        ScopedConfig     scope(yaml);

        auto *config = new QSocConfig(this, nullptr);
        auto *llm    = new QLLMService(this, nullptr);
        llm->setConfig(config);

        const QByteArray png  = makePng(512, 512, QColor(Qt::blue));
        auto            *tool = new QSocToolWebFetch(this, config, llm);
        const QString    result
            = runOn(tool, QStringLiteral("https://e/blue.png"), QStringLiteral("image/png"), png);
        delete tool;

        const QString openMarker  = QString::fromLatin1(QSocToolWebFetch::attachmentMarkerOpen());
        const QString closeMarker = QString::fromLatin1(QSocToolWebFetch::attachmentMarkerClose());
        QVERIFY2(result.contains(openMarker), qPrintable(result.left(200)));
        QVERIFY(result.contains(closeMarker));
        QVERIFY(result.startsWith(QStringLiteral("[image attached:")));

        const int openIdx  = result.indexOf(openMarker);
        const int closeIdx = result.indexOf(closeMarker);
        QVERIFY(openIdx > 0 && closeIdx > openIdx);
        const QString jsonText
            = result.mid(openIdx + openMarker.size(), closeIdx - openIdx - openMarker.size());

        const auto payload = nlohmann::json::parse(jsonText.toStdString());
        QCOMPARE(payload["width"].get<int>(), 512);
        QCOMPARE(payload["height"].get<int>(), 512);
        /* Anthropic formula: 512*512/750 = 349. */
        QCOMPARE(payload["est_tokens"].get<int>(), 349);
        QVERIFY(payload["data"].get<std::string>().size() > 100);
        QVERIFY(payload["mime"].get<std::string>().rfind("image/", 0) == 0);
        QCOMPARE(payload["resized"].get<bool>(), false);
    }

    /* Vision model + oversize image with imageMaxDimension < source ->
     * the resize path runs and est_tokens drops below the cap. */
    void visionModelResizesOversizeImage()
    {
        const QByteArray yaml = R"(
llm:
  model: vision
  models:
    vision:
      url: http://example.invalid/v1/chat/completions
      modalities:
        image: true
        image_max_tokens: 10000
        image_max_dimension: 1024
)";
        ScopedConfig     scope(yaml);

        auto *config = new QSocConfig(this, nullptr);
        auto *llm    = new QLLMService(this, nullptr);
        llm->setConfig(config);

        const QByteArray png  = makePng(4096, 4096, QColor(Qt::yellow));
        auto            *tool = new QSocToolWebFetch(this, config, llm);
        const QString    result
            = runOn(tool, QStringLiteral("https://e/yellow.png"), QStringLiteral("image/png"), png);
        delete tool;

        const QString openMarker = QString::fromLatin1(QSocToolWebFetch::attachmentMarkerOpen());
        QVERIFY2(result.contains(openMarker), qPrintable(result.left(200)));

        const int     openIdx     = result.indexOf(openMarker);
        const QString closeMarker = QString::fromLatin1(QSocToolWebFetch::attachmentMarkerClose());
        const int     closeIdx    = result.indexOf(closeMarker);
        const QString jsonText
            = result.mid(openIdx + openMarker.size(), closeIdx - openIdx - openMarker.size());
        const auto payload = nlohmann::json::parse(jsonText.toStdString());

        QCOMPARE(payload["resized"].get<bool>(), true);
        QCOMPARE(payload["width"].get<int>(), 1024);
        QCOMPARE(payload["height"].get<int>(), 1024);
        QVERIFY(payload["est_tokens"].get<int>() <= 10000);
    }

    /* Vision model with an aggressively low image_max_tokens that even
     * the resized image exceeds -> fallback, with a reason explaining
     * the post-resize overage. */
    void visionModelRejectsBudgetOverflowEvenAfterResize()
    {
        const QByteArray yaml = R"(
llm:
  model: vision
  models:
    vision:
      url: http://example.invalid/v1/chat/completions
      modalities:
        image: true
        image_max_tokens: 50
        image_max_dimension: 1024
)";
        ScopedConfig     scope(yaml);

        auto *config = new QSocConfig(this, nullptr);
        auto *llm    = new QLLMService(this, nullptr);
        llm->setConfig(config);

        const QByteArray png  = makePng(2048, 2048, QColor(Qt::magenta));
        auto            *tool = new QSocToolWebFetch(this, config, llm);
        const QString    result
            = runOn(tool, QStringLiteral("https://e/m.png"), QStringLiteral("image/png"), png);
        delete tool;

        QVERIFY2(result.contains(QStringLiteral("not inlined")), qPrintable(result.left(200)));
        QVERIFY(result.contains(QStringLiteral("even after resize")));
        QVERIFY(!result.contains(QString::fromLatin1(QSocToolWebFetch::attachmentMarkerOpen())));
    }
};

} // namespace

QSOC_TEST_MAIN(TestQSocToolWebImage)
#include "test_qsoctoolwebimage.moc"
