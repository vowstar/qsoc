// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "common/qsoclicense.h"
#include "gui/mainwindow/licensedialog.h"
#include "gui/mainwindow/mainwindow.h"

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QListWidget>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QTimer>
#include <QtTest>

namespace {

class Test : public QObject
{
    Q_OBJECT

private slots:
    void dialogShowsEveryComponent()
    {
        LicenseDialog dialog;
        auto         *list = dialog.findChild<QListWidget *>();
        auto         *text = dialog.findChild<QPlainTextEdit *>();
        QVERIFY(list);
        QVERIFY(text);
        QVERIFY(text->isReadOnly());
        QCOMPARE(list->count(), QSocLicense::components().size());
        for (int row = 0; row < list->count(); ++row) {
            list->setCurrentRow(row);
            const QSocLicense::Component &component = QSocLicense::components().at(row);
            QCOMPARE(list->item(row)->text(), component.name);
            QVERIFY(text->toPlainText().startsWith(component.name + " (" + component.license));
            QVERIFY(text->toPlainText().size() > component.url.size() + 64);
        }
    }

    void aboutOpensLicenseViewer()
    {
        MainWindow window;
        auto      *about = window.findChild<QAction *>("actionAbout");
        QVERIFY(about);

        bool viewerShown = false;
        QTimer::singleShot(0, this, [&viewerShown]() {
            auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
            QVERIFY(box);
            QAbstractButton *licenses = nullptr;
            for (QAbstractButton *button : box->buttons()) {
                if (button->text() == QStringLiteral("Third-Party Licenses")) {
                    licenses = button;
                }
            }
            QVERIFY(licenses);
            QTimer::singleShot(0, [&viewerShown]() {
                auto *dialog = qobject_cast<LicenseDialog *>(QApplication::activeModalWidget());
                QVERIFY(dialog);
                viewerShown = true;
                dialog->reject();
            });
            licenses->click();
        });
        about->trigger();
        QVERIFY(viewerShown);
    }
};

} // namespace

QTEST_MAIN(Test)
#include "test_qsocguilicensedialog.moc"
