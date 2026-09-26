// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/qsoctoolresultstore.h"
#include "qsoc_test.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>
#ifdef Q_OS_UNIX
#include <csignal>
#include <sys/resource.h>
#endif

namespace {
using Store = QSocToolResultStore;

QString recordPath(const Store &store, const QString &id)
{
    return QDir(store.directory()).filePath(id + ".qtr");
}

class Test : public QObject
{
    Q_OBJECT
private slots:
    void capturesActualText();
    void utf8Pages();
    void quotasPreserveReferences();
    void cancellationAndPublication();
    void inheritanceIsExplicitAndDurable();
    void inheritanceRollsBack();
    void rejectsForeignAndCorruptRecords();
    void discardOnlyOwnCandidate();
    void temporaryScopeFollowsSharedOwnership();
};

void Test::capturesActualText()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    Store store(root.filePath("artifacts"), "session");
    QVERIFY(store.isBound());
    QString source;
    for (int index = 0; index < 10000; ++index)
        source += QStringLiteral("row %1: α😀尾\n").arg(index);
    source += QStringLiteral("... (output truncated)");
    QString    error;
    const auto reference = store.publish(source, "failed", "truncated", &error);
    QVERIFY2(reference, qPrintable(error));
    QCOMPARE(reference->capturedBytes, source.toUtf8().size());
    const qint64 stored = store.storedBytes();
    QString      restored;
    qint64       next = 0;
    do {
        const auto page = store.read(reference->id, next, 1000000, &error);
        QVERIFY2(page, qPrintable(error));
        QCOMPARE(page->offset, next);
        QVERIFY(page->text.toUtf8().size() <= 32768);
        QVERIFY(page->nextOffset > next || page->eof);
        QCOMPARE(page->reference.completion, QString("failed"));
        QCOMPARE(page->reference.sourceCompleteness, QString("truncated"));
        restored += page->text;
        next = page->nextOffset;
        if (page->eof)
            break;
    } while (true);
    QCOMPARE(restored, source);
    QCOMPARE(next, reference->capturedBytes);
    QCOMPARE(QFileInfo(recordPath(store, reference->id)).size(), stored);
    QCOMPARE(QDir(store.directory()).entryList({"*.qtr"}, QDir::Files).size(), 1);
    const auto unknown = store.publish("complete captured reply", "ok");
    QVERIFY(unknown);
    QCOMPARE(unknown->sourceCompleteness, QString("unknown"));
}

void Test::utf8Pages()
{
    QTemporaryDir root;
    Store         store(root.filePath("artifacts"), "session");
    const QString source    = QString::fromUcs4(U"Aα😀尾Z\n");
    const auto    reference = store.publish(source, "ok");
    QVERIFY(reference);
    for (int limit = 4; limit <= 9; ++limit) {
        QByteArray result;
        qint64     offset = 0;
        while (offset < reference->capturedBytes) {
            const auto page = store.read(reference->id, offset, limit);
            QVERIFY(page);
            QCOMPARE(page->nextOffset - offset, page->text.toUtf8().size());
            QVERIFY(page->text.toUtf8().size() <= limit);
            result += page->text.toUtf8();
            offset = page->nextOffset;
        }
        QCOMPARE(result, source.toUtf8());
    }
    QVERIFY(!store.read(reference->id, 2, 8));
    QVERIFY(!store.read(reference->id, 1, 1));
    QVERIFY(!store.read(reference->id, -1, 8));
    QVERIFY(!store.read(reference->id, 1000, 8));
    QVERIFY(!store.read(reference->id, 0, 0));
    const auto eof = store.read(reference->id, reference->capturedBytes, 8);
    QVERIFY(eof && eof->eof && eof->text.isEmpty());
    const auto empty = store.publish("", "ok");
    QVERIFY(empty);
    const auto emptyPage = store.read(empty->id, 0, 4);
    QVERIFY(emptyPage && emptyPage->eof && emptyPage->nextOffset == 0);
}

void Test::quotasPreserveReferences()
{
    QTemporaryDir root;
    Store         initial(root.filePath("artifacts"), "session");
    const auto    first = initial.publish(QString(40, 'a'), "ok");
    QVERIFY(first);
    Store::Limits limits;
    limits.artifactBytes = 40;
    limits.sessionBytes  = initial.storedBytes() * 2 - 1;
    Store   limited(initial.directory(), "session", limits);
    QString error;
    QVERIFY(!limited.publish(QString(41, 'a'), "ok", "unknown", &error));
    QVERIFY(error.contains("artifact quota"));
    QVERIFY(!limited.publish(QString(40, 'a'), "ok", "unknown", &error));
    QVERIFY(error.contains("Session artifact quota"));
    QVERIFY(limited.read(first->id, 0, 100));
    limits.artifactBytes = 1;
    limits.sessionBytes  = 1;
    Store lowered(initial.directory(), "session", limits);
    QVERIFY(lowered.read(first->id, 0, 100));
    QCOMPARE(initial.storedBytes(), limited.storedBytes());
    Store::Limits utf8Limits;
    utf8Limits.artifactBytes = 5;
    Store utf8(root.filePath("utf8"), "unicode", utf8Limits);
    QVERIFY(!utf8.publish(QStringLiteral("ααα"), "ok"));
}

void Test::cancellationAndPublication()
{
    QTemporaryDir root;
    Store         store(root.filePath("artifacts"), "session");
    int           calls    = 0;
    bool          reserved = false;
    QString       error;
    const auto    cancelled = store.publish(QString(100000, 'x'), "ok", "unknown", &error, [&] {
        reserved = store.reservedBytes() > 100000;
        return ++calls < 3;
    });
    QVERIFY(!cancelled);
    QVERIFY(reserved);
    QCOMPARE(store.reservedBytes(), 0);
    QCOMPARE(store.storedBytes(), 0);
    QCOMPARE(
        QDir(store.directory()).entryList(QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot),
        QStringList{".scope"});
    Store      second(store.directory(), "session");
    bool       refusedConcurrent = false;
    const auto first             = store.publish("accepted", "ok", "unknown", &error, [&] {
        refusedConcurrent = !second.publish("second", "ok");
        return true;
    });
    QVERIFY(first);
    QVERIFY(refusedConcurrent);
    QCOMPARE(QDir(store.directory()).entryList({"*.qtr"}, QDir::Files).size(), 1);
#ifdef Q_OS_UNIX
    struct rlimit previous{};
    QVERIFY(getrlimit(RLIMIT_FSIZE, &previous) == 0);
    struct rlimit limited    = previous;
    limited.rlim_cur         = 1024;
    const auto signalHandler = std::signal(SIGXFSZ, SIG_IGN);
    const int  installed     = setrlimit(RLIMIT_FSIZE, &limited);
    const auto failed        = installed == 0
                                   ? store.publish(QString(100000, 'x'), "ok", "unknown", &error)
                                   : std::optional<Store::Reference>();
    const int  restored      = setrlimit(RLIMIT_FSIZE, &previous);
    std::signal(SIGXFSZ, signalHandler);
    QVERIFY(installed == 0 && restored == 0);
    QVERIFY(!failed);
    QVERIFY(error.contains("write failed"));
    QCOMPARE(store.reservedBytes(), 0);
    QVERIFY(store.read(first->id, 0, 100));
#endif
}

void Test::inheritanceIsExplicitAndDurable()
{
    QTemporaryDir root;
    Store         parent(root.filePath("parent"), "parent");
    Store         child(root.filePath("child"), "child");
    Store         sibling(root.filePath("sibling"), "sibling");
    const auto    a      = parent.publish("snapshot text", "ok");
    const auto    hidden = parent.publish("outside the inherited snapshot", "ok");
    QVERIFY(a && hidden);
    QVERIFY(child.inherit(parent, {*a}));
    const auto b = parent.publish("later parent text", "ok");
    const auto c = sibling.publish("sibling text", "ok");
    QVERIFY(b && c);
    const auto inherited = child.read(a->id, 0, 32768);
    QVERIFY(inherited);
    QCOMPARE(inherited->text, QString("snapshot text"));
    QCOMPARE(inherited->reference.origin, QString("parent"));
    QCOMPARE(inherited->reference.sha256, a->sha256);
    QVERIFY(!child.read(hidden->id, 0, 32768));
    QVERIFY(!child.read(b->id, 0, 32768));
    QVERIFY(!child.read(c->id, 0, 32768));
    QVERIFY(QDir(parent.directory()).removeRecursively());
    Store      resumed(child.directory(), "child");
    const auto afterDelete = resumed.read(a->id, 0, 32768);
    QVERIFY(afterDelete);
    QCOMPARE(afterDelete->text, inherited->text);
}

void Test::inheritanceRollsBack()
{
    QTemporaryDir root;
    Store         parent(root.filePath("parent"), "parent");
    const auto    a = parent.publish("first", "ok");
    const auto    b = parent.publish("second", "ok");
    QVERIFY(a && b);
    Store child(root.filePath("child"), "child");
    int   calls = 0;
    QVERIFY(!child.inherit(parent, {*a, *b}, nullptr, [&] { return ++calls <= 3; }));
    QCOMPARE(child.reservedBytes(), 0);
    QCOMPARE(child.storedBytes(), 0);
    QVERIFY(!child.read(a->id, 0, 10));
    QVERIFY(!child.read(b->id, 0, 10));
    QVERIFY(child.inherit(parent, {*a, *b}));
    Store::Limits limits;
    limits.sessionBytes = child.storedBytes() - 1;
    Store tooSmall(root.filePath("small"), "child", limits);
    QVERIFY(!tooSmall.inherit(parent, {*a, *b}));
    QCOMPARE(tooSmall.storedBytes(), 0);
    auto wrong      = *a;
    wrong.sha256[0] = wrong.sha256[0] == 'a' ? 'b' : 'a';
    QVERIFY(!tooSmall.inherit(parent, {wrong}));
    QVERIFY(!child.inherit(parent, {*a, wrong}));
    QVERIFY(!child.inherit(child, {*a}));
}

void Test::rejectsForeignAndCorruptRecords()
{
    QTemporaryDir root;
    Store         store(root.filePath("artifacts"), "session");
    const auto    reference = store.publish("capture body", "ok");
    QVERIFY(reference);
    Store foreign(store.directory(), "other-session");
    QVERIFY(!foreign.read(reference->id, 0, 100));
    QVERIFY(!store.read("../" + reference->id, 0, 100));
    QVERIFY(!store.read(recordPath(store, reference->id), 0, 100));
    QFile record(recordPath(store, reference->id));
    QVERIFY(record.open(QIODevice::ReadWrite));
    QVERIFY(record.seek(record.size() - 1));
    QCOMPARE(record.write("!"), 1);
    record.close();
    QVERIFY(!store.read(reference->id, 0, 100));
    QVERIFY(record.remove());
    QVERIFY(!store.read(reference->id, 0, 100));
    const QString originalDirectory = store.directory();
    QVERIFY(QDir(originalDirectory).removeRecursively());
    Store replacement(originalDirectory, "session");
    QVERIFY(replacement.isBound());
    QVERIFY(!store.isBound());
    QVERIFY(!store.publish("stale scope", "ok"));
#ifdef Q_OS_UNIX
    Store     &current = replacement;
    const auto valid   = current.publish("protected", "ok");
    QVERIFY(valid);
    const QString link = root.filePath("link");
    QVERIFY(QFile::link(current.directory(), link));
    Store linked(link, "session");
    QVERIFY(!linked.isBound());
#endif
}
void Test::temporaryScopeFollowsSharedOwnership()
{
    auto original = Store::temporary({});
    QVERIFY(original && original->isTemporary());
    const auto directory = original->directory();
    const auto record    = original->publish("snapshot text", "ok");
    QVERIFY(record);
    auto snapshot = original;
    original.reset();
    QVERIFY(snapshot->read(record->id, 0, 100));
    snapshot.reset();
    QVERIFY(!QFileInfo::exists(directory));
}

void Test::discardOnlyOwnCandidate()
{
    QTemporaryDir root;
    Store         parent(root.filePath("parent"), "parent");
    const auto    kept      = parent.publish("retained", "ok");
    const auto    candidate = parent.publish("candidate", "ok");
    QVERIFY(kept && candidate);
    Store child(root.filePath("child"), "child");
    QVERIFY(child.inherit(parent, {*kept}));
    QVERIFY(!child.discardPublished(*kept));
    auto wrong = *candidate;
    wrong.capturedBytes++;
    QVERIFY(!parent.discardPublished(wrong));
    QVERIFY(parent.read(candidate->id, 0, 100));
    const auto used = parent.storedBytes();
    QVERIFY(parent.discardPublished(*candidate));
    QVERIFY(parent.storedBytes() < used);
    QVERIFY(!parent.read(candidate->id, 0, 100));
    QVERIFY(parent.read(kept->id, 0, 100));
    QVERIFY(child.read(kept->id, 0, 100));
}

} // namespace

QSOC_TEST_MAIN(Test)
#include "test_qsoctoolresultstore.moc"
