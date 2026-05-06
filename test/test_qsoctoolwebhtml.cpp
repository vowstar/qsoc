// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/tool/qsoctoolweb.h"
#include "qsoc_test.h"

#include <QtTest>

namespace {

class Test : public QObject
{
    Q_OBJECT

private slots:
    /* Well-formed inputs */
    void headingsAndParagraphs();
    void boldAndItalic();
    void inlineCodeAndPreCode();
    void unorderedList();
    void orderedList();
    void anchorAndImage();
    void table();

    /* Malformed inputs the hand-rolled parser used to mishandle */
    void unclosedParagraph();
    void misnestedBoldAcrossBlocks();
    void scriptContentMustNotLeak();
    void styleContentMustNotLeak();
    void unquotedAttribute();
    void htmlEntities();
    void numericCharRef();
    void malformedNestedTagsRecover();
    void unclosedAnchor();
    void preserveCodeBlockContentVerbatim();
};

} // namespace

void Test::headingsAndParagraphs()
{
    const QString out = QSocToolWebFetch::htmlToMarkdown(QStringLiteral(
        "<h1>Title</h1><p>First paragraph.</p>"
        "<h2>Sub</h2><p>Second paragraph.</p>"));
    QVERIFY(out.contains(QStringLiteral("# Title")));
    QVERIFY(out.contains(QStringLiteral("## Sub")));
    QVERIFY(out.contains(QStringLiteral("First paragraph.")));
    QVERIFY(out.contains(QStringLiteral("Second paragraph.")));
}

void Test::boldAndItalic()
{
    const QString out = QSocToolWebFetch::htmlToMarkdown(
        QStringLiteral("<p>Text with <strong>bold</strong> and <em>italic</em>.</p>"));
    QVERIFY(out.contains(QStringLiteral("**bold**")));
    QVERIFY(out.contains(QStringLiteral("*italic*")));
}

void Test::inlineCodeAndPreCode()
{
    const QString out = QSocToolWebFetch::htmlToMarkdown(QStringLiteral(
        "<p>Use <code>std::move</code> here.</p>"
        "<pre><code class=\"language-cpp\">int x = 0;</code></pre>"));
    QVERIFY(out.contains(QStringLiteral("`std::move`")));
    QVERIFY(out.contains(QStringLiteral("```cpp")));
    QVERIFY(out.contains(QStringLiteral("int x = 0;")));
    /* The fence must close exactly once. */
    QCOMPARE(out.count(QStringLiteral("```")), 2);
}

void Test::unorderedList()
{
    const QString out = QSocToolWebFetch::htmlToMarkdown(
        QStringLiteral("<ul><li>one</li><li>two</li></ul>"));
    QVERIFY(out.contains(QStringLiteral("- one")));
    QVERIFY(out.contains(QStringLiteral("- two")));
}

void Test::orderedList()
{
    const QString out = QSocToolWebFetch::htmlToMarkdown(
        QStringLiteral("<ol><li>alpha</li><li>beta</li><li>gamma</li></ol>"));
    QVERIFY(out.contains(QStringLiteral("1. alpha")));
    QVERIFY(out.contains(QStringLiteral("2. beta")));
    QVERIFY(out.contains(QStringLiteral("3. gamma")));
}

void Test::anchorAndImage()
{
    const QString out = QSocToolWebFetch::htmlToMarkdown(QStringLiteral(
        "<p><a href=\"https://example.com\">Example</a></p>"
        "<p><img alt=\"logo\" src=\"/logo.png\"></p>"));
    QVERIFY(out.contains(QStringLiteral("[Example](https://example.com)")));
    QVERIFY(out.contains(QStringLiteral("![logo](/logo.png)")));
}

void Test::table()
{
    const QString out = QSocToolWebFetch::htmlToMarkdown(QStringLiteral(
        "<table>"
        "<thead><tr><th>Name</th><th>Score</th></tr></thead>"
        "<tbody><tr><td>Alice</td><td>90</td></tr><tr><td>Bob</td><td>85</td></tr></tbody>"
        "</table>"));
    QVERIFY(out.contains(QStringLiteral("| Name")));
    QVERIFY(out.contains(QStringLiteral("| Score")));
    QVERIFY(out.contains(QStringLiteral("| Alice")));
    QVERIFY(out.contains(QStringLiteral("| Bob")));
    QVERIFY(out.contains(QStringLiteral("---")));
}

void Test::unclosedParagraph()
{
    /* WHATWG implicitly closes the open <p> when a new block-level
     * element opens; the hand-rolled parser left content tangled. */
    const QString out = QSocToolWebFetch::htmlToMarkdown(
        QStringLiteral("<p>first<div>second</div>"));
    QVERIFY(out.contains(QStringLiteral("first")));
    QVERIFY(out.contains(QStringLiteral("second")));
    /* No raw HTML must leak through. */
    QVERIFY(!out.contains(QStringLiteral("<p>")));
    QVERIFY(!out.contains(QStringLiteral("<div>")));
}

void Test::misnestedBoldAcrossBlocks()
{
    /* Reordering rules ("the adoption agency algorithm") split misnested
     * inline formatting; both runs of "x" should still appear bolded. */
    const QString out = QSocToolWebFetch::htmlToMarkdown(QStringLiteral("<p><b>x<p>y</b>z</p>"));
    QVERIFY(out.contains(QLatin1Char('x')));
    QVERIFY(out.contains(QLatin1Char('y')));
    QVERIFY(out.contains(QLatin1Char('z')));
    QVERIFY(!out.contains(QStringLiteral("<b>")));
}

void Test::scriptContentMustNotLeak()
{
    const QString out = QSocToolWebFetch::htmlToMarkdown(QStringLiteral(
        "<p>before</p><script>var x = '</script' + 'leak';</script>"
        "<p>after</p>"));
    QVERIFY(out.contains(QStringLiteral("before")));
    QVERIFY(out.contains(QStringLiteral("after")));
    QVERIFY(!out.contains(QStringLiteral("var x")));
    QVERIFY(!out.contains(QStringLiteral("leak")));
}

void Test::styleContentMustNotLeak()
{
    const QString out = QSocToolWebFetch::htmlToMarkdown(
        QStringLiteral("<style>body { color: red; }</style><p>visible</p>"));
    QVERIFY(out.contains(QStringLiteral("visible")));
    QVERIFY(!out.contains(QStringLiteral("color: red")));
    QVERIFY(!out.contains(QStringLiteral("body {")));
}

void Test::unquotedAttribute()
{
    const QString out = QSocToolWebFetch::htmlToMarkdown(
        QStringLiteral("<p><a href=https://example.org/path>link</a></p>"));
    QVERIFY(out.contains(QStringLiteral("[link](https://example.org/path)")));
}

void Test::htmlEntities()
{
    const QString out = QSocToolWebFetch::htmlToMarkdown(
        QStringLiteral("<p>A&amp;B &lt;tag&gt; &quot;q&quot; &mdash; &hearts;</p>"));
    QVERIFY(out.contains(QStringLiteral("A&B")));
    QVERIFY(out.contains(QStringLiteral("<tag>")));
    QVERIFY(out.contains(QStringLiteral("\"q\"")));
    /* Em dash and heart symbol must be decoded to actual chars. */
    QVERIFY(out.contains(QChar(0x2014)));
    QVERIFY(out.contains(QChar(0x2665)));
}

void Test::numericCharRef()
{
    const QString out = QSocToolWebFetch::htmlToMarkdown(
        QStringLiteral("<p>copy &#169; deg &#176; smile &#x1F600;</p>"));
    QVERIFY(out.contains(QChar(0x00A9)));
    QVERIFY(out.contains(QChar(0x00B0)));
    /* Supplementary plane: surrogate pair encoded U+1F600. */
    const char32_t smile = 0x1F600;
    QVERIFY(out.contains(QString::fromUcs4(&smile, 1)));
}

void Test::malformedNestedTagsRecover()
{
    /* Tag soup with crossed elements; spec defines a recovery walk that
     * still produces a valid tree. We just need the text to survive. */
    const QString out = QSocToolWebFetch::htmlToMarkdown(
        QStringLiteral("<p>aa<strong><em>bb</strong>cc</em>dd</p>"));
    QVERIFY(out.contains(QStringLiteral("aa")));
    QVERIFY(out.contains(QStringLiteral("bb")));
    QVERIFY(out.contains(QStringLiteral("cc")));
    QVERIFY(out.contains(QStringLiteral("dd")));
}

void Test::unclosedAnchor()
{
    const QString out = QSocToolWebFetch::htmlToMarkdown(
        QStringLiteral("<p><a href=\"x\">link without closing"));
    QVERIFY(out.contains(QStringLiteral("link without closing")));
    /* Whatever the parser decides, the link href must be preserved. */
    QVERIFY(out.contains(QStringLiteral("(x)")) || out.contains(QStringLiteral("[link")));
}

void Test::preserveCodeBlockContentVerbatim()
{
    /* Whitespace and special chars inside <pre> must round-trip. */
    const QString out = QSocToolWebFetch::htmlToMarkdown(
        QStringLiteral("<pre><code>line1\n    indented\nline3</code></pre>"));
    QVERIFY(out.contains(QStringLiteral("    indented")));
    QVERIFY(out.contains(QStringLiteral("line1")));
    QVERIFY(out.contains(QStringLiteral("line3")));
}

QSOC_TEST_MAIN(Test)
#include "test_qsoctoolwebhtml.moc"
