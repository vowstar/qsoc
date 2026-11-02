// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/qsocagent.h"
#include "agent/qsocagentconfig.h"
#include "agent/tool/qsoctoolfile.h"
#include "agent/tool/qsoctoolshowfigure.h"
#include "common/qsocimageattach.h"
#include "qsoc_test.h"

#include <QBuffer>
#include <QImage>
#include <QTemporaryDir>
#include <QtTest>

namespace {

QByteArray makePng(int width, int height)
{
    QImage image(width, height, QImage::Format_RGB32);
    image.fill(Qt::cyan);
    QByteArray bytes;
    QBuffer    buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    return bytes;
}

QString writeFile(const QTemporaryDir &dir, const QString &name, const QByteArray &bytes)
{
    QFile file(dir.filePath(name));
    file.open(QIODevice::WriteOnly);
    file.write(bytes);
    return file.fileName();
}

class Test : public QObject
{
    Q_OBJECT

private slots:
    void showsImageAsDisplayOnly();
    void titleBecomesLabel();
    void rejectsTextAndMissingFiles();
    void wrongArgumentTypesAreIgnored();
    void svgSourceAndFilesShowAsSvg();
    void invalidSvgReportsToTheModel();
    void figuresRefuseUnknownTypes();
    void transcriptDropsBackedImageBytes();
    void transcriptKeepsNewestUnsavedImagesWithinBudget();
    void promptFollowsSurface();
};

void Test::showsImageAsDisplayOnly()
{
    QTemporaryDir      dir;
    QSocToolFileRead   reader(this);
    QSocToolShowFigure tool(this, &reader);
    const QString      path   = writeFile(dir, "a.png", makePng(32, 16));
    const QString      result = tool.execute({{"file_path", path.toStdString()}});

    QList<QSocAgent::AttachmentSpec> attachments;
    const QString text = QSocAgent::extractImageAttachments(result, &attachments);
    QVERIFY2(text.startsWith(QStringLiteral("Sent to the user's terminal")), qPrintable(text));
    QVERIFY(text.contains(QStringLiteral("32x16")));
    QCOMPARE(attachments.size(), 1);
    QVERIFY(attachments[0].displayOnly);
    QCOMPARE(attachments[0].sourceUrl, path);
}

void Test::titleBecomesLabel()
{
    QTemporaryDir                    dir;
    QSocToolFileRead                 reader(this);
    QSocToolShowFigure               tool(this, &reader);
    const QString                    path = writeFile(dir, "a.png", makePng(8, 8));
    QList<QSocAgent::AttachmentSpec> attachments;
    QSocAgent::extractImageAttachments(
        tool.execute({{"file_path", path.toStdString()}, {"title", "Power"}}), &attachments);
    QCOMPARE(attachments.size(), 1);
    QCOMPARE(attachments[0].sourceUrl, QStringLiteral("Power"));
}

void Test::rejectsTextAndMissingFiles()
{
    QTemporaryDir      dir;
    QSocToolFileRead   reader(this);
    QSocToolShowFigure tool(this, &reader);
    const QString      text = writeFile(dir, "a.txt", "hello\n");
    QVERIFY(tool.execute({{"file_path", text.toStdString()}}).startsWith(QStringLiteral("Error:")));
    QVERIFY(tool.execute({{"file_path", dir.filePath("none.png").toStdString()}})
                .startsWith(QStringLiteral("Error:")));
    QVERIFY(tool.execute(json::object()).startsWith(QStringLiteral("Error:")));
}

void Test::wrongArgumentTypesAreIgnored()
{
    QTemporaryDir      dir;
    QSocToolFileRead   reader(this);
    QSocToolShowFigure tool(this, &reader);
    const QString      path   = writeFile(dir, "a.png", makePng(8, 8));
    const QString      result = tool.execute(
        {{"file_path", path.toStdString()}, {"inspect", "true"}, {"title", 5}});
    QVERIFY2(result.startsWith(QStringLiteral("Sent")), qPrintable(result.left(80)));
}

void Test::svgSourceAndFilesShowAsSvg()
{
    QTemporaryDir      dir;
    QSocToolFileRead   reader(this);
    QSocToolShowFigure tool(this, &reader);
    const QByteArray svg = "<svg viewBox=\"0 0 10 10\"><rect width=\"10\" height=\"10\"/></svg>\n";
    QList<QSocAgent::AttachmentSpec> attachments;
    QSocAgent::extractImageAttachments(tool.execute({{"svg", svg.toStdString()}}), &attachments);
    QCOMPARE(attachments.size(), 1);
    QCOMPARE(attachments[0].mime, QStringLiteral("image/svg+xml"));
    QVERIFY(attachments[0].displayOnly);
    QCOMPARE(QByteArray::fromBase64(attachments[0].dataB64.toLatin1()), svg);

    const QString path = writeFile(dir, "a.SVG", svg);
    QSocAgent::extractImageAttachments(
        tool.execute({{"file_path", path.toStdString()}}), &attachments);
    QCOMPARE(attachments.size(), 1);
    QCOMPARE(attachments[0].mime, QStringLiteral("image/svg+xml"));
}

void Test::invalidSvgReportsToTheModel()
{
    QSocToolFileRead   reader(this);
    QSocToolShowFigure tool(this, &reader);
    QVERIFY(tool.execute({{"svg", "<html></html>"}}).startsWith(QStringLiteral("Error:")));
    QVERIFY(tool.execute({{"svg", "<svg><rect"}}).startsWith(QStringLiteral("Error:")));
}

void Test::figuresRefuseUnknownTypes()
{
    QTemporaryDir dir;
    QVERIFY(QSocImageAttach::saveFigure(dir.path(), "x", "x/../../y").isEmpty());
    QVERIFY(QSocImageAttach::saveFigure(dir.path(), "", "image/png").isEmpty());
    const QString saved = QSocImageAttach::saveFigure(dir.path(), "x", "image/png");
    QVERIFY(saved.endsWith(QStringLiteral(".png")));
    QCOMPARE(QFileInfo(saved).absolutePath(), QFileInfo(dir.path()).absoluteFilePath());
}

void Test::transcriptDropsBackedImageBytes()
{
    const json part = {{"type", "image_url"}, {"image_url", {{"url", "data:image/png;base64,AA"}}}};
    const json history = json::array(
        {{{"role", "user"},
          {"content", json::array({part})},
          {"_img_tokens", 1},
          {"_qsoc_figures_backed", true}}});
    QVERIFY(QSocAgent::transcriptForFrontend(history)[0]["content"].empty());
}

void Test::transcriptKeepsNewestUnsavedImagesWithinBudget()
{
    const std::string body(5 * 1024 * 1024, 'A');
    const json        part
        = {{"type", "image_url"}, {"image_url", {{"url", "data:image/png;base64," + body}}}};
    const json message = {{"role", "user"}, {"content", json::array({part})}, {"_img_tokens", 1}};
    const json transcript = QSocAgent::transcriptForFrontend(json::array({message, message}));
    QVERIFY(transcript[0]["content"].empty());
    QCOMPARE(transcript[1]["content"].size(), size_t(1));
}

void Test::promptFollowsSurface()
{
    QSocAgentConfig config;
    const auto      prompt = [&config](const char *surface, bool child = false) {
        config.displaySurface = QString::fromLatin1(surface);
        config.isSubAgent     = child;
        QSocAgent agent(nullptr, nullptr, nullptr, config);
        return agent.buildSystemPromptWithMemory(false);
    };
    QVERIFY(!prompt("none").contains(QStringLiteral("# Visual output")));
    QVERIFY(prompt("bitmap").contains(QStringLiteral("show_figure")));
    QVERIFY(prompt("bitmap").contains(QStringLiteral("```svg")));
    QVERIFY(!prompt("cells").contains(QStringLiteral("```svg")));
    QVERIFY(!prompt("bitmap").contains(QStringLiteral("block-character")));
    QVERIFY(prompt("cells").contains(QStringLiteral("block-character")));
    QVERIFY(!prompt("bitmap", true).contains(QStringLiteral("# Visual output")));
}

} // namespace

QSOC_TEST_MAIN(Test)
#include "test_qsoctoolshowfigure.moc"
