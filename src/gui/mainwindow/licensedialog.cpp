// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "gui/mainwindow/licensedialog.h"

#include "common/qsoclicense.h"

#include <QDialogButtonBox>
#include <QFontDatabase>
#include <QSplitter>
#include <QVBoxLayout>

LicenseDialog::LicenseDialog(QWidget *parent)
    : QDialog(parent)
    , componentList(new QListWidget(this))
    , licenseText(new QPlainTextEdit(this))
{
    setWindowTitle(tr("Third-Party Licenses"));
    resize(900, 600);

    for (const QSocLicense::Component &component : QSocLicense::components()) {
        auto *item = new QListWidgetItem(component.name, componentList);
        item->setToolTip(component.license);
    }

    licenseText->setReadOnly(true);
    licenseText->setLineWrapMode(QPlainTextEdit::NoWrap);
    licenseText->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));

    auto *splitter = new QSplitter(this);
    splitter->addWidget(componentList);
    splitter->addWidget(licenseText);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({200, 700});

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(splitter);
    layout->addWidget(buttons);

    connect(
        componentList,
        &QListWidget::currentRowChanged,
        this,
        &LicenseDialog::handleCurrentRowChanged);
    componentList->setCurrentRow(0);
}

void LicenseDialog::handleCurrentRowChanged(int row)
{
    const QList<QSocLicense::Component> &components = QSocLicense::components();
    if (row < 0 || row >= components.size()) {
        licenseText->clear();
        return;
    }
    const QSocLicense::Component &component = components.at(row);
    licenseText->setPlainText(
        QStringLiteral("%1 (%2)\n%3\n\n%4")
            .arg(component.name, component.license, component.url, QSocLicense::text(component)));
}

#include "moc_licensedialog.cpp"
