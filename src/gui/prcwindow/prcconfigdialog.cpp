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
    , nameEdit_(nullptr)
    , freqSpin_(nullptr)
    , phaseSpin_(nullptr)
    , dividerSpin_(nullptr)
    , enableGateCheck_(nullptr)
    , activeLowCheck_(nullptr)
    , durationSpin_(nullptr)
    , synchronousCheck_(nullptr)
    , stagesSpin_(nullptr)
    , voltageSpin_(nullptr)
    , isolationCheck_(nullptr)
    , retentionCheck_(nullptr)
{
    setWindowTitle(QString("Configure %1").arg(item->primitiveTypeName()));
    setMinimumWidth(400);

    auto *mainLayout = new QVBoxLayout(this);

    /* Basic information: name and type */
    auto *infoGroup  = new QGroupBox(tr("Basic Information"), this);
    auto *infoLayout = new QFormLayout(infoGroup);

    nameEdit_ = new QLineEdit(item->primitiveName(), this);
    infoLayout->addRow(tr("Name:"), nameEdit_);

    auto *typeLabel = new QLabel(item->primitiveTypeName(), this);
    infoLayout->addRow(tr("Type:"), typeLabel);

    mainLayout->addWidget(infoGroup);

    /* Type-specific configuration fields */
    auto *configGroup = new QGroupBox(tr("Configuration"), this);
    formLayout_       = new QFormLayout(configGroup);

    switch (item->primitiveType()) {
    case ClockSource:
        createClockSourceFields();
        break;
    case ClockTarget:
        createClockTargetFields();
        break;
    case ResetSource:
        createResetSourceFields();
        break;
    case ResetTarget:
        createResetTargetFields();
        break;
    case PowerDomain:
        createPowerDomainFields();
        break;
    }

    mainLayout->addWidget(configGroup);

    auto *buttonBox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttonBox, &QDialogButtonBox::accepted, this, [this]() {
        applyConfiguration();
        accept();
    });
    connect(buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);
    mainLayout->addWidget(buttonBox);

    setLayout(mainLayout);
}

void PrcConfigDialog::createClockSourceFields()
{
    const auto &params = std::get<ClockSourceParams>(item_->params());

    freqSpin_ = new QDoubleSpinBox(this);
    freqSpin_->setRange(0.001, 10000.0);
    freqSpin_->setDecimals(3);
    freqSpin_->setSuffix(" MHz");
    freqSpin_->setValue(params.frequency_mhz);
    formLayout_->addRow(tr("Frequency:"), freqSpin_);

    phaseSpin_ = new QDoubleSpinBox(this);
    phaseSpin_->setRange(-360.0, 360.0);
    phaseSpin_->setDecimals(2);
    phaseSpin_->setSuffix(" deg");
    phaseSpin_->setValue(params.phase_deg);
    formLayout_->addRow(tr("Phase:"), phaseSpin_);
}

void PrcConfigDialog::createClockTargetFields()
{
    const auto &params = std::get<ClockTargetParams>(item_->params());

    dividerSpin_ = new QSpinBox(this);
    dividerSpin_->setRange(1, 1024);
    dividerSpin_->setValue(params.divider);
    formLayout_->addRow(tr("Divider:"), dividerSpin_);

    enableGateCheck_ = new QCheckBox(tr("Enable clock gating"), this);
    enableGateCheck_->setChecked(params.enable_gate);
    formLayout_->addRow(tr("Gating:"), enableGateCheck_);
}

void PrcConfigDialog::createResetSourceFields()
{
    const auto &params = std::get<ResetSourceParams>(item_->params());

    activeLowCheck_ = new QCheckBox(tr("Active low reset signal"), this);
    activeLowCheck_->setChecked(params.active_low);
    formLayout_->addRow(tr("Polarity:"), activeLowCheck_);

    durationSpin_ = new QDoubleSpinBox(this);
    durationSpin_->setRange(0.1, 10000.0);
    durationSpin_->setDecimals(2);
    durationSpin_->setSuffix(" us");
    durationSpin_->setValue(params.duration_us);
    formLayout_->addRow(tr("Duration:"), durationSpin_);
}

void PrcConfigDialog::createResetTargetFields()
{
    const auto &params = std::get<ResetTargetParams>(item_->params());

    synchronousCheck_ = new QCheckBox(tr("Synchronous reset"), this);
    synchronousCheck_->setChecked(params.synchronous);
    formLayout_->addRow(tr("Type:"), synchronousCheck_);

    stagesSpin_ = new QSpinBox(this);
    stagesSpin_->setRange(1, 10);
    stagesSpin_->setValue(params.stages);
    formLayout_->addRow(tr("Sync Stages:"), stagesSpin_);
}

void PrcConfigDialog::createPowerDomainFields()
{
    const auto &params = std::get<PowerDomainParams>(item_->params());

    voltageSpin_ = new QDoubleSpinBox(this);
    voltageSpin_->setRange(0.1, 5.0);
    voltageSpin_->setDecimals(2);
    voltageSpin_->setSuffix(" V");
    voltageSpin_->setValue(params.voltage);
    formLayout_->addRow(tr("Voltage:"), voltageSpin_);

    isolationCheck_ = new QCheckBox(tr("Enable isolation"), this);
    isolationCheck_->setChecked(params.isolation);
    formLayout_->addRow(tr("Isolation:"), isolationCheck_);

    retentionCheck_ = new QCheckBox(tr("Enable retention"), this);
    retentionCheck_->setChecked(params.retention);
    formLayout_->addRow(tr("Retention:"), retentionCheck_);
}

void PrcConfigDialog::applyConfiguration()
{
    /* Update primitive name */
    item_->setPrimitiveName(nameEdit_->text());

    /* Update type-specific parameters */
    switch (item_->primitiveType()) {
    case ClockSource: {
        ClockSourceParams params;
        params.frequency_mhz = freqSpin_->value();
        params.phase_deg     = phaseSpin_->value();
        item_->setParams(params);
        break;
    }
    case ClockTarget: {
        ClockTargetParams params;
        params.divider     = dividerSpin_->value();
        params.enable_gate = enableGateCheck_->isChecked();
        item_->setParams(params);
        break;
    }
    case ResetSource: {
        ResetSourceParams params;
        params.active_low  = activeLowCheck_->isChecked();
        params.duration_us = durationSpin_->value();
        item_->setParams(params);
        break;
    }
    case ResetTarget: {
        ResetTargetParams params;
        params.synchronous = synchronousCheck_->isChecked();
        params.stages      = stagesSpin_->value();
        item_->setParams(params);
        break;
    }
    case PowerDomain: {
        PowerDomainParams params;
        params.voltage   = voltageSpin_->value();
        params.isolation = isolationCheck_->isChecked();
        params.retention = retentionCheck_->isChecked();
        item_->setParams(params);
        break;
    }
    }
}
