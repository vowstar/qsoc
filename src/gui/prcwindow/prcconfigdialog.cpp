// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2023-2025 Huang Rui <vowstar@gmail.com>

#include "prcconfigdialog.h"

#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QMessageBox>
#include <QScrollArea>
#include <QVBoxLayout>

using namespace PrcLibrary;

PrcConfigDialog::PrcConfigDialog(PrcPrimitiveItem *item, PrcScene *scene, QWidget *parent)
    : QDialog(parent)
    , item_(item)
    , scene_(scene)
    , mainLayout_(nullptr)
    , nameEdit_(nullptr)
    , controllerCombo_(nullptr)
    , editControllerBtn_(nullptr)
    , inputFreqEdit_(nullptr)
    , targetFreqEdit_(nullptr)
    , targetMuxCheck_(nullptr)
    , targetSelectEdit_(nullptr)
    , targetResetEdit_(nullptr)
    , targetTestClockEdit_(nullptr)
    , targetIcgCheck_(nullptr)
    , targetIcgEnableEdit_(nullptr)
    , targetIcgPolarityCombo_(nullptr)
    , targetIcgClockOnResetCheck_(nullptr)
    , targetDivCheck_(nullptr)
    , targetDivDefaultSpin_(nullptr)
    , targetDivValueEdit_(nullptr)
    , targetDivWidthSpin_(nullptr)
    , targetDivResetEdit_(nullptr)
    , targetDivClockOnResetCheck_(nullptr)
    , targetInvCheck_(nullptr)
    , rstSrcActiveCombo_(nullptr)
    , rstTgtActiveCombo_(nullptr)
    , rstTgtAsyncCheck_(nullptr)
    , rstTgtAsyncClockEdit_(nullptr)
    , rstTgtAsyncStageSpin_(nullptr)
    , pwrDomVoltageSpin_(nullptr)
    , pwrDomPgoodEdit_(nullptr)
    , pwrDomWaitDepSpin_(nullptr)
    , pwrDomSettleOnSpin_(nullptr)
    , pwrDomSettleOffSpin_(nullptr)
{
    setWindowTitle(QString("Configure %1").arg(item->primitiveTypeName()));
    setMinimumWidth(450);

    mainLayout_ = new QVBoxLayout(this);

    /* Basic information: name */
    auto *infoGroup  = new QGroupBox(tr("Basic Information"), this);
    auto *infoLayout = new QFormLayout(infoGroup);

    nameEdit_ = new QLineEdit(item->primitiveName(), this);
    infoLayout->addRow(tr("Name:"), nameEdit_);

    auto *typeLabel = new QLabel(item->primitiveTypeName(), this);
    infoLayout->addRow(tr("Type:"), typeLabel);

    mainLayout_->addWidget(infoGroup);

    /* Create controller selection group */
    createControllerGroup();

    /* Create type-specific form */
    switch (item->primitiveType()) {
    case ClockInput:
        createClockInputForm();
        break;
    case ClockTarget:
        createClockTargetForm();
        break;
    case ResetSource:
        createResetSourceForm();
        break;
    case ResetTarget:
        createResetTargetForm();
        break;
    case PowerDomain:
        createPowerDomainForm();
        break;
    }

    /* Dialog buttons */
    auto *buttonBox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttonBox, &QDialogButtonBox::accepted, this, [this]() {
        applyConfiguration();
        accept();
    });
    connect(buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);
    mainLayout_->addWidget(buttonBox);

    setLayout(mainLayout_);
}

/* Clock Input Form */
void PrcConfigDialog::createClockInputForm()
{
    const auto &params = std::get<ClockInputParams>(item_->params());

    auto *group  = new QGroupBox(tr("Input Settings"), this);
    auto *layout = new QFormLayout(group);

    inputFreqEdit_ = new QLineEdit(params.freq, this);
    inputFreqEdit_->setPlaceholderText("25MHz");
    layout->addRow(tr("Frequency:"), inputFreqEdit_);

    mainLayout_->addWidget(group);
}

/* Clock Target Form */
void PrcConfigDialog::createClockTargetForm()
{
    const auto &params = std::get<ClockTargetParams>(item_->params());

    /* Basic target settings */
    auto *basicGroup  = new QGroupBox(tr("Target Settings"), this);
    auto *basicLayout = new QFormLayout(basicGroup);

    targetFreqEdit_ = new QLineEdit(params.freq, this);
    targetFreqEdit_->setPlaceholderText("400MHz");
    basicLayout->addRow(tr("Frequency:"), targetFreqEdit_);

    targetSelectEdit_ = new QLineEdit(params.select, this);
    targetSelectEdit_->setPlaceholderText("clk_sel");
    basicLayout->addRow(tr("Select Signal:"), targetSelectEdit_);

    targetResetEdit_ = new QLineEdit(params.reset, this);
    targetResetEdit_->setPlaceholderText("rst_n (for GF_MUX)");
    basicLayout->addRow(tr("Reset Signal:"), targetResetEdit_);

    targetTestClockEdit_ = new QLineEdit(params.test_clock, this);
    targetTestClockEdit_->setPlaceholderText("test_clk");
    basicLayout->addRow(tr("Test Clock:"), targetTestClockEdit_);

    mainLayout_->addWidget(basicGroup);

    /* MUX settings */
    auto *muxGroup  = new QGroupBox(tr("MUX (auto when multiple sources)"), this);
    auto *muxLayout = new QFormLayout(muxGroup);
    muxGroup->setCheckable(true);
    muxGroup->setChecked(params.mux.configured);
    targetMuxCheck_ = nullptr; /* Use group checkbox */
    connect(muxGroup, &QGroupBox::toggled, [this](bool checked) { Q_UNUSED(checked); });
    mainLayout_->addWidget(muxGroup);

    /* ICG settings */
    auto *icgGroup  = new QGroupBox(tr("ICG (Clock Gating)"), this);
    auto *icgLayout = new QFormLayout(icgGroup);
    icgGroup->setCheckable(true);
    icgGroup->setChecked(params.icg.configured);

    targetIcgEnableEdit_ = new QLineEdit(params.icg.enable, this);
    targetIcgEnableEdit_->setPlaceholderText("clk_en");
    icgLayout->addRow(tr("Enable Signal:"), targetIcgEnableEdit_);

    targetIcgPolarityCombo_ = new QComboBox(this);
    targetIcgPolarityCombo_->addItems({"high", "low"});
    targetIcgPolarityCombo_->setCurrentText(
        params.icg.polarity.isEmpty() ? "high" : params.icg.polarity);
    icgLayout->addRow(tr("Polarity:"), targetIcgPolarityCombo_);

    targetIcgClockOnResetCheck_ = new QCheckBox(tr("Enable clock during reset"), this);
    targetIcgClockOnResetCheck_->setChecked(params.icg.clock_on_reset);
    icgLayout->addRow(tr("Clock on Reset:"), targetIcgClockOnResetCheck_);

    targetIcgCheck_ = nullptr; /* Use group checkbox */
    connect(icgGroup, &QGroupBox::toggled, [this, icgLayout](bool checked) {
        for (int i = 0; i < icgLayout->count(); ++i) {
            if (auto *widget = icgLayout->itemAt(i)->widget()) {
                widget->setEnabled(checked);
            }
        }
    });
    icgGroup->toggled(params.icg.configured);

    mainLayout_->addWidget(icgGroup);

    /* DIV settings */
    auto *divGroup  = new QGroupBox(tr("DIV (Clock Divider)"), this);
    auto *divLayout = new QFormLayout(divGroup);
    divGroup->setCheckable(true);
    divGroup->setChecked(params.div.configured);

    targetDivDefaultSpin_ = new QSpinBox(this);
    targetDivDefaultSpin_->setRange(1, 65535);
    targetDivDefaultSpin_->setValue(params.div.default_value);
    divLayout->addRow(tr("Default Value:"), targetDivDefaultSpin_);

    targetDivValueEdit_ = new QLineEdit(params.div.value, this);
    targetDivValueEdit_->setPlaceholderText("clk_div (runtime control)");
    divLayout->addRow(tr("Value Signal:"), targetDivValueEdit_);

    targetDivWidthSpin_ = new QSpinBox(this);
    targetDivWidthSpin_->setRange(0, 32);
    targetDivWidthSpin_->setValue(params.div.width);
    targetDivWidthSpin_->setSpecialValueText("auto");
    divLayout->addRow(tr("Bit Width:"), targetDivWidthSpin_);

    targetDivResetEdit_ = new QLineEdit(params.div.reset, this);
    targetDivResetEdit_->setPlaceholderText("rst_n");
    divLayout->addRow(tr("Reset Signal:"), targetDivResetEdit_);

    targetDivClockOnResetCheck_ = new QCheckBox(tr("Enable clock during reset"), this);
    targetDivClockOnResetCheck_->setChecked(params.div.clock_on_reset);
    divLayout->addRow(tr("Clock on Reset:"), targetDivClockOnResetCheck_);

    targetDivCheck_ = nullptr; /* Use group checkbox */
    connect(divGroup, &QGroupBox::toggled, [this, divLayout](bool checked) {
        for (int i = 0; i < divLayout->count(); ++i) {
            if (auto *widget = divLayout->itemAt(i)->widget()) {
                widget->setEnabled(checked);
            }
        }
    });
    divGroup->toggled(params.div.configured);

    mainLayout_->addWidget(divGroup);

    /* INV settings */
    auto *invGroup  = new QGroupBox(tr("INV (Clock Inverter)"), this);
    auto *invLayout = new QFormLayout(invGroup);
    invGroup->setCheckable(true);
    invGroup->setChecked(params.inv.configured);

    targetInvCheck_ = nullptr; /* Use group checkbox */

    mainLayout_->addWidget(invGroup);

    /* Store group pointers for apply */
    targetMuxCheck_ = reinterpret_cast<QCheckBox *>(muxGroup);
    targetIcgCheck_ = reinterpret_cast<QCheckBox *>(icgGroup);
    targetDivCheck_ = reinterpret_cast<QCheckBox *>(divGroup);
    targetInvCheck_ = reinterpret_cast<QCheckBox *>(invGroup);
}

/* Reset Source Form */
void PrcConfigDialog::createResetSourceForm()
{
    const auto &params = std::get<ResetSourceParams>(item_->params());

    auto *group  = new QGroupBox(tr("Source Settings"), this);
    auto *layout = new QFormLayout(group);

    rstSrcActiveCombo_ = new QComboBox(this);
    rstSrcActiveCombo_->addItems({"low", "high"});
    rstSrcActiveCombo_->setCurrentText(params.active.isEmpty() ? "low" : params.active);
    layout->addRow(tr("Active Level:"), rstSrcActiveCombo_);

    mainLayout_->addWidget(group);
}

/* Reset Target Form */
void PrcConfigDialog::createResetTargetForm()
{
    const auto &params = std::get<ResetTargetParams>(item_->params());

    auto *basicGroup  = new QGroupBox(tr("Target Settings"), this);
    auto *basicLayout = new QFormLayout(basicGroup);

    rstTgtActiveCombo_ = new QComboBox(this);
    rstTgtActiveCombo_->addItems({"low", "high"});
    rstTgtActiveCombo_->setCurrentText(params.active.isEmpty() ? "low" : params.active);
    basicLayout->addRow(tr("Active Level:"), rstTgtActiveCombo_);

    mainLayout_->addWidget(basicGroup);

    /* Async synchronizer settings */
    auto *asyncGroup  = new QGroupBox(tr("Async Synchronizer (qsoc_rst_sync)"), this);
    auto *asyncLayout = new QFormLayout(asyncGroup);
    asyncGroup->setCheckable(true);
    asyncGroup->setChecked(params.sync.async_configured);

    rstTgtAsyncClockEdit_ = new QLineEdit(params.sync.async_clock, this);
    rstTgtAsyncClockEdit_->setPlaceholderText("clk_sys");
    asyncLayout->addRow(tr("Clock:"), rstTgtAsyncClockEdit_);

    rstTgtAsyncStageSpin_ = new QSpinBox(this);
    rstTgtAsyncStageSpin_->setRange(2, 8);
    rstTgtAsyncStageSpin_->setValue(params.sync.async_stage);
    asyncLayout->addRow(tr("Stages:"), rstTgtAsyncStageSpin_);

    rstTgtAsyncCheck_ = nullptr; /* Use group checkbox */
    connect(asyncGroup, &QGroupBox::toggled, [this, asyncLayout](bool checked) {
        for (int i = 0; i < asyncLayout->count(); ++i) {
            if (auto *widget = asyncLayout->itemAt(i)->widget()) {
                widget->setEnabled(checked);
            }
        }
    });
    asyncGroup->toggled(params.sync.async_configured);

    mainLayout_->addWidget(asyncGroup);

    rstTgtAsyncCheck_ = reinterpret_cast<QCheckBox *>(asyncGroup);
}

/* Power Domain Form */
void PrcConfigDialog::createPowerDomainForm()
{
    const auto &params = std::get<PowerDomainParams>(item_->params());

    auto *group  = new QGroupBox(tr("Domain Settings"), this);
    auto *layout = new QFormLayout(group);

    pwrDomVoltageSpin_ = new QSpinBox(this);
    pwrDomVoltageSpin_->setRange(100, 5000);
    pwrDomVoltageSpin_->setSuffix(" mV");
    pwrDomVoltageSpin_->setValue(params.v_mv);
    layout->addRow(tr("Voltage:"), pwrDomVoltageSpin_);

    pwrDomPgoodEdit_ = new QLineEdit(params.pgood, this);
    pwrDomPgoodEdit_->setPlaceholderText("pgood_xxx");
    layout->addRow(tr("Power Good:"), pwrDomPgoodEdit_);

    pwrDomWaitDepSpin_ = new QSpinBox(this);
    pwrDomWaitDepSpin_->setRange(0, 65535);
    pwrDomWaitDepSpin_->setValue(params.wait_dep);
    layout->addRow(tr("Wait Dep Cycles:"), pwrDomWaitDepSpin_);

    pwrDomSettleOnSpin_ = new QSpinBox(this);
    pwrDomSettleOnSpin_->setRange(0, 65535);
    pwrDomSettleOnSpin_->setValue(params.settle_on);
    layout->addRow(tr("Settle On Cycles:"), pwrDomSettleOnSpin_);

    pwrDomSettleOffSpin_ = new QSpinBox(this);
    pwrDomSettleOffSpin_->setRange(0, 65535);
    pwrDomSettleOffSpin_->setValue(params.settle_off);
    layout->addRow(tr("Settle Off Cycles:"), pwrDomSettleOffSpin_);

    mainLayout_->addWidget(group);
}

/* Controller Selection Group */
void PrcConfigDialog::createControllerGroup()
{
    auto *group  = new QGroupBox(tr("Controller Assignment"), this);
    auto *layout = new QHBoxLayout(group);

    controllerCombo_ = new QComboBox(this);
    controllerCombo_->setMinimumWidth(200);
    populateControllerCombo();

    editControllerBtn_ = new QPushButton(tr("Edit..."), this);
    editControllerBtn_->setToolTip(tr("Edit controller settings"));

    layout->addWidget(controllerCombo_, 1);
    layout->addWidget(editControllerBtn_);

    connect(
        controllerCombo_,
        QOverload<int>::of(&QComboBox::currentIndexChanged),
        this,
        &PrcConfigDialog::onControllerChanged);
    connect(
        editControllerBtn_, &QPushButton::clicked, this, &PrcConfigDialog::onEditControllerClicked);

    mainLayout_->addWidget(group);
}

void PrcConfigDialog::populateControllerCombo()
{
    if (!controllerCombo_) {
        return;
    }

    controllerCombo_->clear();

    /* Get current controller name from params */
    QString     currentController;
    const auto &params = item_->params();

    if (std::holds_alternative<ClockInputParams>(params)) {
        currentController = std::get<ClockInputParams>(params).controller;
    } else if (std::holds_alternative<ClockTargetParams>(params)) {
        currentController = std::get<ClockTargetParams>(params).controller;
    } else if (std::holds_alternative<ResetSourceParams>(params)) {
        currentController = std::get<ResetSourceParams>(params).controller;
    } else if (std::holds_alternative<ResetTargetParams>(params)) {
        currentController = std::get<ResetTargetParams>(params).controller;
    } else if (std::holds_alternative<PowerDomainParams>(params)) {
        currentController = std::get<PowerDomainParams>(params).controller;
    }

    /* Add existing controllers based on primitive type */
    QStringList controllerNames;
    QString     defaultName;

    switch (item_->primitiveType()) {
    case ClockInput:
    case ClockTarget:
        if (scene_) {
            controllerNames = scene_->clockControllerNames();
        }
        defaultName = "clock_ctrl";
        break;
    case ResetSource:
    case ResetTarget:
        if (scene_) {
            controllerNames = scene_->resetControllerNames();
        }
        defaultName = "reset_ctrl";
        break;
    case PowerDomain:
        if (scene_) {
            controllerNames = scene_->powerControllerNames();
        }
        defaultName = "power_ctrl";
        break;
    }

    /* Add controllers to combo */
    for (const QString &name : controllerNames) {
        controllerCombo_->addItem(name, name);
    }

    /* Add separator and "New Controller..." option */
    if (!controllerNames.isEmpty()) {
        controllerCombo_->insertSeparator(controllerCombo_->count());
    }
    controllerCombo_->addItem(tr("New Controller..."), QVariant("__new__"));

    /* Select current controller or add if not in list */
    if (!currentController.isEmpty()) {
        int idx = controllerCombo_->findData(currentController);
        if (idx >= 0) {
            controllerCombo_->setCurrentIndex(idx);
        } else {
            /* Add current controller to list if not found */
            controllerCombo_->insertItem(0, currentController, currentController);
            controllerCombo_->setCurrentIndex(0);
        }
    } else if (!controllerNames.isEmpty()) {
        controllerCombo_->setCurrentIndex(0);
    } else {
        /* No controllers exist - add default */
        controllerCombo_->insertItem(0, defaultName, defaultName);
        controllerCombo_->setCurrentIndex(0);
    }
}

void PrcConfigDialog::onControllerChanged(int index)
{
    if (!controllerCombo_ || index < 0) {
        return;
    }

    QString value = controllerCombo_->currentData().toString();

    if (value == "__new__") {
        /* Create new controller - show input dialog */
        QString defaultName;
        switch (item_->primitiveType()) {
        case ClockInput:
        case ClockTarget:
            defaultName = "clock_ctrl_new";
            break;
        case ResetSource:
        case ResetTarget:
            defaultName = "reset_ctrl_new";
            break;
        case PowerDomain:
            defaultName = "power_ctrl_new";
            break;
        }

        bool    ok;
        QString name = QInputDialog::getText(
            this, tr("New Controller"), tr("Controller name:"), QLineEdit::Normal, defaultName, &ok);

        if (ok && !name.isEmpty()) {
            /* Create controller in scene */
            if (scene_) {
                switch (item_->primitiveType()) {
                case ClockInput:
                case ClockTarget: {
                    ClockControllerDef def;
                    def.name = name;
                    scene_->setClockController(name, def);
                    break;
                }
                case ResetSource:
                case ResetTarget: {
                    ResetControllerDef def;
                    def.name = name;
                    scene_->setResetController(name, def);
                    break;
                }
                case PowerDomain: {
                    PowerControllerDef def;
                    def.name = name;
                    scene_->setPowerController(name, def);
                    break;
                }
                }
            }

            /* Repopulate and select new controller */
            populateControllerCombo();
            int idx = controllerCombo_->findData(name);
            if (idx >= 0) {
                controllerCombo_->setCurrentIndex(idx);
            }
        } else {
            /* User cancelled - reset to first item */
            if (controllerCombo_->count() > 1) {
                controllerCombo_->setCurrentIndex(0);
            }
        }
    }
}

void PrcConfigDialog::onEditControllerClicked()
{
    if (!controllerCombo_ || !scene_) {
        return;
    }

    QString controllerName = controllerCombo_->currentData().toString();
    if (controllerName.isEmpty() || controllerName == "__new__") {
        return;
    }

    /* Show controller edit dialog - for now just show a message */
    /* TODO: Implement PrcControllerDialog */
    QMessageBox::information(
        this,
        tr("Edit Controller"),
        tr("Controller: %1\n\nController settings dialog not implemented yet.").arg(controllerName));
}

/* Apply Configuration */
void PrcConfigDialog::applyConfiguration()
{
    /* Update primitive name */
    item_->setPrimitiveName(nameEdit_->text());

    /* Get selected controller name */
    QString selectedController;
    if (controllerCombo_) {
        QString data = controllerCombo_->currentData().toString();
        if (data != "__new__") {
            selectedController = data;
        }
    }

    /* Update type-specific parameters */
    switch (item_->primitiveType()) {
    case ClockInput: {
        ClockInputParams params;
        params.name       = nameEdit_->text();
        params.freq       = inputFreqEdit_->text();
        params.controller = selectedController;
        item_->setParams(params);
        break;
    }
    case ClockTarget: {
        ClockTargetParams params;
        params.name       = nameEdit_->text();
        params.freq       = targetFreqEdit_->text();
        params.controller = selectedController;
        params.select     = targetSelectEdit_->text();
        params.reset      = targetResetEdit_->text();
        params.test_clock = targetTestClockEdit_->text();

        /* MUX */
        auto *muxGroup        = reinterpret_cast<QGroupBox *>(targetMuxCheck_);
        params.mux.configured = muxGroup ? muxGroup->isChecked() : false;

        /* ICG */
        auto *icgGroup        = reinterpret_cast<QGroupBox *>(targetIcgCheck_);
        params.icg.configured = icgGroup ? icgGroup->isChecked() : false;
        if (params.icg.configured) {
            params.icg.enable         = targetIcgEnableEdit_->text();
            params.icg.polarity       = targetIcgPolarityCombo_->currentText();
            params.icg.clock_on_reset = targetIcgClockOnResetCheck_->isChecked();
        }

        /* DIV */
        auto *divGroup        = reinterpret_cast<QGroupBox *>(targetDivCheck_);
        params.div.configured = divGroup ? divGroup->isChecked() : false;
        if (params.div.configured) {
            params.div.default_value  = targetDivDefaultSpin_->value();
            params.div.value          = targetDivValueEdit_->text();
            params.div.width          = targetDivWidthSpin_->value();
            params.div.reset          = targetDivResetEdit_->text();
            params.div.clock_on_reset = targetDivClockOnResetCheck_->isChecked();
        }

        /* INV */
        auto *invGroup        = reinterpret_cast<QGroupBox *>(targetInvCheck_);
        params.inv.configured = invGroup ? invGroup->isChecked() : false;

        item_->setParams(params);
        break;
    }
    case ResetSource: {
        ResetSourceParams params;
        params.name       = nameEdit_->text();
        params.active     = rstSrcActiveCombo_->currentText();
        params.controller = selectedController;
        item_->setParams(params);
        break;
    }
    case ResetTarget: {
        ResetTargetParams params;
        params.name       = nameEdit_->text();
        params.active     = rstTgtActiveCombo_->currentText();
        params.controller = selectedController;

        auto *asyncGroup             = reinterpret_cast<QGroupBox *>(rstTgtAsyncCheck_);
        params.sync.async_configured = asyncGroup ? asyncGroup->isChecked() : false;
        if (params.sync.async_configured) {
            params.sync.async_clock = rstTgtAsyncClockEdit_->text();
            params.sync.async_stage = rstTgtAsyncStageSpin_->value();
        }

        item_->setParams(params);
        break;
    }
    case PowerDomain: {
        PowerDomainParams params;
        params.name       = nameEdit_->text();
        params.controller = selectedController;
        params.v_mv       = pwrDomVoltageSpin_->value();
        params.pgood      = pwrDomPgoodEdit_->text();
        params.wait_dep   = pwrDomWaitDepSpin_->value();
        params.settle_on  = pwrDomSettleOnSpin_->value();
        params.settle_off = pwrDomSettleOffSpin_->value();
        /* TODO: dependencies and follow entries via separate UI */
        item_->setParams(params);
        break;
    }
    }
}

/* Link Config Dialog */
PrcLinkConfigDialog::PrcLinkConfigDialog(
    const QString         &sourceName,
    const QString         &targetName,
    const ClockLinkParams &linkParams,
    QWidget               *parent)
    : QDialog(parent)
    , sourceName_(sourceName)
    , targetName_(targetName)
    , linkParams_(linkParams)
    , mainLayout_(nullptr)
    , icgGroup_(nullptr)
    , icgEnableEdit_(nullptr)
    , icgPolarityCombo_(nullptr)
    , icgTestEnableEdit_(nullptr)
    , icgResetEdit_(nullptr)
    , icgClockOnResetCheck_(nullptr)
    , icgStaGuideGroup_(nullptr)
    , icgStaCellEdit_(nullptr)
    , icgStaInEdit_(nullptr)
    , icgStaOutEdit_(nullptr)
    , icgStaInstanceEdit_(nullptr)
    , divGroup_(nullptr)
    , divDefaultSpin_(nullptr)
    , divValueEdit_(nullptr)
    , divWidthSpin_(nullptr)
    , divResetEdit_(nullptr)
    , divClockOnResetCheck_(nullptr)
    , divStaGuideGroup_(nullptr)
    , divStaCellEdit_(nullptr)
    , divStaInEdit_(nullptr)
    , divStaOutEdit_(nullptr)
    , divStaInstanceEdit_(nullptr)
    , invGroup_(nullptr)
    , invStaGuideGroup_(nullptr)
    , invStaCellEdit_(nullptr)
    , invStaInEdit_(nullptr)
    , invStaOutEdit_(nullptr)
    , invStaInstanceEdit_(nullptr)
    , linkStaGuideGroup_(nullptr)
    , linkStaCellEdit_(nullptr)
    , linkStaInEdit_(nullptr)
    , linkStaOutEdit_(nullptr)
    , linkStaInstanceEdit_(nullptr)
{
    setWindowTitle(QString("Configure Link: %1 -> %2").arg(sourceName).arg(targetName));
    setMinimumWidth(500);

    mainLayout_ = new QVBoxLayout(this);

    /* Header info */
    auto *infoGroup  = new QGroupBox(tr("Link Information"), this);
    auto *infoLayout = new QFormLayout(infoGroup);

    auto *sourceLabel = new QLabel(sourceName_, this);
    infoLayout->addRow(tr("Source:"), sourceLabel);

    auto *targetLabel = new QLabel(targetName_, this);
    infoLayout->addRow(tr("Target:"), targetLabel);

    mainLayout_->addWidget(infoGroup);

    /* Create form widgets */
    createForm();

    /* Dialog buttons */
    auto *buttonBox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttonBox, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);
    mainLayout_->addWidget(buttonBox);

    setLayout(mainLayout_);
}

void PrcLinkConfigDialog::createForm()
{
    /* Scroll area for long forms */
    auto *scrollArea = new QScrollArea(this);
    scrollArea->setWidgetResizable(true);
    scrollArea->setFrameShape(QFrame::NoFrame);

    auto *scrollWidget = new QWidget(scrollArea);
    auto *scrollLayout = new QVBoxLayout(scrollWidget);

    /* ICG Group */
    icgGroup_ = new QGroupBox(tr("ICG (Link-level Clock Gating)"), scrollWidget);
    icgGroup_->setCheckable(true);
    icgGroup_->setChecked(linkParams_.icg.configured);

    auto *icgLayout = new QFormLayout(icgGroup_);

    icgEnableEdit_ = new QLineEdit(linkParams_.icg.enable, icgGroup_);
    icgEnableEdit_->setPlaceholderText("clk_en");
    icgLayout->addRow(tr("Enable Signal:"), icgEnableEdit_);

    icgPolarityCombo_ = new QComboBox(icgGroup_);
    icgPolarityCombo_->addItems({"high", "low"});
    icgPolarityCombo_->setCurrentText(
        linkParams_.icg.polarity.isEmpty() ? "high" : linkParams_.icg.polarity);
    icgLayout->addRow(tr("Polarity:"), icgPolarityCombo_);

    icgTestEnableEdit_ = new QLineEdit(linkParams_.icg.test_enable, icgGroup_);
    icgTestEnableEdit_->setPlaceholderText("test_en");
    icgLayout->addRow(tr("Test Enable:"), icgTestEnableEdit_);

    icgResetEdit_ = new QLineEdit(linkParams_.icg.reset, icgGroup_);
    icgResetEdit_->setPlaceholderText("rst_n");
    icgLayout->addRow(tr("Reset Signal:"), icgResetEdit_);

    icgClockOnResetCheck_ = new QCheckBox(tr("Enable clock during reset"), icgGroup_);
    icgClockOnResetCheck_->setChecked(linkParams_.icg.clock_on_reset);
    icgLayout->addRow(icgClockOnResetCheck_);

    /* ICG STA Guide */
    icgStaGuideGroup_ = new QGroupBox(tr("ICG STA Guide"), icgGroup_);
    icgStaGuideGroup_->setCheckable(true);
    icgStaGuideGroup_->setChecked(linkParams_.icg.sta_guide.configured);

    auto *icgStaLayout = new QFormLayout(icgStaGuideGroup_);
    icgStaCellEdit_    = new QLineEdit(linkParams_.icg.sta_guide.cell, icgStaGuideGroup_);
    icgStaCellEdit_->setPlaceholderText("BUF_X2");
    icgStaLayout->addRow(tr("Cell:"), icgStaCellEdit_);

    icgStaInEdit_ = new QLineEdit(linkParams_.icg.sta_guide.in, icgStaGuideGroup_);
    icgStaInEdit_->setPlaceholderText("I");
    icgStaLayout->addRow(tr("In Pin:"), icgStaInEdit_);

    icgStaOutEdit_ = new QLineEdit(linkParams_.icg.sta_guide.out, icgStaGuideGroup_);
    icgStaOutEdit_->setPlaceholderText("Z");
    icgStaLayout->addRow(tr("Out Pin:"), icgStaOutEdit_);

    icgStaInstanceEdit_ = new QLineEdit(linkParams_.icg.sta_guide.instance, icgStaGuideGroup_);
    icgStaInstanceEdit_->setPlaceholderText("u_icg_sta");
    icgStaLayout->addRow(tr("Instance:"), icgStaInstanceEdit_);

    icgLayout->addRow(icgStaGuideGroup_);

    scrollLayout->addWidget(icgGroup_);

    /* DIV Group */
    divGroup_ = new QGroupBox(tr("DIV (Link-level Clock Divider)"), scrollWidget);
    divGroup_->setCheckable(true);
    divGroup_->setChecked(linkParams_.div.configured);

    auto *divLayout = new QFormLayout(divGroup_);

    divDefaultSpin_ = new QSpinBox(divGroup_);
    divDefaultSpin_->setRange(1, 65535);
    divDefaultSpin_->setValue(linkParams_.div.default_value);
    divLayout->addRow(tr("Default Value:"), divDefaultSpin_);

    divValueEdit_ = new QLineEdit(linkParams_.div.value, divGroup_);
    divValueEdit_->setPlaceholderText("div_ratio (runtime control)");
    divLayout->addRow(tr("Value Signal:"), divValueEdit_);

    divWidthSpin_ = new QSpinBox(divGroup_);
    divWidthSpin_->setRange(0, 32);
    divWidthSpin_->setValue(linkParams_.div.width);
    divWidthSpin_->setSpecialValueText("auto");
    divLayout->addRow(tr("Bit Width:"), divWidthSpin_);

    divResetEdit_ = new QLineEdit(linkParams_.div.reset, divGroup_);
    divResetEdit_->setPlaceholderText("rst_n");
    divLayout->addRow(tr("Reset Signal:"), divResetEdit_);

    divClockOnResetCheck_ = new QCheckBox(tr("Enable clock during reset"), divGroup_);
    divClockOnResetCheck_->setChecked(linkParams_.div.clock_on_reset);
    divLayout->addRow(divClockOnResetCheck_);

    /* DIV STA Guide */
    divStaGuideGroup_ = new QGroupBox(tr("DIV STA Guide"), divGroup_);
    divStaGuideGroup_->setCheckable(true);
    divStaGuideGroup_->setChecked(linkParams_.div.sta_guide.configured);

    auto *divStaLayout = new QFormLayout(divStaGuideGroup_);
    divStaCellEdit_    = new QLineEdit(linkParams_.div.sta_guide.cell, divStaGuideGroup_);
    divStaCellEdit_->setPlaceholderText("BUF_X2");
    divStaLayout->addRow(tr("Cell:"), divStaCellEdit_);

    divStaInEdit_ = new QLineEdit(linkParams_.div.sta_guide.in, divStaGuideGroup_);
    divStaInEdit_->setPlaceholderText("I");
    divStaLayout->addRow(tr("In Pin:"), divStaInEdit_);

    divStaOutEdit_ = new QLineEdit(linkParams_.div.sta_guide.out, divStaGuideGroup_);
    divStaOutEdit_->setPlaceholderText("Z");
    divStaLayout->addRow(tr("Out Pin:"), divStaOutEdit_);

    divStaInstanceEdit_ = new QLineEdit(linkParams_.div.sta_guide.instance, divStaGuideGroup_);
    divStaInstanceEdit_->setPlaceholderText("u_div_sta");
    divStaLayout->addRow(tr("Instance:"), divStaInstanceEdit_);

    divLayout->addRow(divStaGuideGroup_);

    scrollLayout->addWidget(divGroup_);

    /* INV Group */
    invGroup_ = new QGroupBox(tr("INV (Link-level Clock Inverter)"), scrollWidget);
    invGroup_->setCheckable(true);
    invGroup_->setChecked(linkParams_.inv.configured);

    auto *invLayout = new QFormLayout(invGroup_);

    /* INV STA Guide */
    invStaGuideGroup_ = new QGroupBox(tr("INV STA Guide"), invGroup_);
    invStaGuideGroup_->setCheckable(true);
    invStaGuideGroup_->setChecked(linkParams_.inv.sta_guide.configured);

    auto *invStaLayout = new QFormLayout(invStaGuideGroup_);
    invStaCellEdit_    = new QLineEdit(linkParams_.inv.sta_guide.cell, invStaGuideGroup_);
    invStaCellEdit_->setPlaceholderText("BUF_X2");
    invStaLayout->addRow(tr("Cell:"), invStaCellEdit_);

    invStaInEdit_ = new QLineEdit(linkParams_.inv.sta_guide.in, invStaGuideGroup_);
    invStaInEdit_->setPlaceholderText("I");
    invStaLayout->addRow(tr("In Pin:"), invStaInEdit_);

    invStaOutEdit_ = new QLineEdit(linkParams_.inv.sta_guide.out, invStaGuideGroup_);
    invStaOutEdit_->setPlaceholderText("Z");
    invStaLayout->addRow(tr("Out Pin:"), invStaOutEdit_);

    invStaInstanceEdit_ = new QLineEdit(linkParams_.inv.sta_guide.instance, invStaGuideGroup_);
    invStaInstanceEdit_->setPlaceholderText("u_inv_sta");
    invStaLayout->addRow(tr("Instance:"), invStaInstanceEdit_);

    invLayout->addRow(invStaGuideGroup_);

    scrollLayout->addWidget(invGroup_);

    /* Link-level STA Guide (at end of chain) */
    linkStaGuideGroup_ = new QGroupBox(tr("Link STA Guide (end of processing chain)"), scrollWidget);
    linkStaGuideGroup_->setCheckable(true);
    linkStaGuideGroup_->setChecked(linkParams_.sta_guide.configured);

    auto *linkStaLayout = new QFormLayout(linkStaGuideGroup_);
    linkStaCellEdit_    = new QLineEdit(linkParams_.sta_guide.cell, linkStaGuideGroup_);
    linkStaCellEdit_->setPlaceholderText("FOUNDRY_GUIDE_BUF");
    linkStaLayout->addRow(tr("Cell:"), linkStaCellEdit_);

    linkStaInEdit_ = new QLineEdit(linkParams_.sta_guide.in, linkStaGuideGroup_);
    linkStaInEdit_->setPlaceholderText("A");
    linkStaLayout->addRow(tr("In Pin:"), linkStaInEdit_);

    linkStaOutEdit_ = new QLineEdit(linkParams_.sta_guide.out, linkStaGuideGroup_);
    linkStaOutEdit_->setPlaceholderText("Y");
    linkStaLayout->addRow(tr("Out Pin:"), linkStaOutEdit_);

    linkStaInstanceEdit_ = new QLineEdit(linkParams_.sta_guide.instance, linkStaGuideGroup_);
    linkStaInstanceEdit_->setPlaceholderText("u_link_sta");
    linkStaLayout->addRow(tr("Instance:"), linkStaInstanceEdit_);

    scrollLayout->addWidget(linkStaGuideGroup_);

    scrollLayout->addStretch();

    scrollArea->setWidget(scrollWidget);
    mainLayout_->addWidget(scrollArea);
}

ClockLinkParams PrcLinkConfigDialog::getLinkParams() const
{
    ClockLinkParams params;
    params.sourceName = sourceName_;

    /* ICG */
    params.icg.configured = icgGroup_->isChecked();
    if (params.icg.configured) {
        params.icg.enable         = icgEnableEdit_->text();
        params.icg.polarity       = icgPolarityCombo_->currentText();
        params.icg.test_enable    = icgTestEnableEdit_->text();
        params.icg.reset          = icgResetEdit_->text();
        params.icg.clock_on_reset = icgClockOnResetCheck_->isChecked();

        params.icg.sta_guide.configured = icgStaGuideGroup_->isChecked();
        if (params.icg.sta_guide.configured) {
            params.icg.sta_guide.cell     = icgStaCellEdit_->text();
            params.icg.sta_guide.in       = icgStaInEdit_->text();
            params.icg.sta_guide.out      = icgStaOutEdit_->text();
            params.icg.sta_guide.instance = icgStaInstanceEdit_->text();
        }
    }

    /* DIV */
    params.div.configured = divGroup_->isChecked();
    if (params.div.configured) {
        params.div.default_value  = divDefaultSpin_->value();
        params.div.value          = divValueEdit_->text();
        params.div.width          = divWidthSpin_->value();
        params.div.reset          = divResetEdit_->text();
        params.div.clock_on_reset = divClockOnResetCheck_->isChecked();

        params.div.sta_guide.configured = divStaGuideGroup_->isChecked();
        if (params.div.sta_guide.configured) {
            params.div.sta_guide.cell     = divStaCellEdit_->text();
            params.div.sta_guide.in       = divStaInEdit_->text();
            params.div.sta_guide.out      = divStaOutEdit_->text();
            params.div.sta_guide.instance = divStaInstanceEdit_->text();
        }
    }

    /* INV */
    params.inv.configured = invGroup_->isChecked();
    if (params.inv.configured) {
        params.inv.sta_guide.configured = invStaGuideGroup_->isChecked();
        if (params.inv.sta_guide.configured) {
            params.inv.sta_guide.cell     = invStaCellEdit_->text();
            params.inv.sta_guide.in       = invStaInEdit_->text();
            params.inv.sta_guide.out      = invStaOutEdit_->text();
            params.inv.sta_guide.instance = invStaInstanceEdit_->text();
        }
    }

    /* Link-level STA Guide */
    params.sta_guide.configured = linkStaGuideGroup_->isChecked();
    if (params.sta_guide.configured) {
        params.sta_guide.cell     = linkStaCellEdit_->text();
        params.sta_guide.in       = linkStaInEdit_->text();
        params.sta_guide.out      = linkStaOutEdit_->text();
        params.sta_guide.instance = linkStaInstanceEdit_->text();
    }

    return params;
}
