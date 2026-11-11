// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/qsocagent.h"
#include "agent/tool/qsoctooldocument.h"
#include "agent/tool/qsoctoolpath.h"
#include "common/qsocimageattach.h"

#include <QBuffer>
#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QFontDatabase>
#include <QImage>
#include <QPainter>
#include <QPdfWriter>
#include <QScopeGuard>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>

namespace {

using Member = QPair<QByteArray, QByteArray>;

quint32 crc32(const QByteArray &bytes)
{
    quint32 crc = 0xffffffffU;
    for (const char ch : bytes) {
        crc ^= static_cast<unsigned char>(ch);
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1U)));
    }
    return ~crc;
}

QByteArray makeZip(const QList<Member> &members, quint32 declaredSize = 0, quint16 flags = 0)
{
    QByteArray  bytes, directory;
    QDataStream output(&bytes, QIODevice::WriteOnly);
    QDataStream central(&directory, QIODevice::WriteOnly);
    output.setByteOrder(QDataStream::LittleEndian);
    central.setByteOrder(QDataStream::LittleEndian);
    for (const auto &member : members) {
        const quint32 offset   = bytes.size();
        const quint32 size     = member.second.size();
        const quint32 expanded = declaredSize == 0 ? size : declaredSize;
        const quint32 crc      = crc32(member.second);
        output << quint32(0x04034b50) << quint16(20) << flags << quint16(0) << quint16(0)
               << quint16(0) << crc << size << expanded << quint16(member.first.size())
               << quint16(0);
        output.writeRawData(member.first.constData(), member.first.size());
        output.writeRawData(member.second.constData(), member.second.size());
        central << quint32(0x02014b50) << quint16(20) << quint16(20) << flags << quint16(0)
                << quint16(0) << quint16(0) << crc << size << expanded
                << quint16(member.first.size()) << quint16(0) << quint16(0) << quint16(0)
                << quint16(0) << quint32(0) << offset;
        central.writeRawData(member.first.constData(), member.first.size());
    }
    const quint32 directoryOffset = bytes.size();
    output.writeRawData(directory.constData(), directory.size());
    output << quint32(0x06054b50) << quint16(0) << quint16(0) << quint16(members.size())
           << quint16(members.size()) << quint32(directory.size()) << directoryOffset << quint16(0);
    return bytes;
}

QString save(const QTemporaryDir &directory, const QString &name, const QByteArray &bytes)
{
    const QString path = directory.filePath(name);
    QFile         file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size())
        qFatal("Cannot write document input");
    return path;
}

QByteArray wordBody()
{
    return QByteArray(
        "<w:document "
        "xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\"><w:body>"
        "<w:p><w:r><w:t>Clock specification 时钟规格</w:t></w:r></w:p>"
        "<w:tbl><w:tr><w:tc><w:p><w:r><w:t>Frequency</w:t></w:r></w:p></w:tc>"
        "<w:tc><w:p><w:r><w:t>100 MHz</w:t></w:r></w:p></w:tc></w:tr></w:tbl>"
        "</w:body></w:document>");
}

json result(QSocToolDocumentRead &tool, const json &arguments)
{
    const QString text = tool.execute(arguments);
    if (text.startsWith("Error:"))
        qFatal("%s", qPrintable(text));
    return json::parse(text.toStdString());
}

QString createPdf(const QTemporaryDir &directory, const QFont &font)
{
    const QString path = directory.filePath("sample.pdf");
    QPdfWriter    pdf(path);
    pdf.setResolution(72);
    pdf.setPageSize(QPageSize(QPageSize::A4));
    QPainter painter(&pdf);
    if (!painter.isActive())
        qFatal("Cannot create PDF input");
    painter.setFont(font);
    painter.drawText(30, 60, QStringLiteral("Clock frequency: 100 MHz"));
    painter.drawLine(30, 80, 300, 80);
    if (!pdf.newPage())
        qFatal("Cannot add PDF page");
    painter.drawText(30, 60, QStringLiteral("Reset release: 2 cycles"));
    if (!pdf.newPage())
        qFatal("Cannot add PDF page");
    painter.setPen(QPen(QColor(30, 80, 160), 2));
    painter.drawRect(QRect(30, 60, 220, 120));
    painter.end();
    return path;
}

class TestQSocToolDocument : public QObject
{
    Q_OBJECT

private:
    QFont pdfFont_;

private slots:
    void initTestCase()
    {
        const int id = QFontDatabase::addApplicationFont(
            QFINDTESTDATA("../resource/report/DroidSansMono.ttf"));
        QVERIFY(id >= 0);
        const auto families = QFontDatabase::applicationFontFamilies(id);
        QVERIFY(!families.isEmpty());
        pdfFont_ = QFont(families.first(), 12);
    }

    void docxPreservesParagraphsAndTableText()
    {
        if (QStandardPaths::findExecutable("unzip").isEmpty())
            QSKIP("unzip is optional");
        QTemporaryDir directory;
        const auto    path = save(
            directory,
            QStringLiteral("测试 文档.docx"),
            makeZip({{"word/document.xml", wordBody()}}));
        QSocPathContext paths;
        paths.setWorkingDir(directory.path());
        QSocToolDocumentRead tool(nullptr, &paths);
        QVERIFY(tool.isReadOnly());
        const auto data = result(tool, {{"file_path", QFileInfo(path).fileName().toStdString()}});
        QCOMPARE(data["format"], json("docx"));
        QCOMPARE(data["parts"][0]["source"], json("word/document.xml"));
        const auto text = QString::fromStdString(data["parts"][0]["text"].get<std::string>());
        QVERIFY(text.contains(QStringLiteral("时钟规格")));
        QVERIFY(text.contains("[paragraph 1] Clock specification"));
        QVERIFY(text.contains("Frequency"));
        QVERIFY(text.contains("100 MHz"));
        QVERIFY(text.contains('\t'));
    }

    void docxTextPagination()
    {
        if (QStandardPaths::findExecutable("unzip").isEmpty())
            QSKIP("unzip is optional");
        QTemporaryDir directory;
        const auto    path
            = save(directory, "sample.docx", makeZip({{"word/document.xml", wordBody()}}));
        QSocToolDocumentRead tool;
        const auto first = result(tool, {{"file_path", path.toStdString()}, {"max_chars", 12}});
        QVERIFY(first["parts"][0]["truncated"].get<bool>());
        QCOMPARE(first["parts"][0]["next_offset"], json(12));
        const auto next
            = result(tool, {{"file_path", path.toStdString()}, {"offset", 12}, {"max_chars", 12}});
        QVERIFY(first["parts"][0]["text"] != next["parts"][0]["text"]);
    }

    void converterCanCompleteAfterMemoryChecks()
    {
#ifndef Q_OS_UNIX
        QSKIP("This converter fixture needs a POSIX shell");
#else
        QTemporaryDir directory;
        const auto    converter = save(
            directory,
            "unzip",
            QByteArray("#!/bin/sh\n/bin/sleep 0.1\nprintf '%s' '") + wordBody() + "'\n");
        QVERIFY(QFile::setPermissions(converter, QFileDevice::ReadOwner | QFileDevice::ExeOwner));
        const auto path
            = save(directory, "sample.docx", makeZip({{"word/document.xml", wordBody()}}));
        const auto oldPath = qgetenv("PATH");
        const auto restore = qScopeGuard([&] { qputenv("PATH", oldPath); });
        qputenv("PATH", directory.path().toUtf8());
        QSocToolDocumentRead tool;
        const auto           data = result(tool, {{"file_path", path.toStdString()}});
        QVERIFY(
            QString::fromStdString(data["parts"][0]["text"].get<std::string>())
                .contains("Clock specification"));
#endif
    }

    void xlsxUsesSheetOrderNamesAndCellAddresses()
    {
        if (QStandardPaths::findExecutable("unzip").isEmpty())
            QSKIP("unzip is optional");
        QTemporaryDir    directory;
        const QByteArray workbook
            = "<workbook xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\" "
              "xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/"
              "relationships\"><sheets><sheet name=\"Timing\" sheetId=\"1\" r:id=\"rId2\"/><sheet "
              "name=\"Power\" sheetId=\"2\" r:id=\"rId1\"/></sheets></workbook>";
        const QByteArray relations = "<Relationships><Relationship Id=\"rId1\" "
                                     "Target=\"worksheets/sheet1.xml\"/><Relationship Id=\"rId2\" "
                                     "Target=\"worksheets/sheet2.xml\"/></Relationships>";
        const QByteArray sheet
            = "<worksheet><sheetData><row r=\"1\"><c r=\"A1\" t=\"s\"><v>0</v></c><c "
              "r=\"C1\"><f>2+3</f><v>5</v></c><c r=\"D1\" "
              "t=\"inlineStr\"><is><t>MHz</t></is></c></row></sheetData></worksheet>";
        const auto path = save(
            directory,
            "sample.xlsx",
            makeZip(
                {{"xl/workbook.xml", workbook},
                 {"xl/_rels/workbook.xml.rels", relations},
                 {"xl/sharedStrings.xml",
                  "<sst><si><r><t>Clock</t></r><rPh><t>phonetic</t></rPh><r><t> "
                  "rate</t></r></si></sst>"},
                 {"xl/worksheets/sheet1.xml", sheet},
                 {"xl/worksheets/sheet2.xml", sheet}}));
        QSocToolDocumentRead tool;
        const auto data = result(tool, {{"file_path", path.toStdString()}, {"page_count", 2}});
        QCOMPARE(data["parts"][0]["title"], json("Timing"));
        QCOMPARE(data["parts"][0]["source"], json("xl/worksheets/sheet2.xml"));
        QCOMPARE(data["parts"][1]["title"], json("Power"));
        const auto text = QString::fromStdString(data["parts"][0]["text"].get<std::string>());
        QVERIFY(text.contains("A1\tClock rate"));
        QVERIFY(!text.contains("phonetic"));
        QVERIFY(text.contains("C1\t5\tformula: 2+3"));
        QVERIFY(text.contains("D1\tMHz"));
    }

    void pptxUsesPresentationOrder()
    {
        if (QStandardPaths::findExecutable("unzip").isEmpty())
            QSKIP("unzip is optional");
        QTemporaryDir directory;
        const auto    path = save(
            directory,
            "sample.pptx",
            makeZip(
                {{"ppt/presentation.xml",
                  "<p:presentation "
                  "xmlns:p=\"http://schemas.openxmlformats.org/presentationml/2006/main\" "
                  "xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/"
                  "relationships\"><p:sldIdLst><p:sldId r:id=\"rId2\" id=\"256\"/><p:sldId "
                  "id=\"257\" r:id=\"rId1\"/></p:sldIdLst></p:presentation>"},
                 {"ppt/_rels/presentation.xml.rels",
                  "<Relationships><Relationship Id=\"rId1\" "
                  "Target=\"slides/slide1.xml\"/><Relationship Id=\"rId2\" "
                  "Target=\"slides/slide2.xml\"/></Relationships>"},
                 {"ppt/slides/slide1.xml", "<slide><p><t>Reset sequence</t></p></slide>"},
                 {"ppt/slides/slide2.xml", "<slide><p><t>Clock tree</t></p></slide>"}}));
        QSocToolDocumentRead tool;
        const auto data = result(tool, {{"file_path", path.toStdString()}, {"page_count", 2}});
        QCOMPARE(data["parts"][0]["source"], json("ppt/slides/slide2.xml"));
        QVERIFY(
            QString::fromStdString(data["parts"][0]["text"].get<std::string>())
                .contains("Clock tree"));
        QVERIFY(
            QString::fromStdString(data["parts"][1]["text"].get<std::string>())
                .contains("Reset sequence"));
    }

    void repeatedSharedStringsUseBoundedTextWindows()
    {
        if (QStandardPaths::findExecutable("unzip").isEmpty())
            QSKIP("unzip is optional");
        QTemporaryDir directory;
        QByteArray    sheet("<worksheet><sheetData>");
        for (int row = 1; row <= 128; ++row) {
            sheet += "<row><c r=\"A" + QByteArray::number(row) + "\" t=\"s\"><v>0</v></c></row>";
        }
        sheet += "</sheetData></worksheet>";
        const int  valueLength = 2 * 1024 * 1024;
        const auto path        = save(
            directory,
            "repeated.xlsx",
            makeZip(
                {{"xl/workbook.xml",
                  "<workbook "
                  "xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/"
                  "relationships\"><sheets><sheet name=\"Repeated\" "
                  "r:id=\"rId1\"/></sheets></workbook>"},
                 {"xl/_rels/workbook.xml.rels",
                  "<Relationships><Relationship Id=\"rId1\" "
                  "Target=\"worksheets/sheet1.xml\"/></Relationships>"},
                 {"xl/sharedStrings.xml",
                  QByteArray("<sst><si><t>") + QByteArray(valueLength, 'Z') + "</t></si></sst>"},
                 {"xl/worksheets/sheet1.xml", sheet}}));
        QSocToolDocumentRead tool;
        const auto  first = result(tool, {{"file_path", path.toStdString()}, {"max_chars", 256}});
        const auto &part  = first["parts"][0];
        QCOMPARE(part["text"].get<std::string>().size(), std::size_t(256));
        QVERIFY(part["truncated"].get<bool>());
        QCOMPARE(part["next_offset"], json(256));
        const auto next
            = result(tool, {{"file_path", path.toStdString()}, {"offset", 256}, {"max_chars", 256}});
        QCOMPARE(next["parts"][0]["text"], json(std::string(256, 'Z')));
        const auto secondCell = result(
            tool,
            {{"file_path", path.toStdString()}, {"offset", valueLength + 4}, {"max_chars", 8}});
        QCOMPARE(secondCell["parts"][0]["text"], json("A2\tZZZZZ"));
    }

    void textWindowPreservesUnicodeCharacters()
    {
        if (QStandardPaths::findExecutable("unzip").isEmpty())
            QSKIP("unzip is optional");
        QTemporaryDir directory;
        const QString emoji = QString::fromUcs4(U"😀");
        const auto    path  = save(
            directory,
            "unicode.docx",
            makeZip(
                {{"word/document.xml",
                  (QStringLiteral("<doc><p><t>") + emoji + QStringLiteral("1234</t></p></doc>"))
                      .toUtf8()}}));
        QSocToolDocumentRead tool;
        const auto           full = result(tool, {{"file_path", path.toStdString()}});
        const auto text   = QString::fromStdString(full["parts"][0]["text"].get<std::string>());
        const int  offset = text.indexOf(emoji);
        QVERIFY(offset >= 0);
        const auto first
            = result(tool, {{"file_path", path.toStdString()}, {"offset", offset}, {"max_chars", 1}});
        QCOMPARE(QString::fromStdString(first["parts"][0]["text"].get<std::string>()), emoji);
        QCOMPARE(first["parts"][0]["next_offset"], json(offset + 2));
        const auto next = result(
            tool, {{"file_path", path.toStdString()}, {"offset", offset + 2}, {"max_chars", 2}});
        QCOMPARE(next["parts"][0]["text"], json("12"));
    }

    void listRejectsMissingDocumentBody()
    {
        QTemporaryDir directory;
        const auto    path = save(directory, "missing.docx", makeZip({{"other.xml", "<other/>"}}));
        QSocToolDocumentRead tool;
        QVERIFY(tool.execute({{"file_path", path.toStdString()}, {"mode", "list"}})
                    .contains("part is missing"));
    }

    void rejectsUnsafeArchives()
    {
        QTemporaryDir           directory;
        QSocToolDocumentRead    tool;
        const QList<QByteArray> archives
            = {makeZip({{"../escape.xml", wordBody()}}),
               makeZip({{"word/document.xml", wordBody()}}, 9 * 1024 * 1024),
               makeZip({{"word/document.xml", wordBody()}}, 0, 1),
               makeZip({{"word/document.xml", wordBody()}, {"word/document.xml", wordBody()}})};
        for (int i = 0; i < archives.size(); ++i) {
            const auto path
                = save(directory, QStringLiteral("unsafe%1.docx").arg(i), archives.at(i));
            QVERIFY(tool.execute({{"file_path", path.toStdString()}}).startsWith("Error:"));
        }
        QList<Member> members;
        for (int i = 0; i < 9; ++i)
            members.append({QByteArray::number(i), "x"});
        const auto total = save(directory, "total.docx", makeZip(members, 8 * 1024 * 1024));
        QVERIFY(tool.execute({{"file_path", total.toStdString()}}).contains("expanded sizes"));
        members.clear();
        for (int i = 0; i < 4097; ++i)
            members.append({QByteArray::number(i), "x"});
        const auto count = save(directory, "count.docx", makeZip(members));
        QVERIFY(tool.execute({{"file_path", count.toStdString()}}).contains("directory limits"));
    }

    void rejectsDtd()
    {
        if (QStandardPaths::findExecutable("unzip").isEmpty())
            QSKIP("unzip is optional");
        QTemporaryDir directory;
        const auto    path = save(
            directory,
            "entities.docx",
            makeZip(
                {{"word/document.xml",
                  "<!DOCTYPE doc [<!ENTITY value 'expanded'>]><doc><p><t>&value;</t></p></doc>"}}));
        QSocToolDocumentRead tool;
        QVERIFY(tool.execute({{"file_path", path.toStdString()}}).contains("DTD"));
    }

    void pdfPageTextAndEmptyTextLayer()
    {
        if (QStandardPaths::findExecutable("pdfinfo").isEmpty()
            || QStandardPaths::findExecutable("pdftotext").isEmpty())
            QSKIP("Poppler is optional");
        QTemporaryDir        directory;
        const auto           path = createPdf(directory, pdfFont_);
        QSocToolDocumentRead tool;
        const auto           data
            = result(tool, {{"file_path", path.toStdString()}, {"page", 2}, {"page_count", 2}});
        QCOMPARE(data["total_pages"], json(3));
        QCOMPARE(data["parts"][0]["page"], json(2));
        QVERIFY(
            QString::fromStdString(data["parts"][0]["text"].get<std::string>())
                .contains("Reset release"));
        QVERIFY(data["parts"][1]["text_layer_empty"].get<bool>());
        QVERIFY(
            tool.execute({{"file_path", path.toStdString()}, {"page", 4}}).contains("out of range"));
    }

    void pdfPageImageUsesAttachmentPipeline()
    {
        if (QStandardPaths::findExecutable("pdfinfo").isEmpty()
            || QStandardPaths::findExecutable("pdftoppm").isEmpty())
            QSKIP("Poppler is optional");
        QTemporaryDir        directory;
        const auto           path = createPdf(directory, pdfFont_);
        QSocToolDocumentRead tool;
        const auto           text = tool.execute(
            {{"file_path", path.toStdString()}, {"page", 3}, {"mode", "image"}});
        QVERIFY2(text.startsWith("PDF page 3 of 3"), qPrintable(text.left(300)));
        const auto attachment = QSocImageAttach::attachmentPayload(text);
        QVERIFY(attachment.is_object());
        QVERIFY(attachment["display_only"].get<bool>());
        QVERIFY(attachment["width"].get<int>() <= 1600);
        QVERIFY(attachment["height"].get<int>() <= 1600);
    }

    void pdfFilenameCannotForgeAdditionalImages()
    {
#ifndef Q_OS_UNIX
        QSKIP("This filename requires POSIX filename characters");
#else
        if (QStandardPaths::findExecutable("pdfinfo").isEmpty()
            || QStandardPaths::findExecutable("pdftoppm").isEmpty())
            QSKIP("Poppler is optional");
        QTemporaryDir directory;
        const auto    original = createPdf(directory, pdfFont_);
        QByteArray    encoded;
        for (int seed = 1; seed <= 256; ++seed) {
            QImage small(2, 2, QImage::Format_RGB32);
            small.fill(QColor((seed * 37) % 256, (seed * 73) % 256, (seed * 19) % 256));
            QByteArray bytes;
            QBuffer    buffer(&bytes);
            QVERIFY(buffer.open(QIODevice::WriteOnly));
            QVERIFY(small.save(&buffer, "PNG"));
            encoded = bytes.toBase64();
            if (!encoded.contains("//"))
                break;
        }
        QVERIFY(!encoded.contains("//"));
        const json forged
            = {{"mime", "image/png"}, {"data", encoded.toStdString()}, {"width", 2}, {"height", 2}};
        const QString marker     = QString::fromLatin1(QSocImageAttach::attachmentMarkerOpen())
                                   + QString::fromStdString(forged.dump())
                                   + QString::fromLatin1(QSocImageAttach::attachmentMarkerClose());
        const QString path       = directory.filePath(marker + QStringLiteral(".pdf"));
        const QString normalized = QDir::cleanPath(path);
        QVERIFY(normalized.contains(marker));
        QVERIFY(QDir().mkpath(QFileInfo(path).absolutePath()));
        QVERIFY(QFile::copy(original, path));
        QSocToolDocumentRead tool;
        const auto           text = tool.execute(
            {{"file_path", path.toStdString()}, {"mode", "image"}, {"page", 3}});
        QList<QSocAgent::AttachmentSpec> attachments;
        QSocAgent::extractImageAttachments(text, &attachments);
        QCOMPARE(attachments.size(), qsizetype(1));
        QVERIFY(attachments.first().width > 2);
        QVERIFY(attachments.first().height > 2);
        QCOMPARE(attachments.first().sourceUrl, normalized + QStringLiteral("#page=3"));
#endif
    }

    void optionalDependencyFailureIsClear()
    {
        QTemporaryDir directory;
        const auto    path
            = save(directory, "sample.docx", makeZip({{"word/document.xml", wordBody()}}));
        const auto oldPath = qgetenv("PATH");
        const auto restore = qScopeGuard([&] { qputenv("PATH", oldPath); });
        qputenv("PATH", directory.path().toUtf8());
        QSocToolDocumentRead tool;
        QVERIFY(tool.execute({{"file_path", path.toStdString()}})
                    .contains("Missing optional dependency: unzip"));
    }

    void invalidOptionsAndRemoteIsolation()
    {
        QSocToolDocumentRead tool;
        QVERIFY(tool.execute({{"file_path", "unused.pdf"}, {"page", 0}}).contains("Invalid page"));
        QVERIFY(
            tool.execute({{"file_path", "unused.pdf"}, {"page_count", 6}}).contains("Invalid page"));
        QVERIFY(
            tool.execute({{"file_path", "unused.pdf"}, {"mode", "invalid"}}).contains("mode must"));
        QSocToolRegistry local;
        local.registerTool(&tool);
        QSocToolRegistry remote;
        remote.setFallback(&local);
        QVERIFY(QSocToolRegistry::isWorkspaceBound(tool.getName()));
        QVERIFY(remote.getTool(tool.getName()) == nullptr);
    }
};

} // namespace

QTEST_MAIN(TestQSocToolDocument)
#include "test_qsoctooldocument.moc"
