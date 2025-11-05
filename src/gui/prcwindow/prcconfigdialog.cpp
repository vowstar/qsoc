// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2023-2025 Huang Rui <vowstar@gmail.com>

#include "prcconfigdialog.h"

#include <QDialogButtonBox>
#include <QGroupBox>
#include <QLabel>
#include <QVBoxLayout>

using namespace PrcLibrary;

PrcConfigDialog::PrcConfigDialog(PrcPrimitiveItem *item, QWidget *parent)
    : QDialog(parent)
    , item_(item)
    , formLayout_(nullptr)
{
    setWindowTitle(QString("Configure %1").arg(item->primitiveTypeName()));
    setMinimumWidth(400);

    auto *mainLayout = new QVBoxLayout(this);

    /* Basic information: name and type */
    auto *infoGroup  = new QGroupBox(tr("Basic Information"), this);
    auto *infoLayout = new QFormLayout(infoGroup);

    auto *nameEdit  = new QLineEdit(item->primitiveName(), this);
    fields_["name"] = nameEdit;
    infoLayout->addRow(tr("Name:"), nameEdit);

    auto *typeLabel = new QLabel(item->primitiveTypeName(), this);
    infoLayout->addRow(tr("Type:"), typeLabel);

    mainLayout->addWidget(infoGroup);

    /* Type-specific configuration fields */
    auto *configGroup = new QGroupBox(tr("Configuration"), this);
    formLayout_       = new QFormLayout(configGroup);

    createFieldsForType(item->primitiveType());

    mainLayout->addWidget(configGroup);

    auto *buttonBox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttonBox, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);
    mainLayout->addWidget(buttonBox);

    setLayout(mainLayout);
}

void PrcConfigDialog::createFieldsForType(PrimitiveType type)
{
    switch (type) {
    case ClockSource: {
        auto *freqEdit       = new QLineEdit(item_->config("frequency", "100MHz").toString(), this);
        fields_["frequency"] = freqEdit;
        formLayout_->addRow(tr("Frequency:"), freqEdit);

        auto *phaseEdit  = new QLineEdit(item_->config("phase", "0").toString(), this);
        fields_["phase"] = phaseEdit;
        formLayout_->addRow(tr("Phase (degrees):"), phaseEdit);

        break;
    }

    case ClockTarget: {
        auto *divEdit      = new QLineEdit(item_->config("divider", "1").toString(), this);
        fields_["divider"] = divEdit;
        formLayout_->addRow(tr("Divider:"), divEdit);

        auto *gateEdit = new QLineEdit(item_->config("enable_gate", "false").toString(), this);
        fields_["enable_gate"] = gateEdit;
        formLayout_->addRow(tr("Enable Gate:"), gateEdit);

        break;
    }

    case ResetSource: {
        auto *levelEdit = new QLineEdit(item_->config("active_level", "low").toString(), this);
        fields_["active_level"] = levelEdit;
        formLayout_->addRow(tr("Active Level:"), levelEdit);

        auto *durationEdit  = new QLineEdit(item_->config("duration", "10us").toString(), this);
        fields_["duration"] = durationEdit;
        formLayout_->addRow(tr("Duration:"), durationEdit);

        break;
    }

    case ResetTarget: {
        auto *syncEdit = new QLineEdit(item_->config("synchronous", "true").toString(), this);
        fields_["synchronous"] = syncEdit;
        formLayout_->addRow(tr("Synchronous:"), syncEdit);

        auto *stagesEdit  = new QLineEdit(item_->config("stages", "2").toString(), this);
        fields_["stages"] = stagesEdit;
        formLayout_->addRow(tr("Sync Stages:"), stagesEdit);

        break;
    }

    case PowerDomain: {
        auto *voltageEdit  = new QLineEdit(item_->config("voltage", "1.0V").toString(), this);
        fields_["voltage"] = voltageEdit;
        formLayout_->addRow(tr("Voltage:"), voltageEdit);

        auto *isoEdit        = new QLineEdit(item_->config("isolation", "true").toString(), this);
        fields_["isolation"] = isoEdit;
        formLayout_->addRow(tr("Isolation:"), isoEdit);

        auto *retEdit        = new QLineEdit(item_->config("retention", "false").toString(), this);
        fields_["retention"] = retEdit;
        formLayout_->addRow(tr("Retention:"), retEdit);

        break;
    }
    }
}

QMap<QString, QVariant> PrcConfigDialog::getConfiguration() const
{
    QMap<QString, QVariant> config;

    for (auto it = fields_.constBegin(); it != fields_.constEnd(); ++it) {
        config[it.key()] = it.value()->text();
    }

    return config;
}
