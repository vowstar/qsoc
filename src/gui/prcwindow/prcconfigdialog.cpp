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

/**
 * @brief Create a QLineEdit with an Auto button
 */
QWidget *PrcConfigDialog::createAutoLineEdit(
    QLineEdit    **lineEdit,
    const QString &initialValue,
    const QString &placeholder,
    const QString &autoValue,
    QWidget       *parent)
{
    auto *container = new QWidget(parent);
    auto *layout    = new QHBoxLayout(container);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);

    *lineEdit = new QLineEdit(initialValue, container);
    (*lineEdit)->setPlaceholderText(placeholder);
    layout->addWidget(*lineEdit, 1);

    auto *autoBtn = new QPushButton(tr("Auto"), container);
    autoBtn->setFixedWidth(50);
    autoBtn->setToolTip(tr("Auto-fill: %1").arg(autoValue.isEmpty() ? tr("(empty)") : autoValue));
    layout->addWidget(autoBtn);

    /* Only fill if empty */
    QLineEdit *edit = *lineEdit;
    QObject::connect(autoBtn, &QPushButton::clicked, [edit, autoValue]() {
        if (edit->text().isEmpty()) {
            edit->setText(autoValue);
        }
    });

    return container;
}

PrcConfigDialog::PrcConfigDialog(
    PrcPrimitiveItem *item, PrcScene *scene, const QStringList &connectedSources, QWidget *parent)
    : QDialog(parent)
    , item_(item)
    , scene_(scene)
    , connectedSources_(connectedSources)
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
    , targetMuxStaGroup_(nullptr)
    , targetMuxStaCellEdit_(nullptr)
    , targetMuxStaInEdit_(nullptr)
    , targetMuxStaOutEdit_(nullptr)
    , targetMuxStaInstanceEdit_(nullptr)
    , targetIcgStaGroup_(nullptr)
    , targetIcgStaCellEdit_(nullptr)
    , targetIcgStaInEdit_(nullptr)
    , targetIcgStaOutEdit_(nullptr)
    , targetIcgStaInstanceEdit_(nullptr)
    , targetDivStaGroup_(nullptr)
    , targetDivStaCellEdit_(nullptr)
    , targetDivStaInEdit_(nullptr)
    , targetDivStaOutEdit_(nullptr)
    , targetDivStaInstanceEdit_(nullptr)
    , targetInvStaGroup_(nullptr)
    , targetInvStaCellEdit_(nullptr)
    , targetInvStaInEdit_(nullptr)
    , targetInvStaOutEdit_(nullptr)
    , targetInvStaInstanceEdit_(nullptr)
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

    /* Derive auto values from target name */
    QString baseName = params.name;
    if (baseName.startsWith("clk_")) {
        baseName = baseName.mid(4);
    }
    QString autoSelect     = params.name + "_sel";
    QString autoReset      = "rst_" + baseName + "_n";
    QString autoTestClock  = "clk_hse";
    QString autoIcgEnable  = params.name + "_en";
    QString autoDivValue   = params.name + "_div";
    QString autoMuxStaInst = "u_DONTTOUCH_" + params.name + "_mux";
    QString autoIcgStaInst = "u_DONTTOUCH_" + params.name + "_icg";
    QString autoDivStaInst = "u_DONTTOUCH_" + params.name;
    QString autoInvStaInst = "u_DONTTOUCH_" + params.name + "_inv";
    int     sourceCount    = connectedSources_.size();
    bool    muxEnabled     = sourceCount >= 2;

    /* Basic target settings (full width) */
    auto *basicGroup  = new QGroupBox(tr("Target Settings"), this);
    auto *basicLayout = new QFormLayout(basicGroup);

    targetFreqEdit_ = new QLineEdit(params.freq, this);
    targetFreqEdit_->setPlaceholderText("400MHz");
    basicLayout->addRow(tr("Frequency:"), targetFreqEdit_);

    basicLayout->addRow(
        tr("Select:"),
        createAutoLineEdit(&targetSelectEdit_, params.select, autoSelect, autoSelect, this));

    basicLayout->addRow(
        tr("Reset:"),
        createAutoLineEdit(&targetResetEdit_, params.reset, autoReset, autoReset, this));

    basicLayout->addRow(
        tr("Test Clock:"),
        createAutoLineEdit(
            &targetTestClockEdit_, params.test_clock, autoTestClock, autoTestClock, this));

    mainLayout_->addWidget(basicGroup);

    /* Two-column layout for MUX/ICG (left) and DIV/INV (right) */
    auto *columnsWidget = new QWidget(this);
    auto *columnsLayout = new QHBoxLayout(columnsWidget);
    columnsLayout->setContentsMargins(0, 0, 0, 0);
    columnsLayout->setSpacing(8);

    auto *leftColumn = new QWidget(columnsWidget);
    auto *leftLayout = new QVBoxLayout(leftColumn);
    leftLayout->setContentsMargins(0, 0, 0, 0);

    auto *rightColumn = new QWidget(columnsWidget);
    auto *rightLayout = new QVBoxLayout(rightColumn);
    rightLayout->setContentsMargins(0, 0, 0, 0);

    /* MUX Group (left column) */
    QString muxTitle;
    if (sourceCount == 0) {
        muxTitle = tr("MUX (no sources)");
    } else if (sourceCount == 1) {
        muxTitle = tr("MUX (1 source)");
    } else {
        muxTitle = tr("MUX (%1 sources)").arg(sourceCount);
    }

    auto *muxGroup  = new QGroupBox(muxTitle, leftColumn);
    auto *muxLayout = new QFormLayout(muxGroup);
    muxGroup->setCheckable(true);
    muxGroup->setChecked(muxEnabled);

    connect(muxGroup, &QGroupBox::toggled, [muxGroup, muxEnabled](bool) {
        muxGroup->blockSignals(true);
        muxGroup->setChecked(muxEnabled);
        muxGroup->blockSignals(false);
    });

    if (!connectedSources_.isEmpty()) {
        auto *linksLabel = new QLabel(connectedSources_.join(", "), muxGroup);
        linksLabel->setWordWrap(true);
        linksLabel->setStyleSheet("color: #666; font-style: italic;");
        muxLayout->addRow(tr("Connected:"), linksLabel);
    } else {
        auto *noLinksLabel = new QLabel(tr("(connect inputs)"), muxGroup);
        noLinksLabel->setStyleSheet("color: #999; font-style: italic;");
        muxLayout->addRow(noLinksLabel);
    }

    targetMuxStaGroup_ = new QGroupBox(tr("STA Guide"), muxGroup);
    targetMuxStaGroup_->setCheckable(true);
    targetMuxStaGroup_->setChecked(params.mux.sta_guide.configured);
    auto *muxStaLayout = new QFormLayout(targetMuxStaGroup_);

    targetMuxStaCellEdit_ = new QLineEdit(params.mux.sta_guide.cell, targetMuxStaGroup_);
    targetMuxStaCellEdit_->setPlaceholderText("");
    muxStaLayout->addRow(tr("Cell:"), targetMuxStaCellEdit_);

    targetMuxStaInEdit_ = new QLineEdit(params.mux.sta_guide.in, targetMuxStaGroup_);
    targetMuxStaInEdit_->setPlaceholderText("A");
    muxStaLayout->addRow(tr("In:"), targetMuxStaInEdit_);

    targetMuxStaOutEdit_ = new QLineEdit(params.mux.sta_guide.out, targetMuxStaGroup_);
    targetMuxStaOutEdit_->setPlaceholderText("X");
    muxStaLayout->addRow(tr("Out:"), targetMuxStaOutEdit_);

    muxStaLayout->addRow(
        tr("Instance:"),
        createAutoLineEdit(
            &targetMuxStaInstanceEdit_,
            params.mux.sta_guide.instance,
            autoMuxStaInst,
            autoMuxStaInst,
            targetMuxStaGroup_));

    muxLayout->addRow(targetMuxStaGroup_);
    leftLayout->addWidget(muxGroup);

    /* ICG Group (left column) */
    auto *icgGroup  = new QGroupBox(tr("ICG (Clock Gating)"), leftColumn);
    auto *icgLayout = new QFormLayout(icgGroup);
    icgGroup->setCheckable(true);
    icgGroup->setChecked(params.icg.configured);

    icgLayout->addRow(
        tr("Enable:"),
        createAutoLineEdit(
            &targetIcgEnableEdit_, params.icg.enable, autoIcgEnable, autoIcgEnable, icgGroup));

    targetIcgPolarityCombo_ = new QComboBox(icgGroup);
    targetIcgPolarityCombo_->addItems({"high", "low"});
    targetIcgPolarityCombo_->setCurrentText(
        params.icg.polarity.isEmpty() ? "high" : params.icg.polarity);
    icgLayout->addRow(tr("Polarity:"), targetIcgPolarityCombo_);

    targetIcgClockOnResetCheck_ = new QCheckBox(tr("Clock on reset"), icgGroup);
    targetIcgClockOnResetCheck_->setChecked(params.icg.clock_on_reset);
    icgLayout->addRow(targetIcgClockOnResetCheck_);

    targetIcgStaGroup_ = new QGroupBox(tr("STA Guide"), icgGroup);
    targetIcgStaGroup_->setCheckable(true);
    targetIcgStaGroup_->setChecked(params.icg.sta_guide.configured);
    auto *icgStaLayout = new QFormLayout(targetIcgStaGroup_);

    targetIcgStaCellEdit_ = new QLineEdit(params.icg.sta_guide.cell, targetIcgStaGroup_);
    targetIcgStaCellEdit_->setPlaceholderText("");
    icgStaLayout->addRow(tr("Cell:"), targetIcgStaCellEdit_);

    targetIcgStaInEdit_ = new QLineEdit(params.icg.sta_guide.in, targetIcgStaGroup_);
    targetIcgStaInEdit_->setPlaceholderText("A");
    icgStaLayout->addRow(tr("In:"), targetIcgStaInEdit_);

    targetIcgStaOutEdit_ = new QLineEdit(params.icg.sta_guide.out, targetIcgStaGroup_);
    targetIcgStaOutEdit_->setPlaceholderText("X");
    icgStaLayout->addRow(tr("Out:"), targetIcgStaOutEdit_);

    icgStaLayout->addRow(
        tr("Instance:"),
        createAutoLineEdit(
            &targetIcgStaInstanceEdit_,
            params.icg.sta_guide.instance,
            autoIcgStaInst,
            autoIcgStaInst,
            targetIcgStaGroup_));

    icgLayout->addRow(targetIcgStaGroup_);
    leftLayout->addWidget(icgGroup);
    leftLayout->addStretch();

    /* DIV Group (right column) */
    auto *divGroup  = new QGroupBox(tr("DIV (Clock Divider)"), rightColumn);
    auto *divLayout = new QFormLayout(divGroup);
    divGroup->setCheckable(true);
    divGroup->setChecked(params.div.configured);

    targetDivDefaultSpin_ = new QSpinBox(divGroup);
    targetDivDefaultSpin_->setRange(1, 65535);
    targetDivDefaultSpin_->setValue(params.div.default_value);
    divLayout->addRow(tr("Default:"), targetDivDefaultSpin_);

    divLayout->addRow(
        tr("Value:"),
        createAutoLineEdit(
            &targetDivValueEdit_, params.div.value, autoDivValue, autoDivValue, divGroup));

    targetDivWidthSpin_ = new QSpinBox(divGroup);
    targetDivWidthSpin_->setRange(0, 32);
    targetDivWidthSpin_->setValue(params.div.width);
    targetDivWidthSpin_->setSpecialValueText("auto");
    divLayout->addRow(tr("Width:"), targetDivWidthSpin_);

    divLayout->addRow(
        tr("Reset:"),
        createAutoLineEdit(&targetDivResetEdit_, params.div.reset, autoReset, autoReset, divGroup));

    targetDivClockOnResetCheck_ = new QCheckBox(tr("Clock on reset"), divGroup);
    targetDivClockOnResetCheck_->setChecked(params.div.clock_on_reset);
    divLayout->addRow(targetDivClockOnResetCheck_);

    targetDivStaGroup_ = new QGroupBox(tr("STA Guide"), divGroup);
    targetDivStaGroup_->setCheckable(true);
    targetDivStaGroup_->setChecked(params.div.sta_guide.configured);
    auto *divStaLayout = new QFormLayout(targetDivStaGroup_);

    targetDivStaCellEdit_ = new QLineEdit(params.div.sta_guide.cell, targetDivStaGroup_);
    targetDivStaCellEdit_->setPlaceholderText("");
    divStaLayout->addRow(tr("Cell:"), targetDivStaCellEdit_);

    targetDivStaInEdit_ = new QLineEdit(params.div.sta_guide.in, targetDivStaGroup_);
    targetDivStaInEdit_->setPlaceholderText("A");
    divStaLayout->addRow(tr("In:"), targetDivStaInEdit_);

    targetDivStaOutEdit_ = new QLineEdit(params.div.sta_guide.out, targetDivStaGroup_);
    targetDivStaOutEdit_->setPlaceholderText("X");
    divStaLayout->addRow(tr("Out:"), targetDivStaOutEdit_);

    divStaLayout->addRow(
        tr("Instance:"),
        createAutoLineEdit(
            &targetDivStaInstanceEdit_,
            params.div.sta_guide.instance,
            autoDivStaInst,
            autoDivStaInst,
            targetDivStaGroup_));

    divLayout->addRow(targetDivStaGroup_);
    rightLayout->addWidget(divGroup);

    /* INV Group (right column) */
    auto *invGroup  = new QGroupBox(tr("INV (Clock Inverter)"), rightColumn);
    auto *invLayout = new QFormLayout(invGroup);
    invGroup->setCheckable(true);
    invGroup->setChecked(params.inv.configured);

    targetInvStaGroup_ = new QGroupBox(tr("STA Guide"), invGroup);
    targetInvStaGroup_->setCheckable(true);
    targetInvStaGroup_->setChecked(params.inv.sta_guide.configured);
    auto *invStaLayout = new QFormLayout(targetInvStaGroup_);

    targetInvStaCellEdit_ = new QLineEdit(params.inv.sta_guide.cell, targetInvStaGroup_);
    targetInvStaCellEdit_->setPlaceholderText("");
    invStaLayout->addRow(tr("Cell:"), targetInvStaCellEdit_);

    targetInvStaInEdit_ = new QLineEdit(params.inv.sta_guide.in, targetInvStaGroup_);
    targetInvStaInEdit_->setPlaceholderText("A");
    invStaLayout->addRow(tr("In:"), targetInvStaInEdit_);

    targetInvStaOutEdit_ = new QLineEdit(params.inv.sta_guide.out, targetInvStaGroup_);
    targetInvStaOutEdit_->setPlaceholderText("X");
    invStaLayout->addRow(tr("Out:"), targetInvStaOutEdit_);

    invStaLayout->addRow(
        tr("Instance:"),
        createAutoLineEdit(
            &targetInvStaInstanceEdit_,
            params.inv.sta_guide.instance,
            autoInvStaInst,
            autoInvStaInst,
            targetInvStaGroup_));

    invLayout->addRow(targetInvStaGroup_);
    rightLayout->addWidget(invGroup);
    rightLayout->addStretch();

    columnsLayout->addWidget(leftColumn);
    columnsLayout->addWidget(rightColumn);
    mainLayout_->addWidget(columnsWidget);

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

    /* Determine controller type based on primitive type */
    PrcControllerDialog::ControllerType ctrlType;
    switch (item_->primitiveType()) {
    case ClockInput:
    case ClockTarget:
        ctrlType = PrcControllerDialog::ClockController;
        break;
    case ResetSource:
    case ResetTarget:
        ctrlType = PrcControllerDialog::ResetController;
        break;
    case PowerDomain:
        ctrlType = PrcControllerDialog::PowerController;
        break;
    default:
        return;
    }

    /* Show controller dialog */
    PrcControllerDialog dialog(ctrlType, controllerName, scene_, this);

    /* Handle delete request */
    connect(&dialog, &PrcControllerDialog::deleteRequested, [this, controllerName, ctrlType]() {
        switch (ctrlType) {
        case PrcControllerDialog::ClockController:
            scene_->removeClockController(controllerName);
            break;
        case PrcControllerDialog::ResetController:
            scene_->removeResetController(controllerName);
            break;
        case PrcControllerDialog::PowerController:
            scene_->removePowerController(controllerName);
            break;
        }
        populateControllerCombo();
    });

    if (dialog.exec() == QDialog::Accepted) {
        /* Apply controller changes */
        switch (ctrlType) {
        case PrcControllerDialog::ClockController:
            scene_->setClockController(controllerName, dialog.getClockControllerDef());
            break;
        case PrcControllerDialog::ResetController:
            scene_->setResetController(controllerName, dialog.getResetControllerDef());
            break;
        case PrcControllerDialog::PowerController:
            scene_->setPowerController(controllerName, dialog.getPowerControllerDef());
            break;
        }
    }
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

        /* MUX - auto-determined by connection count */
        params.mux.configured = connectedSources_.size() >= 2;
        if (targetMuxStaGroup_) {
            params.mux.sta_guide.configured = targetMuxStaGroup_->isChecked();
            if (params.mux.sta_guide.configured) {
                params.mux.sta_guide.cell     = targetMuxStaCellEdit_->text();
                params.mux.sta_guide.in       = targetMuxStaInEdit_->text();
                params.mux.sta_guide.out      = targetMuxStaOutEdit_->text();
                params.mux.sta_guide.instance = targetMuxStaInstanceEdit_->text();
            }
        }

        /* ICG */
        auto *icgGroup        = reinterpret_cast<QGroupBox *>(targetIcgCheck_);
        params.icg.configured = icgGroup ? icgGroup->isChecked() : false;
        if (params.icg.configured) {
            params.icg.enable         = targetIcgEnableEdit_->text();
            params.icg.polarity       = targetIcgPolarityCombo_->currentText();
            params.icg.clock_on_reset = targetIcgClockOnResetCheck_->isChecked();
        }
        if (targetIcgStaGroup_) {
            params.icg.sta_guide.configured = targetIcgStaGroup_->isChecked();
            if (params.icg.sta_guide.configured) {
                params.icg.sta_guide.cell     = targetIcgStaCellEdit_->text();
                params.icg.sta_guide.in       = targetIcgStaInEdit_->text();
                params.icg.sta_guide.out      = targetIcgStaOutEdit_->text();
                params.icg.sta_guide.instance = targetIcgStaInstanceEdit_->text();
            }
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
        if (targetDivStaGroup_) {
            params.div.sta_guide.configured = targetDivStaGroup_->isChecked();
            if (params.div.sta_guide.configured) {
                params.div.sta_guide.cell     = targetDivStaCellEdit_->text();
                params.div.sta_guide.in       = targetDivStaInEdit_->text();
                params.div.sta_guide.out      = targetDivStaOutEdit_->text();
                params.div.sta_guide.instance = targetDivStaInstanceEdit_->text();
            }
        }

        /* INV */
        auto *invGroup        = reinterpret_cast<QGroupBox *>(targetInvCheck_);
        params.inv.configured = invGroup ? invGroup->isChecked() : false;
        if (targetInvStaGroup_) {
            params.inv.sta_guide.configured = targetInvStaGroup_->isChecked();
            if (params.inv.sta_guide.configured) {
                params.inv.sta_guide.cell     = targetInvStaCellEdit_->text();
                params.inv.sta_guide.in       = targetInvStaInEdit_->text();
                params.inv.sta_guide.out      = targetInvStaOutEdit_->text();
                params.inv.sta_guide.instance = targetInvStaInstanceEdit_->text();
            }
        }

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

/**
 * @brief Create a QLineEdit with an Auto button for PrcLinkConfigDialog
 */
QWidget *PrcLinkConfigDialog::createAutoLineEdit(
    QLineEdit    **lineEdit,
    const QString &initialValue,
    const QString &placeholder,
    const QString &autoValue,
    QWidget       *parent)
{
    auto *container = new QWidget(parent);
    auto *layout    = new QHBoxLayout(container);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);

    *lineEdit = new QLineEdit(initialValue, container);
    (*lineEdit)->setPlaceholderText(placeholder);
    layout->addWidget(*lineEdit, 1);

    auto *autoBtn = new QPushButton(tr("Auto"), container);
    autoBtn->setFixedWidth(50);
    autoBtn->setToolTip(tr("Auto-fill: %1").arg(autoValue.isEmpty() ? tr("(empty)") : autoValue));
    layout->addWidget(autoBtn);

    /* Only fill if empty */
    QLineEdit *edit = *lineEdit;
    QObject::connect(autoBtn, &QPushButton::clicked, [edit, autoValue]() {
        if (edit->text().isEmpty()) {
            edit->setText(autoValue);
        }
    });

    return container;
}

void PrcLinkConfigDialog::createForm()
{
    /* Derive auto values from target name */
    QString baseName = targetName_;
    if (baseName.startsWith("clk_")) {
        baseName = baseName.mid(4);
    }
    QString autoIcgEnable      = targetName_ + "_en";
    QString autoReset          = "rst_" + baseName + "_n";
    QString autoDivValue       = targetName_ + "_div";
    QString autoIcgStaInstance = "u_DONTTOUCH_" + targetName_ + "_icg";
    QString autoDivStaInstance = "u_DONTTOUCH_" + targetName_;
    QString autoInvStaInstance = "u_DONTTOUCH_" + targetName_ + "_inv";
    QString autoLinkInstance   = "u_DONTTOUCH_" + targetName_ + "_link";

    /* Two-column layout */
    auto *columnsWidget = new QWidget(this);
    auto *columnsLayout = new QHBoxLayout(columnsWidget);
    columnsLayout->setContentsMargins(0, 0, 0, 0);
    columnsLayout->setSpacing(8);

    /* Left column: ICG + DIV */
    auto *leftColumn = new QWidget(columnsWidget);
    auto *leftLayout = new QVBoxLayout(leftColumn);
    leftLayout->setContentsMargins(0, 0, 0, 0);

    /* Right column: INV + Link STA */
    auto *rightColumn = new QWidget(columnsWidget);
    auto *rightLayout = new QVBoxLayout(rightColumn);
    rightLayout->setContentsMargins(0, 0, 0, 0);

    /* ICG Group */
    icgGroup_ = new QGroupBox(tr("ICG (Clock Gating)"), leftColumn);
    icgGroup_->setCheckable(true);
    icgGroup_->setChecked(linkParams_.icg.configured);

    auto *icgLayout = new QFormLayout(icgGroup_);

    icgLayout->addRow(
        tr("Enable:"),
        createAutoLineEdit(
            &icgEnableEdit_, linkParams_.icg.enable, autoIcgEnable, autoIcgEnable, icgGroup_));

    icgPolarityCombo_ = new QComboBox(icgGroup_);
    icgPolarityCombo_->addItems({"high", "low"});
    icgPolarityCombo_->setCurrentText(
        linkParams_.icg.polarity.isEmpty() ? "high" : linkParams_.icg.polarity);
    icgLayout->addRow(tr("Polarity:"), icgPolarityCombo_);

    icgTestEnableEdit_ = new QLineEdit(linkParams_.icg.test_enable, icgGroup_);
    icgTestEnableEdit_->setPlaceholderText("test_en");
    icgLayout->addRow(tr("Test Enable:"), icgTestEnableEdit_);

    icgLayout->addRow(
        tr("Reset:"),
        createAutoLineEdit(&icgResetEdit_, linkParams_.icg.reset, autoReset, autoReset, icgGroup_));

    icgClockOnResetCheck_ = new QCheckBox(tr("Clock on reset"), icgGroup_);
    icgClockOnResetCheck_->setChecked(linkParams_.icg.clock_on_reset);
    icgLayout->addRow(icgClockOnResetCheck_);

    /* ICG STA Guide */
    icgStaGuideGroup_ = new QGroupBox(tr("STA Guide"), icgGroup_);
    icgStaGuideGroup_->setCheckable(true);
    icgStaGuideGroup_->setChecked(linkParams_.icg.sta_guide.configured);

    auto *icgStaLayout = new QFormLayout(icgStaGuideGroup_);
    icgStaCellEdit_    = new QLineEdit(linkParams_.icg.sta_guide.cell, icgStaGuideGroup_);
    icgStaCellEdit_->setPlaceholderText("");
    icgStaLayout->addRow(tr("Cell:"), icgStaCellEdit_);

    icgStaInEdit_ = new QLineEdit(linkParams_.icg.sta_guide.in, icgStaGuideGroup_);
    icgStaInEdit_->setPlaceholderText("A");
    icgStaLayout->addRow(tr("In:"), icgStaInEdit_);

    icgStaOutEdit_ = new QLineEdit(linkParams_.icg.sta_guide.out, icgStaGuideGroup_);
    icgStaOutEdit_->setPlaceholderText("X");
    icgStaLayout->addRow(tr("Out:"), icgStaOutEdit_);

    icgStaLayout->addRow(
        tr("Instance:"),
        createAutoLineEdit(
            &icgStaInstanceEdit_,
            linkParams_.icg.sta_guide.instance,
            autoIcgStaInstance,
            autoIcgStaInstance,
            icgStaGuideGroup_));

    icgLayout->addRow(icgStaGuideGroup_);
    leftLayout->addWidget(icgGroup_);

    /* DIV Group */
    divGroup_ = new QGroupBox(tr("DIV (Clock Divider)"), leftColumn);
    divGroup_->setCheckable(true);
    divGroup_->setChecked(linkParams_.div.configured);

    auto *divLayout = new QFormLayout(divGroup_);

    divDefaultSpin_ = new QSpinBox(divGroup_);
    divDefaultSpin_->setRange(1, 65535);
    divDefaultSpin_->setValue(linkParams_.div.default_value);
    divLayout->addRow(tr("Default:"), divDefaultSpin_);

    divLayout->addRow(
        tr("Value:"),
        createAutoLineEdit(
            &divValueEdit_, linkParams_.div.value, autoDivValue, autoDivValue, divGroup_));

    divWidthSpin_ = new QSpinBox(divGroup_);
    divWidthSpin_->setRange(0, 32);
    divWidthSpin_->setValue(linkParams_.div.width);
    divWidthSpin_->setSpecialValueText("auto");
    divLayout->addRow(tr("Width:"), divWidthSpin_);

    divLayout->addRow(
        tr("Reset:"),
        createAutoLineEdit(&divResetEdit_, linkParams_.div.reset, autoReset, autoReset, divGroup_));

    divClockOnResetCheck_ = new QCheckBox(tr("Clock on reset"), divGroup_);
    divClockOnResetCheck_->setChecked(linkParams_.div.clock_on_reset);
    divLayout->addRow(divClockOnResetCheck_);

    /* DIV STA Guide */
    divStaGuideGroup_ = new QGroupBox(tr("STA Guide"), divGroup_);
    divStaGuideGroup_->setCheckable(true);
    divStaGuideGroup_->setChecked(linkParams_.div.sta_guide.configured);

    auto *divStaLayout = new QFormLayout(divStaGuideGroup_);
    divStaCellEdit_    = new QLineEdit(linkParams_.div.sta_guide.cell, divStaGuideGroup_);
    divStaCellEdit_->setPlaceholderText("");
    divStaLayout->addRow(tr("Cell:"), divStaCellEdit_);

    divStaInEdit_ = new QLineEdit(linkParams_.div.sta_guide.in, divStaGuideGroup_);
    divStaInEdit_->setPlaceholderText("A");
    divStaLayout->addRow(tr("In:"), divStaInEdit_);

    divStaOutEdit_ = new QLineEdit(linkParams_.div.sta_guide.out, divStaGuideGroup_);
    divStaOutEdit_->setPlaceholderText("X");
    divStaLayout->addRow(tr("Out:"), divStaOutEdit_);

    divStaLayout->addRow(
        tr("Instance:"),
        createAutoLineEdit(
            &divStaInstanceEdit_,
            linkParams_.div.sta_guide.instance,
            autoDivStaInstance,
            autoDivStaInstance,
            divStaGuideGroup_));

    divLayout->addRow(divStaGuideGroup_);
    leftLayout->addWidget(divGroup_);
    leftLayout->addStretch();

    /* INV Group */
    invGroup_ = new QGroupBox(tr("INV (Clock Inverter)"), rightColumn);
    invGroup_->setCheckable(true);
    invGroup_->setChecked(linkParams_.inv.configured);

    auto *invLayout = new QFormLayout(invGroup_);

    /* INV STA Guide */
    invStaGuideGroup_ = new QGroupBox(tr("STA Guide"), invGroup_);
    invStaGuideGroup_->setCheckable(true);
    invStaGuideGroup_->setChecked(linkParams_.inv.sta_guide.configured);

    auto *invStaLayout = new QFormLayout(invStaGuideGroup_);
    invStaCellEdit_    = new QLineEdit(linkParams_.inv.sta_guide.cell, invStaGuideGroup_);
    invStaCellEdit_->setPlaceholderText("");
    invStaLayout->addRow(tr("Cell:"), invStaCellEdit_);

    invStaInEdit_ = new QLineEdit(linkParams_.inv.sta_guide.in, invStaGuideGroup_);
    invStaInEdit_->setPlaceholderText("A");
    invStaLayout->addRow(tr("In:"), invStaInEdit_);

    invStaOutEdit_ = new QLineEdit(linkParams_.inv.sta_guide.out, invStaGuideGroup_);
    invStaOutEdit_->setPlaceholderText("X");
    invStaLayout->addRow(tr("Out:"), invStaOutEdit_);

    invStaLayout->addRow(
        tr("Instance:"),
        createAutoLineEdit(
            &invStaInstanceEdit_,
            linkParams_.inv.sta_guide.instance,
            autoInvStaInstance,
            autoInvStaInstance,
            invStaGuideGroup_));

    invLayout->addRow(invStaGuideGroup_);
    rightLayout->addWidget(invGroup_);

    /* Link-level STA Guide */
    linkStaGuideGroup_ = new QGroupBox(tr("Link STA Guide"), rightColumn);
    linkStaGuideGroup_->setCheckable(true);
    linkStaGuideGroup_->setChecked(linkParams_.sta_guide.configured);

    auto *linkStaLayout = new QFormLayout(linkStaGuideGroup_);
    linkStaCellEdit_    = new QLineEdit(linkParams_.sta_guide.cell, linkStaGuideGroup_);
    linkStaCellEdit_->setPlaceholderText("");
    linkStaLayout->addRow(tr("Cell:"), linkStaCellEdit_);

    linkStaInEdit_ = new QLineEdit(linkParams_.sta_guide.in, linkStaGuideGroup_);
    linkStaInEdit_->setPlaceholderText("A");
    linkStaLayout->addRow(tr("In:"), linkStaInEdit_);

    linkStaOutEdit_ = new QLineEdit(linkParams_.sta_guide.out, linkStaGuideGroup_);
    linkStaOutEdit_->setPlaceholderText("X");
    linkStaLayout->addRow(tr("Out:"), linkStaOutEdit_);

    linkStaLayout->addRow(
        tr("Instance:"),
        createAutoLineEdit(
            &linkStaInstanceEdit_,
            linkParams_.sta_guide.instance,
            autoLinkInstance,
            autoLinkInstance,
            linkStaGuideGroup_));

    rightLayout->addWidget(linkStaGuideGroup_);
    rightLayout->addStretch();

    columnsLayout->addWidget(leftColumn);
    columnsLayout->addWidget(rightColumn);
    mainLayout_->addWidget(columnsWidget);
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

/* Controller Dialog Implementation */

PrcControllerDialog::PrcControllerDialog(
    ControllerType type, const QString &name, PrcScene *scene, QWidget *parent)
    : QDialog(parent)
    , type_(type)
    , name_(name)
    , scene_(scene)
    , mainLayout_(nullptr)
    , nameEdit_(nullptr)
    , testEnableEdit_(nullptr)
    , hostClockEdit_(nullptr)
    , hostResetEdit_(nullptr)
    , elementsList_(nullptr)
    , deleteBtn_(nullptr)
{
    /* Set window title based on type */
    QString typeStr;
    switch (type_) {
    case ClockController:
        typeStr = tr("Clock");
        break;
    case ResetController:
        typeStr = tr("Reset");
        break;
    case PowerController:
        typeStr = tr("Power");
        break;
    }
    setWindowTitle(QString(tr("Configure %1 Controller")).arg(typeStr));
    setMinimumWidth(400);

    mainLayout_ = new QVBoxLayout(this);

    createForm();

    /* Dialog buttons */
    auto *buttonLayout = new QHBoxLayout();

    deleteBtn_ = new QPushButton(tr("Delete Controller"), this);
    deleteBtn_->setStyleSheet("color: #c00;");
    connect(deleteBtn_, &QPushButton::clicked, this, &PrcControllerDialog::onDeleteClicked);
    buttonLayout->addWidget(deleteBtn_);

    buttonLayout->addStretch();

    auto *buttonBox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttonBox, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);
    buttonLayout->addWidget(buttonBox);

    mainLayout_->addLayout(buttonLayout);

    setLayout(mainLayout_);
}

void PrcControllerDialog::createForm()
{
    /* Basic Information Group */
    auto *basicGroup  = new QGroupBox(tr("Basic Information"), this);
    auto *basicLayout = new QFormLayout(basicGroup);

    nameEdit_ = new QLineEdit(name_, this);
    nameEdit_->setReadOnly(true);
    nameEdit_->setStyleSheet("background-color: #f0f0f0;");
    basicLayout->addRow(tr("Name:"), nameEdit_);

    mainLayout_->addWidget(basicGroup);

    /* DFT Settings Group */
    auto *dftGroup  = new QGroupBox(tr("DFT Settings"), this);
    auto *dftLayout = new QFormLayout(dftGroup);

    /* Create test_enable field with Auto button */
    auto   *testEnableContainer = new QWidget(this);
    auto   *testEnableLayout    = new QHBoxLayout(testEnableContainer);
    QString autoTestEnable      = "test_en";
    testEnableLayout->setContentsMargins(0, 0, 0, 0);
    testEnableLayout->setSpacing(4);

    testEnableEdit_ = new QLineEdit(this);
    testEnableEdit_->setPlaceholderText(autoTestEnable);
    testEnableLayout->addWidget(testEnableEdit_, 1);

    auto *testEnableAutoBtn = new QPushButton(tr("Auto"), this);
    testEnableAutoBtn->setFixedWidth(50);
    testEnableAutoBtn->setToolTip(tr("Auto-fill: %1").arg(autoTestEnable));
    testEnableLayout->addWidget(testEnableAutoBtn);

    connect(testEnableAutoBtn, &QPushButton::clicked, [this, autoTestEnable]() {
        if (testEnableEdit_->text().isEmpty()) {
            testEnableEdit_->setText(autoTestEnable);
        }
    });

    /* Load existing value */
    if (scene_) {
        switch (type_) {
        case ClockController:
            if (scene_->hasClockController(name_)) {
                testEnableEdit_->setText(scene_->clockController(name_).test_enable);
            }
            break;
        case ResetController:
            if (scene_->hasResetController(name_)) {
                testEnableEdit_->setText(scene_->resetController(name_).test_enable);
            }
            break;
        case PowerController:
            if (scene_->hasPowerController(name_)) {
                testEnableEdit_->setText(scene_->powerController(name_).test_enable);
            }
            break;
        }
    }

    dftLayout->addRow(tr("Test Enable:"), testEnableContainer);

    auto *dftHint = new QLabel(tr("DFT bypass signal for scan testing"), this);
    dftHint->setStyleSheet("color: #666; font-style: italic;");
    dftLayout->addRow(dftHint);

    mainLayout_->addWidget(dftGroup);

    /* AO Domain Settings (Power Controller only) */
    if (type_ == PowerController) {
        auto *aoGroup  = new QGroupBox(tr("AO Domain Settings"), this);
        auto *aoLayout = new QFormLayout(aoGroup);

        /* Create host_clock field with Auto button */
        auto   *hostClockContainer = new QWidget(this);
        auto   *hostClockLayout    = new QHBoxLayout(hostClockContainer);
        QString autoHostClock      = "ao_clk";
        hostClockLayout->setContentsMargins(0, 0, 0, 0);
        hostClockLayout->setSpacing(4);

        hostClockEdit_ = new QLineEdit(this);
        hostClockEdit_->setPlaceholderText(autoHostClock);
        hostClockLayout->addWidget(hostClockEdit_, 1);

        auto *hostClockAutoBtn = new QPushButton(tr("Auto"), this);
        hostClockAutoBtn->setFixedWidth(50);
        hostClockAutoBtn->setToolTip(tr("Auto-fill: %1").arg(autoHostClock));
        hostClockLayout->addWidget(hostClockAutoBtn);

        connect(hostClockAutoBtn, &QPushButton::clicked, [this, autoHostClock]() {
            if (hostClockEdit_->text().isEmpty()) {
                hostClockEdit_->setText(autoHostClock);
            }
        });

        /* Create host_reset field with Auto button */
        auto   *hostResetContainer = new QWidget(this);
        auto   *hostResetLayout    = new QHBoxLayout(hostResetContainer);
        QString autoHostReset      = "ao_rst_n";
        hostResetLayout->setContentsMargins(0, 0, 0, 0);
        hostResetLayout->setSpacing(4);

        hostResetEdit_ = new QLineEdit(this);
        hostResetEdit_->setPlaceholderText(autoHostReset);
        hostResetLayout->addWidget(hostResetEdit_, 1);

        auto *hostResetAutoBtn = new QPushButton(tr("Auto"), this);
        hostResetAutoBtn->setFixedWidth(50);
        hostResetAutoBtn->setToolTip(tr("Auto-fill: %1").arg(autoHostReset));
        hostResetLayout->addWidget(hostResetAutoBtn);

        connect(hostResetAutoBtn, &QPushButton::clicked, [this, autoHostReset]() {
            if (hostResetEdit_->text().isEmpty()) {
                hostResetEdit_->setText(autoHostReset);
            }
        });

        /* Load existing values */
        if (scene_ && scene_->hasPowerController(name_)) {
            auto def = scene_->powerController(name_);
            hostClockEdit_->setText(def.host_clock);
            hostResetEdit_->setText(def.host_reset);
        }

        aoLayout->addRow(tr("Host Clock:"), hostClockContainer);
        aoLayout->addRow(tr("Host Reset:"), hostResetContainer);

        auto *aoHint = new QLabel(tr("Always-on domain clock and reset signals"), this);
        aoHint->setStyleSheet("color: #666; font-style: italic;");
        aoLayout->addRow(aoHint);

        mainLayout_->addWidget(aoGroup);
    }

    /* Assigned Elements Group */
    auto *elemGroup  = new QGroupBox(tr("Assigned Elements"), this);
    auto *elemLayout = new QVBoxLayout(elemGroup);

    elementsList_ = new QListWidget(this);
    elementsList_->setMaximumHeight(120);
    elementsList_->setSelectionMode(QAbstractItemView::NoSelection);
    populateElementsList();

    elemLayout->addWidget(elementsList_);

    auto *elemHint = new QLabel(tr("Elements using this controller (read-only)"), this);
    elemHint->setStyleSheet("color: #666; font-style: italic;");
    elemLayout->addWidget(elemHint);

    mainLayout_->addWidget(elemGroup);
}

void PrcControllerDialog::populateElementsList()
{
    if (!scene_ || !elementsList_) {
        return;
    }

    elementsList_->clear();

    /* Find all elements assigned to this controller */
    for (const auto &node : scene_->nodes()) {
        auto prcItem = std::dynamic_pointer_cast<PrcPrimitiveItem>(node);
        if (!prcItem) {
            continue;
        }

        QString     itemController;
        QString     itemName;
        QString     itemType;
        const auto &params = prcItem->params();

        switch (type_) {
        case ClockController:
            if (std::holds_alternative<ClockInputParams>(params)) {
                const auto &p  = std::get<ClockInputParams>(params);
                itemController = p.controller;
                itemName       = p.name;
                itemType       = tr("Input");
            } else if (std::holds_alternative<ClockTargetParams>(params)) {
                const auto &p  = std::get<ClockTargetParams>(params);
                itemController = p.controller;
                itemName       = p.name;
                itemType       = tr("Target");
            }
            break;
        case ResetController:
            if (std::holds_alternative<ResetSourceParams>(params)) {
                const auto &p  = std::get<ResetSourceParams>(params);
                itemController = p.controller;
                itemName       = p.name;
                itemType       = tr("Source");
            } else if (std::holds_alternative<ResetTargetParams>(params)) {
                const auto &p  = std::get<ResetTargetParams>(params);
                itemController = p.controller;
                itemName       = p.name;
                itemType       = tr("Target");
            }
            break;
        case PowerController:
            if (std::holds_alternative<PowerDomainParams>(params)) {
                const auto &p  = std::get<PowerDomainParams>(params);
                itemController = p.controller;
                itemName       = p.name;
                itemType       = tr("Domain");
            }
            break;
        }

        if (itemController == name_ && !itemName.isEmpty()) {
            auto *item = new QListWidgetItem(QString("%1 (%2)").arg(itemName, itemType));
            elementsList_->addItem(item);
        }
    }

    if (elementsList_->count() == 0) {
        auto *item = new QListWidgetItem(tr("(no elements assigned)"));
        item->setForeground(Qt::gray);
        elementsList_->addItem(item);
    }
}

void PrcControllerDialog::onDeleteClicked()
{
    /* Check if controller has assigned elements */
    if (elementsList_ && elementsList_->count() > 0) {
        auto *firstItem = elementsList_->item(0);
        if (firstItem && !firstItem->text().startsWith("(")) {
            QMessageBox::warning(
                this,
                tr("Cannot Delete"),
                tr("This controller has assigned elements.\n"
                   "Please reassign or remove all elements before deleting."));
            return;
        }
    }

    /* Confirm deletion */
    auto result = QMessageBox::question(
        this,
        tr("Delete Controller"),
        tr("Are you sure you want to delete controller '%1'?").arg(name_),
        QMessageBox::Yes | QMessageBox::No);

    if (result == QMessageBox::Yes) {
        emit deleteRequested();
        reject();
    }
}

ClockControllerDef PrcControllerDialog::getClockControllerDef() const
{
    ClockControllerDef def;
    def.name        = name_;
    def.test_enable = testEnableEdit_ ? testEnableEdit_->text() : QString();
    return def;
}

ResetControllerDef PrcControllerDialog::getResetControllerDef() const
{
    ResetControllerDef def;
    def.name        = name_;
    def.test_enable = testEnableEdit_ ? testEnableEdit_->text() : QString();
    return def;
}

PowerControllerDef PrcControllerDialog::getPowerControllerDef() const
{
    PowerControllerDef def;
    def.name        = name_;
    def.test_enable = testEnableEdit_ ? testEnableEdit_->text() : QString();
    def.host_clock  = hostClockEdit_ ? hostClockEdit_->text() : QString();
    def.host_reset  = hostResetEdit_ ? hostResetEdit_->text() : QString();
    return def;
}
