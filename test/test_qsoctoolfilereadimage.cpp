// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

/* read_file with image MIME -> attachment marker. The fixtures are
 * generated at runtime; nothing binary is committed to the repo. */

#include "agent/tool/qsoctoolfile.h"
#include "common/qllmservice.h"
#include "common/qsocconfig.h"
#include "common/qsocimageattach.h"
#include "qsoc_test.h"

#include <nlohmann/json.hpp>
#include <QBuffer>
#include <QColor>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QTemporaryDir>
#include <QtTest>

namespace {

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

QString writeTempFile(QTemporaryDir &dir, const QString &name, const QByteArray &data)
{
    const QString path = dir.filePath(name);
    QFile         file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        qFatal("writeTempFile open failed");
    }
    file.write(data);
    file.close();
    return path;
}

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

QString runRead(QSocToolFileRead *tool, const QString &path)
{
    nlohmann::json args;
    args["file_path"] = path.toStdString();
    return tool->execute(args);
}

class TestQSocToolFileReadImage : public QObject
{
    Q_OBJECT

private slots:
    /* Vision-capable model: read_file on a PNG returns the same
     * attachment marker the agent main loop knows how to lift. */
    void visionModelInlinesPng()
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
        auto            *config = new QSocConfig(this, nullptr);
        auto            *llm    = new QLLMService(this, nullptr);
        llm->setConfig(config);

        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const QString path
            = writeTempFile(tmp, QStringLiteral("p.png"), makePng(512, 512, QColor(Qt::cyan)));

        auto         *tool   = new QSocToolFileRead(this, /*pathContext=*/nullptr, llm);
        const QString result = runRead(tool, path);
        delete tool;

        const QString openMarker = QString::fromLatin1(QSocImageAttach::attachmentMarkerOpen());
        QVERIFY2(result.contains(openMarker), qPrintable(result.left(200)));
        QVERIFY(result.startsWith(QStringLiteral("[image attached:")));
    }

    /* Text-only model gets the alt-text fallback even when the file
     * is unmistakably a PNG. */
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
        auto            *config = new QSocConfig(this, nullptr);
        auto            *llm    = new QLLMService(this, nullptr);
        llm->setConfig(config);

        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const QString path
            = writeTempFile(tmp, QStringLiteral("q.png"), makePng(64, 64, QColor(Qt::magenta)));

        auto         *tool   = new QSocToolFileRead(this, nullptr, llm);
        const QString result = runRead(tool, path);
        delete tool;

        QVERIFY2(result.contains(QStringLiteral("not inlined")), qPrintable(result.left(200)));
        const QString openMarker = QString::fromLatin1(QSocImageAttach::attachmentMarkerOpen());
        QVERIFY(!result.contains(openMarker));
    }

    /* No LLM pointer at all (remote-mode parity): same fallback. */
    void noLlmFallsBackToText()
    {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const QByteArray bytes = makePng(64, 64, QColor(Qt::yellow));
        const QString    path  = writeTempFile(tmp, QStringLiteral("r.png"), bytes);

        auto         *tool   = new QSocToolFileRead(this, nullptr, nullptr);
        const QString result = runRead(tool, path);
        delete tool;
        QVERIFY2(result.contains(QStringLiteral("not inlined")), qPrintable(result.left(200)));
    }

    /* Plain text file must continue through the normal text path; the
     * image branch only triggers on a magic-byte hit. */
    void textFileStillReadsAsText()
    {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const QString path = writeTempFile(
            tmp, QStringLiteral("note.txt"), QByteArrayLiteral("hello world\nline 2\n"));

        auto         *tool   = new QSocToolFileRead(this, nullptr, nullptr);
        const QString result = runRead(tool, path);
        delete tool;

        QVERIFY2(result.contains(QStringLiteral("hello world")), qPrintable(result));
        QVERIFY(result.contains(QStringLiteral("line 2")));
        const QString openMarker = QString::fromLatin1(QSocImageAttach::attachmentMarkerOpen());
        QVERIFY(!result.contains(openMarker));
    }

    /* Misnamed file: a PNG saved as `.txt` must still attach when the
     * model accepts images. Extension is advisory; magic bytes win. */
    void misnamedExtensionStillDetectsImage()
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
        auto            *config = new QSocConfig(this, nullptr);
        auto            *llm    = new QLLMService(this, nullptr);
        llm->setConfig(config);

        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const QString path
            = writeTempFile(tmp, QStringLiteral("README.txt"), makePng(96, 96, QColor(Qt::darkBlue)));

        auto         *tool   = new QSocToolFileRead(this, nullptr, llm);
        const QString result = runRead(tool, path);
        delete tool;

        const QString openMarker = QString::fromLatin1(QSocImageAttach::attachmentMarkerOpen());
        QVERIFY2(result.contains(openMarker), qPrintable(result.left(200)));
    }

    /* Vision model: getDescription advertises image-reading so the
     * model knows the tool can return visual content. */
    void visionModelDescriptionMentionsImages()
    {
        const QByteArray yaml = R"(
llm:
  model: vision
  models:
    vision:
      url: http://example.invalid/v1/chat/completions
      modalities:
        image: true
)";
        ScopedConfig     scope(yaml);
        auto            *config = new QSocConfig(this, nullptr);
        auto            *llm    = new QLLMService(this, nullptr);
        llm->setConfig(config);

        QSocToolFileRead tool(this, nullptr, llm);
        const QString    desc = tool.getDescription();
        QVERIFY(desc.contains(QStringLiteral("PNG")));
        QVERIFY(desc.contains(QStringLiteral("multimodal")));
        QVERIFY(desc.contains(QStringLiteral("ALWAYS use this tool")));
    }

    /* Text-only model: description must NOT promise image reading
     * so the model never asks for a capability the endpoint lacks. */
    void textOnlyModelDescriptionOmitsImages()
    {
        const QByteArray yaml = R"(
llm:
  model: textonly
  models:
    textonly:
      url: http://example.invalid/v1/chat/completions
)";
        ScopedConfig     scope(yaml);
        auto            *config = new QSocConfig(this, nullptr);
        auto            *llm    = new QLLMService(this, nullptr);
        llm->setConfig(config);

        QSocToolFileRead tool(this, nullptr, llm);
        const QString    desc = tool.getDescription();
        QVERIFY(!desc.contains(QStringLiteral("PNG")));
        QVERIFY(!desc.contains(QStringLiteral("multimodal")));
        QVERIFY(!desc.contains(QStringLiteral("screenshot")));
    }

    /* Null LLM (remote-mode parity): conservative text-only desc. */
    void nullLlmDescriptionOmitsImages()
    {
        QSocToolFileRead tool(this, nullptr, nullptr);
        const QString    desc = tool.getDescription();
        QVERIFY(!desc.contains(QStringLiteral("PNG")));
        QVERIFY(!desc.contains(QStringLiteral("multimodal")));
    }

    /* Switching the active model flips the description on the next
     * read so the agent sends the right tool schema next turn. */
    void switchingModelUpdatesDescription()
    {
        const QByteArray yaml = R"(
llm:
  model: vision
  models:
    vision:
      url: http://example.invalid/v1/chat/completions
      modalities:
        image: true
    textonly:
      url: http://example.invalid/v1/chat/completions
)";
        ScopedConfig     scope(yaml);
        auto            *config = new QSocConfig(this, nullptr);
        auto            *llm    = new QLLMService(this, nullptr);
        llm->setConfig(config);

        QSocToolFileRead tool(this, nullptr, llm);
        QVERIFY(tool.getDescription().contains(QStringLiteral("PNG")));

        QVERIFY(llm->setCurrentModel(QStringLiteral("textonly")));
        QVERIFY(!tool.getDescription().contains(QStringLiteral("PNG")));

        QVERIFY(llm->setCurrentModel(QStringLiteral("vision")));
        QVERIFY(tool.getDescription().contains(QStringLiteral("PNG")));
    }
};

} // namespace

QSOC_TEST_MAIN(TestQSocToolFileReadImage)
#include "test_qsoctoolfilereadimage.moc"
