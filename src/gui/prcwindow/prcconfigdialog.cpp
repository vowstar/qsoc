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
    , item(item)
    , scene(scene)
    , connectedSources(connectedSources)
    , mainLayout(nullptr)
    , nameEdit(nullptr)
    , controllerCombo(nullptr)
    , editControllerBtn(nullptr)
    , inputFreqEdit(nullptr)
    , targetFreqEdit(nullptr)
    , targetMuxCheck(nullptr)
    , targetSelectEdit(nullptr)
    , targetResetEdit(nullptr)
    , targetTestClockEdit(nullptr)
    , targetIcgCheck(nullptr)
    , targetIcgEnableEdit(nullptr)
    , targetIcgPolarityCombo(nullptr)
    , targetIcgClockOnResetCheck(nullptr)
    , targetDivCheck(nullptr)
    , targetDivDefaultSpin(nullptr)
    , targetDivValueEdit(nullptr)
    , targetDivWidthSpin(nullptr)
    , targetDivResetEdit(nullptr)
    , targetDivClockOnResetCheck(nullptr)
    , targetInvCheck(nullptr)
    , targetMuxStaGroup(nullptr)
    , targetMuxStaCellEdit(nullptr)
    , targetMuxStaInEdit(nullptr)
    , targetMuxStaOutEdit(nullptr)
    , targetMuxStaInstanceEdit(nullptr)
    , targetIcgStaGroup(nullptr)
    , targetIcgStaCellEdit(nullptr)
    , targetIcgStaInEdit(nullptr)
    , targetIcgStaOutEdit(nullptr)
    , targetIcgStaInstanceEdit(nullptr)
    , targetDivStaGroup(nullptr)
    , targetDivStaCellEdit(nullptr)
    , targetDivStaInEdit(nullptr)
    , targetDivStaOutEdit(nullptr)
    , targetDivStaInstanceEdit(nullptr)
    , targetInvStaGroup(nullptr)
    , targetInvStaCellEdit(nullptr)
    , targetInvStaInEdit(nullptr)
    , targetInvStaOutEdit(nullptr)
    , targetInvStaInstanceEdit(nullptr)
    , rstSrcActiveCombo(nullptr)
    , rstTgtActiveCombo(nullptr)
    , rstTgtAsyncCheck(nullptr)
    , rstTgtAsyncClockEdit(nullptr)
    , rstTgtAsyncStageSpin(nullptr)
    , pwrDomVoltageSpin(nullptr)
    , pwrDomPgoodEdit(nullptr)
    , pwrDomWaitDepSpin(nullptr)
    , pwrDomSettleOnSpin(nullptr)
    , pwrDomSettleOffSpin(nullptr)
{
    setWindowTitle(QString("Configure %1").arg(item->primitiveTypeName()));
    setMinimumWidth(450);

    mainLayout = new QVBoxLayout(this);

    /* Basic information: name */
    auto *infoGroup  = new QGroupBox(tr("Basic Information"), this);
    auto *infoLayout = new QFormLayout(infoGroup);

    nameEdit = new QLineEdit(item->primitiveName(), this);
    infoLayout->addRow(tr("Name:"), nameEdit);

    auto *typeLabel = new QLabel(item->primitiveTypeName(), this);
    infoLayout->addRow(tr("Type:"), typeLabel);

    mainLayout->addWidget(infoGroup);

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
    mainLayout->addWidget(buttonBox);

    setLayout(mainLayout);
}

/* Clock Input Form */
void PrcConfigDialog::createClockInputForm()
{
    const auto &params = std::get<ClockInputParams>(item->params());

    auto *group  = new QGroupBox(tr("Input Settings"), this);
    auto *layout = new QFormLayout(group);

    inputFreqEdit = new QLineEdit(params.freq, this);
    inputFreqEdit->setPlaceholderText("25MHz");
    layout->addRow(tr("Frequency:"), inputFreqEdit);

    mainLayout->addWidget(group);
}

/* Clock Target Form */
void PrcConfigDialog::createClockTargetForm()
{
    const auto &params = std::get<ClockTargetParams>(item->params());

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
    QString autoStaCell    = scene ? scene->getLastStaGuideCell() : QString();
    int     sourceCount    = connectedSources.size();
    bool    muxEnabled     = sourceCount >= 2;

    /* Basic target settings (full width) */
    auto *basicGroup  = new QGroupBox(tr("Target Settings"), this);
    auto *basicLayout = new QFormLayout(basicGroup);

    targetFreqEdit = new QLineEdit(params.freq, this);
    targetFreqEdit->setPlaceholderText("400MHz");
    basicLayout->addRow(tr("Frequency:"), targetFreqEdit);

    basicLayout->addRow(
        tr("Select:"),
        createAutoLineEdit(&targetSelectEdit, params.select, autoSelect, autoSelect, this));

    basicLayout->addRow(
        tr("Reset:"),
        createAutoLineEdit(&targetResetEdit, params.reset, autoReset, autoReset, this));

    basicLayout->addRow(
        tr("Test Clock:"),
        createAutoLineEdit(
            &targetTestClockEdit, params.test_clock, autoTestClock, autoTestClock, this));

    mainLayout->addWidget(basicGroup);

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

    if (!connectedSources.isEmpty()) {
        auto *linksLabel = new QLabel(connectedSources.join(", "), muxGroup);
        linksLabel->setWordWrap(true);
        linksLabel->setStyleSheet("color: #666; font-style: italic;");
        muxLayout->addRow(tr("Connected:"), linksLabel);
    } else {
        auto *noLinksLabel = new QLabel(tr("(connect inputs)"), muxGroup);
        noLinksLabel->setStyleSheet("color: #999; font-style: italic;");
        muxLayout->addRow(noLinksLabel);
    }

    targetMuxStaGroup = new QGroupBox(tr("STA Guide"), muxGroup);
    targetMuxStaGroup->setCheckable(true);
    targetMuxStaGroup->setChecked(params.mux.sta_guide.configured);
    auto *muxStaLayout = new QFormLayout(targetMuxStaGroup);

    auto *muxCellContainer = createAutoLineEdit(
        &targetMuxStaCellEdit,
        params.mux.sta_guide.cell,
        autoStaCell,
        autoStaCell,
        targetMuxStaGroup);
    muxStaLayout->addRow(tr("Cell:"), muxCellContainer);
    connect(targetMuxStaCellEdit, &QLineEdit::editingFinished, this, [this]() {
        if (scene && !targetMuxStaCellEdit->text().isEmpty()) {
            scene->setLastStaGuideCell(targetMuxStaCellEdit->text());
            updateCellPlaceholders();
        }
    });
    if (auto *btn = muxCellContainer->findChild<QPushButton *>()) {
        disconnect(btn, &QPushButton::clicked, nullptr, nullptr);
        connect(btn, &QPushButton::clicked, this, [this]() {
            if (targetMuxStaCellEdit->text().isEmpty() && scene) {
                targetMuxStaCellEdit->setText(scene->getLastStaGuideCell());
            }
        });
    }

    targetMuxStaInEdit = new QLineEdit(params.mux.sta_guide.in, targetMuxStaGroup);
    targetMuxStaInEdit->setPlaceholderText("A");
    muxStaLayout->addRow(tr("In:"), targetMuxStaInEdit);

    targetMuxStaOutEdit = new QLineEdit(params.mux.sta_guide.out, targetMuxStaGroup);
    targetMuxStaOutEdit->setPlaceholderText("X");
    muxStaLayout->addRow(tr("Out:"), targetMuxStaOutEdit);

    muxStaLayout->addRow(
        tr("Instance:"),
        createAutoLineEdit(
            &targetMuxStaInstanceEdit,
            params.mux.sta_guide.instance,
            autoMuxStaInst,
            autoMuxStaInst,
            targetMuxStaGroup));

    muxLayout->addRow(targetMuxStaGroup);
    leftLayout->addWidget(muxGroup);

    /* ICG Group (left column) */
    auto *icgGroup  = new QGroupBox(tr("ICG (Clock Gating)"), leftColumn);
    auto *icgLayout = new QFormLayout(icgGroup);
    icgGroup->setCheckable(true);
    icgGroup->setChecked(params.icg.configured);

    icgLayout->addRow(
        tr("Enable:"),
        createAutoLineEdit(
            &targetIcgEnableEdit, params.icg.enable, autoIcgEnable, autoIcgEnable, icgGroup));

    targetIcgPolarityCombo = new QComboBox(icgGroup);
    targetIcgPolarityCombo->addItems({"high", "low"});
    targetIcgPolarityCombo->setCurrentText(
        params.icg.polarity.isEmpty() ? "high" : params.icg.polarity);
    icgLayout->addRow(tr("Polarity:"), targetIcgPolarityCombo);

    targetIcgClockOnResetCheck = new QCheckBox(tr("Clock on reset"), icgGroup);
    targetIcgClockOnResetCheck->setChecked(params.icg.clock_on_reset);
    icgLayout->addRow(targetIcgClockOnResetCheck);

    targetIcgStaGroup = new QGroupBox(tr("STA Guide"), icgGroup);
    targetIcgStaGroup->setCheckable(true);
    targetIcgStaGroup->setChecked(params.icg.sta_guide.configured);
    auto *icgStaLayout = new QFormLayout(targetIcgStaGroup);

    auto *icgCellContainer = createAutoLineEdit(
        &targetIcgStaCellEdit,
        params.icg.sta_guide.cell,
        autoStaCell,
        autoStaCell,
        targetIcgStaGroup);
    icgStaLayout->addRow(tr("Cell:"), icgCellContainer);
    connect(targetIcgStaCellEdit, &QLineEdit::editingFinished, this, [this]() {
        if (scene && !targetIcgStaCellEdit->text().isEmpty()) {
            scene->setLastStaGuideCell(targetIcgStaCellEdit->text());
            updateCellPlaceholders();
        }
    });
    if (auto *btn = icgCellContainer->findChild<QPushButton *>()) {
        disconnect(btn, &QPushButton::clicked, nullptr, nullptr);
        connect(btn, &QPushButton::clicked, this, [this]() {
            if (targetIcgStaCellEdit->text().isEmpty() && scene) {
                targetIcgStaCellEdit->setText(scene->getLastStaGuideCell());
            }
        });
    }

    targetIcgStaInEdit = new QLineEdit(params.icg.sta_guide.in, targetIcgStaGroup);
    targetIcgStaInEdit->setPlaceholderText("A");
    icgStaLayout->addRow(tr("In:"), targetIcgStaInEdit);

    targetIcgStaOutEdit = new QLineEdit(params.icg.sta_guide.out, targetIcgStaGroup);
    targetIcgStaOutEdit->setPlaceholderText("X");
    icgStaLayout->addRow(tr("Out:"), targetIcgStaOutEdit);

    icgStaLayout->addRow(
        tr("Instance:"),
        createAutoLineEdit(
            &targetIcgStaInstanceEdit,
            params.icg.sta_guide.instance,
            autoIcgStaInst,
            autoIcgStaInst,
            targetIcgStaGroup));

    icgLayout->addRow(targetIcgStaGroup);
    leftLayout->addWidget(icgGroup);
    leftLayout->addStretch();

    /* DIV Group (right column) */
    auto *divGroup  = new QGroupBox(tr("DIV (Clock Divider)"), rightColumn);
    auto *divLayout = new QFormLayout(divGroup);
    divGroup->setCheckable(true);
    divGroup->setChecked(params.div.configured);

    targetDivDefaultSpin = new QSpinBox(divGroup);
    targetDivDefaultSpin->setRange(1, 65535);
    targetDivDefaultSpin->setValue(params.div.default_value);
    divLayout->addRow(tr("Default:"), targetDivDefaultSpin);

    divLayout->addRow(
        tr("Value:"),
        createAutoLineEdit(
            &targetDivValueEdit, params.div.value, autoDivValue, autoDivValue, divGroup));

    targetDivWidthSpin = new QSpinBox(divGroup);
    targetDivWidthSpin->setRange(0, 32);
    targetDivWidthSpin->setValue(params.div.width);
    targetDivWidthSpin->setSpecialValueText("auto");
    divLayout->addRow(tr("Width:"), targetDivWidthSpin);

    divLayout->addRow(
        tr("Reset:"),
        createAutoLineEdit(&targetDivResetEdit, params.div.reset, autoReset, autoReset, divGroup));

    targetDivClockOnResetCheck = new QCheckBox(tr("Clock on reset"), divGroup);
    targetDivClockOnResetCheck->setChecked(params.div.clock_on_reset);
    divLayout->addRow(targetDivClockOnResetCheck);

    targetDivStaGroup = new QGroupBox(tr("STA Guide"), divGroup);
    targetDivStaGroup->setCheckable(true);
    targetDivStaGroup->setChecked(params.div.sta_guide.configured);
    auto *divStaLayout = new QFormLayout(targetDivStaGroup);

    auto *divCellContainer = createAutoLineEdit(
        &targetDivStaCellEdit,
        params.div.sta_guide.cell,
        autoStaCell,
        autoStaCell,
        targetDivStaGroup);
    divStaLayout->addRow(tr("Cell:"), divCellContainer);
    connect(targetDivStaCellEdit, &QLineEdit::editingFinished, this, [this]() {
        if (scene && !targetDivStaCellEdit->text().isEmpty()) {
            scene->setLastStaGuideCell(targetDivStaCellEdit->text());
            updateCellPlaceholders();
        }
    });
    if (auto *btn = divCellContainer->findChild<QPushButton *>()) {
        disconnect(btn, &QPushButton::clicked, nullptr, nullptr);
        connect(btn, &QPushButton::clicked, this, [this]() {
            if (targetDivStaCellEdit->text().isEmpty() && scene) {
                targetDivStaCellEdit->setText(scene->getLastStaGuideCell());
            }
        });
    }

    targetDivStaInEdit = new QLineEdit(params.div.sta_guide.in, targetDivStaGroup);
    targetDivStaInEdit->setPlaceholderText("A");
    divStaLayout->addRow(tr("In:"), targetDivStaInEdit);

    targetDivStaOutEdit = new QLineEdit(params.div.sta_guide.out, targetDivStaGroup);
    targetDivStaOutEdit->setPlaceholderText("X");
    divStaLayout->addRow(tr("Out:"), targetDivStaOutEdit);

    divStaLayout->addRow(
        tr("Instance:"),
        createAutoLineEdit(
            &targetDivStaInstanceEdit,
            params.div.sta_guide.instance,
            autoDivStaInst,
            autoDivStaInst,
            targetDivStaGroup));

    divLayout->addRow(targetDivStaGroup);
    rightLayout->addWidget(divGroup);

    /* INV Group (right column) */
    auto *invGroup  = new QGroupBox(tr("INV (Clock Inverter)"), rightColumn);
    auto *invLayout = new QFormLayout(invGroup);
    invGroup->setCheckable(true);
    invGroup->setChecked(params.inv.configured);

    targetInvStaGroup = new QGroupBox(tr("STA Guide"), invGroup);
    targetInvStaGroup->setCheckable(true);
    targetInvStaGroup->setChecked(params.inv.sta_guide.configured);
    auto *invStaLayout = new QFormLayout(targetInvStaGroup);

    auto *invCellContainer = createAutoLineEdit(
        &targetInvStaCellEdit,
        params.inv.sta_guide.cell,
        autoStaCell,
        autoStaCell,
        targetInvStaGroup);
    invStaLayout->addRow(tr("Cell:"), invCellContainer);
    connect(targetInvStaCellEdit, &QLineEdit::editingFinished, this, [this]() {
        if (scene && !targetInvStaCellEdit->text().isEmpty()) {
            scene->setLastStaGuideCell(targetInvStaCellEdit->text());
            updateCellPlaceholders();
        }
    });
    if (auto *btn = invCellContainer->findChild<QPushButton *>()) {
        disconnect(btn, &QPushButton::clicked, nullptr, nullptr);
        connect(btn, &QPushButton::clicked, this, [this]() {
            if (targetInvStaCellEdit->text().isEmpty() && scene) {
                targetInvStaCellEdit->setText(scene->getLastStaGuideCell());
            }
        });
    }

    targetInvStaInEdit = new QLineEdit(params.inv.sta_guide.in, targetInvStaGroup);
    targetInvStaInEdit->setPlaceholderText("A");
    invStaLayout->addRow(tr("In:"), targetInvStaInEdit);

    targetInvStaOutEdit = new QLineEdit(params.inv.sta_guide.out, targetInvStaGroup);
    targetInvStaOutEdit->setPlaceholderText("X");
    invStaLayout->addRow(tr("Out:"), targetInvStaOutEdit);

    invStaLayout->addRow(
        tr("Instance:"),
        createAutoLineEdit(
            &targetInvStaInstanceEdit,
            params.inv.sta_guide.instance,
            autoInvStaInst,
            autoInvStaInst,
            targetInvStaGroup));

    invLayout->addRow(targetInvStaGroup);
    rightLayout->addWidget(invGroup);
    rightLayout->addStretch();

    columnsLayout->addWidget(leftColumn);
    columnsLayout->addWidget(rightColumn);
    mainLayout->addWidget(columnsWidget);

    /* Store group pointers for apply */
    targetMuxCheck = reinterpret_cast<QCheckBox *>(muxGroup);
    targetIcgCheck = reinterpret_cast<QCheckBox *>(icgGroup);
    targetDivCheck = reinterpret_cast<QCheckBox *>(divGroup);
    targetInvCheck = reinterpret_cast<QCheckBox *>(invGroup);
}

/* Reset Source Form */
void PrcConfigDialog::createResetSourceForm()
{
    const auto &params = std::get<ResetSourceParams>(item->params());

    auto *group  = new QGroupBox(tr("Source Settings"), this);
    auto *layout = new QFormLayout(group);

    rstSrcActiveCombo = new QComboBox(this);
    rstSrcActiveCombo->addItems({"low", "high"});
    rstSrcActiveCombo->setCurrentText(params.active.isEmpty() ? "low" : params.active);
    layout->addRow(tr("Active Level:"), rstSrcActiveCombo);

    mainLayout->addWidget(group);
}

/* Reset Target Form */
void PrcConfigDialog::createResetTargetForm()
{
    const auto &params = std::get<ResetTargetParams>(item->params());

    auto *basicGroup  = new QGroupBox(tr("Target Settings"), this);
    auto *basicLayout = new QFormLayout(basicGroup);

    rstTgtActiveCombo = new QComboBox(this);
    rstTgtActiveCombo->addItems({"low", "high"});
    rstTgtActiveCombo->setCurrentText(params.active.isEmpty() ? "low" : params.active);
    basicLayout->addRow(tr("Active Level:"), rstTgtActiveCombo);

    mainLayout->addWidget(basicGroup);

    /* Async synchronizer settings */
    auto *asyncGroup  = new QGroupBox(tr("Async Synchronizer (qsoc_rst_sync)"), this);
    auto *asyncLayout = new QFormLayout(asyncGroup);
    asyncGroup->setCheckable(true);
    asyncGroup->setChecked(params.sync.async_configured);

    rstTgtAsyncClockEdit = new QLineEdit(params.sync.async_clock, this);
    rstTgtAsyncClockEdit->setPlaceholderText("clk_sys");
    asyncLayout->addRow(tr("Clock:"), rstTgtAsyncClockEdit);

    rstTgtAsyncStageSpin = new QSpinBox(this);
    rstTgtAsyncStageSpin->setRange(2, 8);
    rstTgtAsyncStageSpin->setValue(params.sync.async_stage);
    asyncLayout->addRow(tr("Stages:"), rstTgtAsyncStageSpin);

    rstTgtAsyncCheck = nullptr; /* Use group checkbox */
    connect(asyncGroup, &QGroupBox::toggled, [this, asyncLayout](bool checked) {
        for (int i = 0; i < asyncLayout->count(); ++i) {
            if (auto *widget = asyncLayout->itemAt(i)->widget()) {
                widget->setEnabled(checked);
            }
        }
    });
    asyncGroup->toggled(params.sync.async_configured);

    mainLayout->addWidget(asyncGroup);

    rstTgtAsyncCheck = reinterpret_cast<QCheckBox *>(asyncGroup);
}

/* Power Domain Form */
void PrcConfigDialog::createPowerDomainForm()
{
    const auto &params = std::get<PowerDomainParams>(item->params());

    auto *group  = new QGroupBox(tr("Domain Settings"), this);
    auto *layout = new QFormLayout(group);

    pwrDomVoltageSpin = new QSpinBox(this);
    pwrDomVoltageSpin->setRange(100, 5000);
    pwrDomVoltageSpin->setSuffix(" mV");
    pwrDomVoltageSpin->setValue(params.v_mv);
    layout->addRow(tr("Voltage:"), pwrDomVoltageSpin);

    pwrDomPgoodEdit = new QLineEdit(params.pgood, this);
    pwrDomPgoodEdit->setPlaceholderText("pgood_xxx");
    layout->addRow(tr("Power Good:"), pwrDomPgoodEdit);

    pwrDomWaitDepSpin = new QSpinBox(this);
    pwrDomWaitDepSpin->setRange(0, 65535);
    pwrDomWaitDepSpin->setValue(params.wait_dep);
    layout->addRow(tr("Wait Dep Cycles:"), pwrDomWaitDepSpin);

    pwrDomSettleOnSpin = new QSpinBox(this);
    pwrDomSettleOnSpin->setRange(0, 65535);
    pwrDomSettleOnSpin->setValue(params.settle_on);
    layout->addRow(tr("Settle On Cycles:"), pwrDomSettleOnSpin);

    pwrDomSettleOffSpin = new QSpinBox(this);
    pwrDomSettleOffSpin->setRange(0, 65535);
    pwrDomSettleOffSpin->setValue(params.settle_off);
    layout->addRow(tr("Settle Off Cycles:"), pwrDomSettleOffSpin);

    mainLayout->addWidget(group);
}

/* Controller Selection Group */
void PrcConfigDialog::createControllerGroup()
{
    auto *group  = new QGroupBox(tr("Controller Assignment"), this);
    auto *layout = new QHBoxLayout(group);

    controllerCombo = new QComboBox(this);
    controllerCombo->setMinimumWidth(200);
    populateControllerCombo();

    editControllerBtn = new QPushButton(tr("Edit..."), this);
    editControllerBtn->setToolTip(tr("Edit controller settings"));

    layout->addWidget(controllerCombo, 1);
    layout->addWidget(editControllerBtn);

    connect(
        controllerCombo,
        QOverload<int>::of(&QComboBox::currentIndexChanged),
        this,
        &PrcConfigDialog::onControllerChanged);
    connect(editControllerBtn, &QPushButton::clicked, this, &PrcConfigDialog::onEditControllerClicked);

    mainLayout->addWidget(group);
}

void PrcConfigDialog::populateControllerCombo()
{
    if (!controllerCombo) {
        return;
    }

    controllerCombo->clear();

    /* Get current controller name from params */
    QString     currentController;
    const auto &params = item->params();

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

    switch (item->primitiveType()) {
    case ClockInput:
    case ClockTarget:
        if (scene) {
            controllerNames = scene->clockControllerNames();
        }
        defaultName = "clock_ctrl";
        break;
    case ResetSource:
    case ResetTarget:
        if (scene) {
            controllerNames = scene->resetControllerNames();
        }
        defaultName = "reset_ctrl";
        break;
    case PowerDomain:
        if (scene) {
            controllerNames = scene->powerControllerNames();
        }
        defaultName = "power_ctrl";
        break;
    }

    /* Add controllers to combo */
    for (const QString &name : controllerNames) {
        controllerCombo->addItem(name, name);
    }

    /* Add separator and "New Controller..." option */
    if (!controllerNames.isEmpty()) {
        controllerCombo->insertSeparator(controllerCombo->count());
    }
    controllerCombo->addItem(tr("New Controller..."), QVariant("__new__"));

    /* Select current controller or add if not in list */
    if (!currentController.isEmpty()) {
        int idx = controllerCombo->findData(currentController);
        if (idx >= 0) {
            controllerCombo->setCurrentIndex(idx);
        } else {
            /* Add current controller to list if not found */
            controllerCombo->insertItem(0, currentController, currentController);
            controllerCombo->setCurrentIndex(0);
        }
    } else if (!controllerNames.isEmpty()) {
        controllerCombo->setCurrentIndex(0);
    } else {
        /* No controllers exist - add default */
        controllerCombo->insertItem(0, defaultName, defaultName);
        controllerCombo->setCurrentIndex(0);
    }
}

void PrcConfigDialog::onControllerChanged(int index)
{
    if (!controllerCombo || index < 0) {
        return;
    }

    QString value = controllerCombo->currentData().toString();

    if (value == "__new__") {
        /* Create new controller - show input dialog */
        QString defaultName;
        switch (item->primitiveType()) {
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
            if (scene) {
                switch (item->primitiveType()) {
                case ClockInput:
                case ClockTarget: {
                    ClockControllerDef def;
                    def.name = name;
                    scene->setClockController(name, def);
                    break;
                }
                case ResetSource:
                case ResetTarget: {
                    ResetControllerDef def;
                    def.name = name;
                    scene->setResetController(name, def);
                    break;
                }
                case PowerDomain: {
                    PowerControllerDef def;
                    def.name = name;
                    scene->setPowerController(name, def);
                    break;
                }
                }
            }

            /* Repopulate and select new controller */
            populateControllerCombo();
            int idx = controllerCombo->findData(name);
            if (idx >= 0) {
                controllerCombo->setCurrentIndex(idx);
            }
        } else {
            /* User cancelled - reset to first item */
            if (controllerCombo->count() > 1) {
                controllerCombo->setCurrentIndex(0);
            }
        }
    }
}

void PrcConfigDialog::onEditControllerClicked()
{
    if (!controllerCombo || !scene) {
        return;
    }

    QString controllerName = controllerCombo->currentData().toString();
    if (controllerName.isEmpty() || controllerName == "__new__") {
        return;
    }

    /* Determine controller type based on primitive type */
    PrcControllerDialog::ControllerType ctrlType;
    switch (item->primitiveType()) {
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
    PrcControllerDialog dialog(ctrlType, controllerName, scene, this);

    /* Handle delete request */
    connect(&dialog, &PrcControllerDialog::deleteRequested, [this, controllerName, ctrlType]() {
        switch (ctrlType) {
        case PrcControllerDialog::ClockController:
            scene->removeClockController(controllerName);
            break;
        case PrcControllerDialog::ResetController:
            scene->removeResetController(controllerName);
            break;
        case PrcControllerDialog::PowerController:
            scene->removePowerController(controllerName);
            break;
        }
        populateControllerCombo();
    });

    if (dialog.exec() == QDialog::Accepted) {
        /* Apply controller changes */
        switch (ctrlType) {
        case PrcControllerDialog::ClockController:
            scene->setClockController(controllerName, dialog.getClockControllerDef());
            break;
        case PrcControllerDialog::ResetController:
            scene->setResetController(controllerName, dialog.getResetControllerDef());
            break;
        case PrcControllerDialog::PowerController:
            scene->setPowerController(controllerName, dialog.getPowerControllerDef());
            break;
        }
    }
}

/* Update Cell Placeholders */
void PrcConfigDialog::updateCellPlaceholders()
{
    QString cell = scene ? scene->getLastStaGuideCell() : QString();
    if (targetMuxStaCellEdit) {
        targetMuxStaCellEdit->setPlaceholderText(cell);
    }
    if (targetIcgStaCellEdit) {
        targetIcgStaCellEdit->setPlaceholderText(cell);
    }
    if (targetDivStaCellEdit) {
        targetDivStaCellEdit->setPlaceholderText(cell);
    }
    if (targetInvStaCellEdit) {
        targetInvStaCellEdit->setPlaceholderText(cell);
    }
}

/* Apply Configuration */
void PrcConfigDialog::applyConfiguration()
{
    /* Update primitive name */
    item->setPrimitiveName(nameEdit->text());

    /* Get selected controller name */
    QString selectedController;
    if (controllerCombo) {
        QString data = controllerCombo->currentData().toString();
        if (data != "__new__") {
            selectedController = data;
        }
    }

    /* Update type-specific parameters */
    switch (item->primitiveType()) {
    case ClockInput: {
        ClockInputParams params;
        params.name       = nameEdit->text();
        params.freq       = inputFreqEdit->text();
        params.controller = selectedController;
        item->setParams(params);
        break;
    }
    case ClockTarget: {
        ClockTargetParams params;
        params.name       = nameEdit->text();
        params.freq       = targetFreqEdit->text();
        params.controller = selectedController;
        params.select     = targetSelectEdit->text();
        params.reset      = targetResetEdit->text();
        params.test_clock = targetTestClockEdit->text();

        /* MUX - auto-determined by connection count */
        params.mux.configured = connectedSources.size() >= 2;
        if (targetMuxStaGroup) {
            params.mux.sta_guide.configured = targetMuxStaGroup->isChecked();
            if (params.mux.sta_guide.configured) {
                params.mux.sta_guide.cell     = targetMuxStaCellEdit->text();
                params.mux.sta_guide.in       = targetMuxStaInEdit->text();
                params.mux.sta_guide.out      = targetMuxStaOutEdit->text();
                params.mux.sta_guide.instance = targetMuxStaInstanceEdit->text();
            }
        }

        /* ICG */
        auto *icgGroup        = reinterpret_cast<QGroupBox *>(targetIcgCheck);
        params.icg.configured = icgGroup ? icgGroup->isChecked() : false;
        if (params.icg.configured) {
            params.icg.enable         = targetIcgEnableEdit->text();
            params.icg.polarity       = targetIcgPolarityCombo->currentText();
            params.icg.clock_on_reset = targetIcgClockOnResetCheck->isChecked();
        }
        if (targetIcgStaGroup) {
            params.icg.sta_guide.configured = targetIcgStaGroup->isChecked();
            if (params.icg.sta_guide.configured) {
                params.icg.sta_guide.cell     = targetIcgStaCellEdit->text();
                params.icg.sta_guide.in       = targetIcgStaInEdit->text();
                params.icg.sta_guide.out      = targetIcgStaOutEdit->text();
                params.icg.sta_guide.instance = targetIcgStaInstanceEdit->text();
            }
        }

        /* DIV */
        auto *divGroup        = reinterpret_cast<QGroupBox *>(targetDivCheck);
        params.div.configured = divGroup ? divGroup->isChecked() : false;
        if (params.div.configured) {
            params.div.default_value  = targetDivDefaultSpin->value();
            params.div.value          = targetDivValueEdit->text();
            params.div.width          = targetDivWidthSpin->value();
            params.div.reset          = targetDivResetEdit->text();
            params.div.clock_on_reset = targetDivClockOnResetCheck->isChecked();
        }
        if (targetDivStaGroup) {
            params.div.sta_guide.configured = targetDivStaGroup->isChecked();
            if (params.div.sta_guide.configured) {
                params.div.sta_guide.cell     = targetDivStaCellEdit->text();
                params.div.sta_guide.in       = targetDivStaInEdit->text();
                params.div.sta_guide.out      = targetDivStaOutEdit->text();
                params.div.sta_guide.instance = targetDivStaInstanceEdit->text();
            }
        }

        /* INV */
        auto *invGroup        = reinterpret_cast<QGroupBox *>(targetInvCheck);
        params.inv.configured = invGroup ? invGroup->isChecked() : false;
        if (targetInvStaGroup) {
            params.inv.sta_guide.configured = targetInvStaGroup->isChecked();
            if (params.inv.sta_guide.configured) {
                params.inv.sta_guide.cell     = targetInvStaCellEdit->text();
                params.inv.sta_guide.in       = targetInvStaInEdit->text();
                params.inv.sta_guide.out      = targetInvStaOutEdit->text();
                params.inv.sta_guide.instance = targetInvStaInstanceEdit->text();
            }
        }

        item->setParams(params);

        /* Save last non-empty STA Guide Cell to scene for session memory */
        if (scene) {
            QString lastCell;
            if (params.mux.sta_guide.configured && !params.mux.sta_guide.cell.isEmpty()) {
                lastCell = params.mux.sta_guide.cell;
            }
            if (params.icg.sta_guide.configured && !params.icg.sta_guide.cell.isEmpty()) {
                lastCell = params.icg.sta_guide.cell;
            }
            if (params.div.sta_guide.configured && !params.div.sta_guide.cell.isEmpty()) {
                lastCell = params.div.sta_guide.cell;
            }
            if (params.inv.sta_guide.configured && !params.inv.sta_guide.cell.isEmpty()) {
                lastCell = params.inv.sta_guide.cell;
            }
            if (!lastCell.isEmpty()) {
                scene->setLastStaGuideCell(lastCell);
            }
        }
        break;
    }
    case ResetSource: {
        ResetSourceParams params;
        params.name       = nameEdit->text();
        params.active     = rstSrcActiveCombo->currentText();
        params.controller = selectedController;
        item->setParams(params);
        break;
    }
    case ResetTarget: {
        ResetTargetParams params;
        params.name       = nameEdit->text();
        params.active     = rstTgtActiveCombo->currentText();
        params.controller = selectedController;

        auto *asyncGroup             = reinterpret_cast<QGroupBox *>(rstTgtAsyncCheck);
        params.sync.async_configured = asyncGroup ? asyncGroup->isChecked() : false;
        if (params.sync.async_configured) {
            params.sync.async_clock = rstTgtAsyncClockEdit->text();
            params.sync.async_stage = rstTgtAsyncStageSpin->value();
        }

        item->setParams(params);
        break;
    }
    case PowerDomain: {
        PowerDomainParams params;
        params.name       = nameEdit->text();
        params.controller = selectedController;
        params.v_mv       = pwrDomVoltageSpin->value();
        params.pgood      = pwrDomPgoodEdit->text();
        params.wait_dep   = pwrDomWaitDepSpin->value();
        params.settle_on  = pwrDomSettleOnSpin->value();
        params.settle_off = pwrDomSettleOffSpin->value();
        /* TODO: dependencies and follow entries via separate UI */
        item->setParams(params);
        break;
    }
    }
}

/* Link Config Dialog */
PrcLinkConfigDialog::PrcLinkConfigDialog(
    const QString         &sourceName,
    const QString         &targetName,
    const ClockLinkParams &linkParams,
    PrcScene              *scene,
    QWidget               *parent)
    : QDialog(parent)
    , sourceName(sourceName)
    , targetName(targetName)
    , linkParams(linkParams)
    , scene(scene)
    , mainLayout(nullptr)
    , icgGroup(nullptr)
    , icgEnableEdit(nullptr)
    , icgPolarityCombo(nullptr)
    , icgTestEnableEdit(nullptr)
    , icgResetEdit(nullptr)
    , icgClockOnResetCheck(nullptr)
    , icgStaGuideGroup(nullptr)
    , icgStaCellEdit(nullptr)
    , icgStaInEdit(nullptr)
    , icgStaOutEdit(nullptr)
    , icgStaInstanceEdit(nullptr)
    , divGroup(nullptr)
    , divDefaultSpin(nullptr)
    , divValueEdit(nullptr)
    , divWidthSpin(nullptr)
    , divResetEdit(nullptr)
    , divClockOnResetCheck(nullptr)
    , divStaGuideGroup(nullptr)
    , divStaCellEdit(nullptr)
    , divStaInEdit(nullptr)
    , divStaOutEdit(nullptr)
    , divStaInstanceEdit(nullptr)
    , invGroup(nullptr)
    , invStaGuideGroup(nullptr)
    , invStaCellEdit(nullptr)
    , invStaInEdit(nullptr)
    , invStaOutEdit(nullptr)
    , invStaInstanceEdit(nullptr)
    , linkStaGuideGroup(nullptr)
    , linkStaCellEdit(nullptr)
    , linkStaInEdit(nullptr)
    , linkStaOutEdit(nullptr)
    , linkStaInstanceEdit(nullptr)
{
    setWindowTitle(QString("Configure Link: %1 -> %2").arg(sourceName).arg(targetName));
    setMinimumWidth(500);

    mainLayout = new QVBoxLayout(this);

    /* Header info */
    auto *infoGroup  = new QGroupBox(tr("Link Information"), this);
    auto *infoLayout = new QFormLayout(infoGroup);

    auto *sourceLabel = new QLabel(sourceName, this);
    infoLayout->addRow(tr("Source:"), sourceLabel);

    auto *targetLabel = new QLabel(targetName, this);
    infoLayout->addRow(tr("Target:"), targetLabel);

    mainLayout->addWidget(infoGroup);

    /* Create form widgets */
    createForm();

    /* Dialog buttons */
    auto *buttonBox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttonBox, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);
    mainLayout->addWidget(buttonBox);

    setLayout(mainLayout);
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
    QString baseName = targetName;
    if (baseName.startsWith("clk_")) {
        baseName = baseName.mid(4);
    }
    QString autoIcgEnable      = targetName + "_en";
    QString autoReset          = "rst_" + baseName + "_n";
    QString autoDivValue       = targetName + "_div";
    QString autoIcgStaInstance = "u_DONTTOUCH_" + targetName + "_icg";
    QString autoDivStaInstance = "u_DONTTOUCH_" + targetName;
    QString autoInvStaInstance = "u_DONTTOUCH_" + targetName + "_inv";
    QString autoLinkInstance   = "u_DONTTOUCH_" + targetName + "_link";
    QString autoStaCell        = scene ? scene->getLastStaGuideCell() : QString();

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
    icgGroup = new QGroupBox(tr("ICG (Clock Gating)"), leftColumn);
    icgGroup->setCheckable(true);
    icgGroup->setChecked(linkParams.icg.configured);

    auto *icgLayout = new QFormLayout(icgGroup);

    icgLayout->addRow(
        tr("Enable:"),
        createAutoLineEdit(
            &icgEnableEdit, linkParams.icg.enable, autoIcgEnable, autoIcgEnable, icgGroup));

    icgPolarityCombo = new QComboBox(icgGroup);
    icgPolarityCombo->addItems({"high", "low"});
    icgPolarityCombo->setCurrentText(
        linkParams.icg.polarity.isEmpty() ? "high" : linkParams.icg.polarity);
    icgLayout->addRow(tr("Polarity:"), icgPolarityCombo);

    icgTestEnableEdit = new QLineEdit(linkParams.icg.test_enable, icgGroup);
    icgTestEnableEdit->setPlaceholderText("test_en");
    icgLayout->addRow(tr("Test Enable:"), icgTestEnableEdit);

    icgLayout->addRow(
        tr("Reset:"),
        createAutoLineEdit(&icgResetEdit, linkParams.icg.reset, autoReset, autoReset, icgGroup));

    icgClockOnResetCheck = new QCheckBox(tr("Clock on reset"), icgGroup);
    icgClockOnResetCheck->setChecked(linkParams.icg.clock_on_reset);
    icgLayout->addRow(icgClockOnResetCheck);

    /* ICG STA Guide */
    icgStaGuideGroup = new QGroupBox(tr("STA Guide"), icgGroup);
    icgStaGuideGroup->setCheckable(true);
    icgStaGuideGroup->setChecked(linkParams.icg.sta_guide.configured);

    auto *icgStaLayout = new QFormLayout(icgStaGuideGroup);

    auto *icgCellContainer = createAutoLineEdit(
        &icgStaCellEdit, linkParams.icg.sta_guide.cell, autoStaCell, autoStaCell, icgStaGuideGroup);
    icgStaLayout->addRow(tr("Cell:"), icgCellContainer);
    connect(icgStaCellEdit, &QLineEdit::editingFinished, this, [this]() {
        if (scene && !icgStaCellEdit->text().isEmpty()) {
            scene->setLastStaGuideCell(icgStaCellEdit->text());
            updateCellPlaceholders();
        }
    });
    if (auto *btn = icgCellContainer->findChild<QPushButton *>()) {
        disconnect(btn, &QPushButton::clicked, nullptr, nullptr);
        connect(btn, &QPushButton::clicked, this, [this]() {
            if (icgStaCellEdit->text().isEmpty() && scene) {
                icgStaCellEdit->setText(scene->getLastStaGuideCell());
            }
        });
    }

    icgStaInEdit = new QLineEdit(linkParams.icg.sta_guide.in, icgStaGuideGroup);
    icgStaInEdit->setPlaceholderText("A");
    icgStaLayout->addRow(tr("In:"), icgStaInEdit);

    icgStaOutEdit = new QLineEdit(linkParams.icg.sta_guide.out, icgStaGuideGroup);
    icgStaOutEdit->setPlaceholderText("X");
    icgStaLayout->addRow(tr("Out:"), icgStaOutEdit);

    icgStaLayout->addRow(
        tr("Instance:"),
        createAutoLineEdit(
            &icgStaInstanceEdit,
            linkParams.icg.sta_guide.instance,
            autoIcgStaInstance,
            autoIcgStaInstance,
            icgStaGuideGroup));

    icgLayout->addRow(icgStaGuideGroup);
    leftLayout->addWidget(icgGroup);

    /* DIV Group */
    divGroup = new QGroupBox(tr("DIV (Clock Divider)"), leftColumn);
    divGroup->setCheckable(true);
    divGroup->setChecked(linkParams.div.configured);

    auto *divLayout = new QFormLayout(divGroup);

    divDefaultSpin = new QSpinBox(divGroup);
    divDefaultSpin->setRange(1, 65535);
    divDefaultSpin->setValue(linkParams.div.default_value);
    divLayout->addRow(tr("Default:"), divDefaultSpin);

    divLayout->addRow(
        tr("Value:"),
        createAutoLineEdit(&divValueEdit, linkParams.div.value, autoDivValue, autoDivValue, divGroup));

    divWidthSpin = new QSpinBox(divGroup);
    divWidthSpin->setRange(0, 32);
    divWidthSpin->setValue(linkParams.div.width);
    divWidthSpin->setSpecialValueText("auto");
    divLayout->addRow(tr("Width:"), divWidthSpin);

    divLayout->addRow(
        tr("Reset:"),
        createAutoLineEdit(&divResetEdit, linkParams.div.reset, autoReset, autoReset, divGroup));

    divClockOnResetCheck = new QCheckBox(tr("Clock on reset"), divGroup);
    divClockOnResetCheck->setChecked(linkParams.div.clock_on_reset);
    divLayout->addRow(divClockOnResetCheck);

    /* DIV STA Guide */
    divStaGuideGroup = new QGroupBox(tr("STA Guide"), divGroup);
    divStaGuideGroup->setCheckable(true);
    divStaGuideGroup->setChecked(linkParams.div.sta_guide.configured);

    auto *divStaLayout = new QFormLayout(divStaGuideGroup);

    auto *divCellContainer = createAutoLineEdit(
        &divStaCellEdit, linkParams.div.sta_guide.cell, autoStaCell, autoStaCell, divStaGuideGroup);
    divStaLayout->addRow(tr("Cell:"), divCellContainer);
    connect(divStaCellEdit, &QLineEdit::editingFinished, this, [this]() {
        if (scene && !divStaCellEdit->text().isEmpty()) {
            scene->setLastStaGuideCell(divStaCellEdit->text());
            updateCellPlaceholders();
        }
    });
    if (auto *btn = divCellContainer->findChild<QPushButton *>()) {
        disconnect(btn, &QPushButton::clicked, nullptr, nullptr);
        connect(btn, &QPushButton::clicked, this, [this]() {
            if (divStaCellEdit->text().isEmpty() && scene) {
                divStaCellEdit->setText(scene->getLastStaGuideCell());
            }
        });
    }

    divStaInEdit = new QLineEdit(linkParams.div.sta_guide.in, divStaGuideGroup);
    divStaInEdit->setPlaceholderText("A");
    divStaLayout->addRow(tr("In:"), divStaInEdit);

    divStaOutEdit = new QLineEdit(linkParams.div.sta_guide.out, divStaGuideGroup);
    divStaOutEdit->setPlaceholderText("X");
    divStaLayout->addRow(tr("Out:"), divStaOutEdit);

    divStaLayout->addRow(
        tr("Instance:"),
        createAutoLineEdit(
            &divStaInstanceEdit,
            linkParams.div.sta_guide.instance,
            autoDivStaInstance,
            autoDivStaInstance,
            divStaGuideGroup));

    divLayout->addRow(divStaGuideGroup);
    leftLayout->addWidget(divGroup);
    leftLayout->addStretch();

    /* INV Group */
    invGroup = new QGroupBox(tr("INV (Clock Inverter)"), rightColumn);
    invGroup->setCheckable(true);
    invGroup->setChecked(linkParams.inv.configured);

    auto *invLayout = new QFormLayout(invGroup);

    /* INV STA Guide */
    invStaGuideGroup = new QGroupBox(tr("STA Guide"), invGroup);
    invStaGuideGroup->setCheckable(true);
    invStaGuideGroup->setChecked(linkParams.inv.sta_guide.configured);

    auto *invStaLayout = new QFormLayout(invStaGuideGroup);

    auto *invCellContainer = createAutoLineEdit(
        &invStaCellEdit, linkParams.inv.sta_guide.cell, autoStaCell, autoStaCell, invStaGuideGroup);
    invStaLayout->addRow(tr("Cell:"), invCellContainer);
    connect(invStaCellEdit, &QLineEdit::editingFinished, this, [this]() {
        if (scene && !invStaCellEdit->text().isEmpty()) {
            scene->setLastStaGuideCell(invStaCellEdit->text());
            updateCellPlaceholders();
        }
    });
    if (auto *btn = invCellContainer->findChild<QPushButton *>()) {
        disconnect(btn, &QPushButton::clicked, nullptr, nullptr);
        connect(btn, &QPushButton::clicked, this, [this]() {
            if (invStaCellEdit->text().isEmpty() && scene) {
                invStaCellEdit->setText(scene->getLastStaGuideCell());
            }
        });
    }

    invStaInEdit = new QLineEdit(linkParams.inv.sta_guide.in, invStaGuideGroup);
    invStaInEdit->setPlaceholderText("A");
    invStaLayout->addRow(tr("In:"), invStaInEdit);

    invStaOutEdit = new QLineEdit(linkParams.inv.sta_guide.out, invStaGuideGroup);
    invStaOutEdit->setPlaceholderText("X");
    invStaLayout->addRow(tr("Out:"), invStaOutEdit);

    invStaLayout->addRow(
        tr("Instance:"),
        createAutoLineEdit(
            &invStaInstanceEdit,
            linkParams.inv.sta_guide.instance,
            autoInvStaInstance,
            autoInvStaInstance,
            invStaGuideGroup));

    invLayout->addRow(invStaGuideGroup);
    rightLayout->addWidget(invGroup);

    /* Link-level STA Guide */
    linkStaGuideGroup = new QGroupBox(tr("Link STA Guide"), rightColumn);
    linkStaGuideGroup->setCheckable(true);
    linkStaGuideGroup->setChecked(linkParams.sta_guide.configured);

    auto *linkStaLayout     = new QFormLayout(linkStaGuideGroup);
    auto *linkCellContainer = createAutoLineEdit(
        &linkStaCellEdit, linkParams.sta_guide.cell, autoStaCell, autoStaCell, linkStaGuideGroup);
    linkStaLayout->addRow(tr("Cell:"), linkCellContainer);
    connect(linkStaCellEdit, &QLineEdit::editingFinished, this, [this]() {
        if (scene && !linkStaCellEdit->text().isEmpty()) {
            scene->setLastStaGuideCell(linkStaCellEdit->text());
            updateCellPlaceholders();
        }
    });
    if (auto *btn = linkCellContainer->findChild<QPushButton *>()) {
        disconnect(btn, &QPushButton::clicked, nullptr, nullptr);
        connect(btn, &QPushButton::clicked, this, [this]() {
            if (linkStaCellEdit->text().isEmpty() && scene) {
                linkStaCellEdit->setText(scene->getLastStaGuideCell());
            }
        });
    }

    linkStaInEdit = new QLineEdit(linkParams.sta_guide.in, linkStaGuideGroup);
    linkStaInEdit->setPlaceholderText("A");
    linkStaLayout->addRow(tr("In:"), linkStaInEdit);

    linkStaOutEdit = new QLineEdit(linkParams.sta_guide.out, linkStaGuideGroup);
    linkStaOutEdit->setPlaceholderText("X");
    linkStaLayout->addRow(tr("Out:"), linkStaOutEdit);

    linkStaLayout->addRow(
        tr("Instance:"),
        createAutoLineEdit(
            &linkStaInstanceEdit,
            linkParams.sta_guide.instance,
            autoLinkInstance,
            autoLinkInstance,
            linkStaGuideGroup));

    rightLayout->addWidget(linkStaGuideGroup);
    rightLayout->addStretch();

    columnsLayout->addWidget(leftColumn);
    columnsLayout->addWidget(rightColumn);
    mainLayout->addWidget(columnsWidget);
}

/* Update Cell Placeholders */
void PrcLinkConfigDialog::updateCellPlaceholders()
{
    QString cell = scene ? scene->getLastStaGuideCell() : QString();
    if (icgStaCellEdit) {
        icgStaCellEdit->setPlaceholderText(cell);
    }
    if (divStaCellEdit) {
        divStaCellEdit->setPlaceholderText(cell);
    }
    if (invStaCellEdit) {
        invStaCellEdit->setPlaceholderText(cell);
    }
    if (linkStaCellEdit) {
        linkStaCellEdit->setPlaceholderText(cell);
    }
}

ClockLinkParams PrcLinkConfigDialog::getLinkParams() const
{
    ClockLinkParams params;
    params.sourceName = sourceName;

    /* ICG */
    params.icg.configured = icgGroup->isChecked();
    if (params.icg.configured) {
        params.icg.enable         = icgEnableEdit->text();
        params.icg.polarity       = icgPolarityCombo->currentText();
        params.icg.test_enable    = icgTestEnableEdit->text();
        params.icg.reset          = icgResetEdit->text();
        params.icg.clock_on_reset = icgClockOnResetCheck->isChecked();

        params.icg.sta_guide.configured = icgStaGuideGroup->isChecked();
        if (params.icg.sta_guide.configured) {
            params.icg.sta_guide.cell     = icgStaCellEdit->text();
            params.icg.sta_guide.in       = icgStaInEdit->text();
            params.icg.sta_guide.out      = icgStaOutEdit->text();
            params.icg.sta_guide.instance = icgStaInstanceEdit->text();
        }
    }

    /* DIV */
    params.div.configured = divGroup->isChecked();
    if (params.div.configured) {
        params.div.default_value  = divDefaultSpin->value();
        params.div.value          = divValueEdit->text();
        params.div.width          = divWidthSpin->value();
        params.div.reset          = divResetEdit->text();
        params.div.clock_on_reset = divClockOnResetCheck->isChecked();

        params.div.sta_guide.configured = divStaGuideGroup->isChecked();
        if (params.div.sta_guide.configured) {
            params.div.sta_guide.cell     = divStaCellEdit->text();
            params.div.sta_guide.in       = divStaInEdit->text();
            params.div.sta_guide.out      = divStaOutEdit->text();
            params.div.sta_guide.instance = divStaInstanceEdit->text();
        }
    }

    /* INV */
    params.inv.configured = invGroup->isChecked();
    if (params.inv.configured) {
        params.inv.sta_guide.configured = invStaGuideGroup->isChecked();
        if (params.inv.sta_guide.configured) {
            params.inv.sta_guide.cell     = invStaCellEdit->text();
            params.inv.sta_guide.in       = invStaInEdit->text();
            params.inv.sta_guide.out      = invStaOutEdit->text();
            params.inv.sta_guide.instance = invStaInstanceEdit->text();
        }
    }

    /* Link-level STA Guide */
    params.sta_guide.configured = linkStaGuideGroup->isChecked();
    if (params.sta_guide.configured) {
        params.sta_guide.cell     = linkStaCellEdit->text();
        params.sta_guide.in       = linkStaInEdit->text();
        params.sta_guide.out      = linkStaOutEdit->text();
        params.sta_guide.instance = linkStaInstanceEdit->text();
    }

    /* Save last non-empty STA Guide Cell to scene for session memory */
    if (scene) {
        QString lastCell;
        if (params.icg.sta_guide.configured && !params.icg.sta_guide.cell.isEmpty()) {
            lastCell = params.icg.sta_guide.cell;
        }
        if (params.div.sta_guide.configured && !params.div.sta_guide.cell.isEmpty()) {
            lastCell = params.div.sta_guide.cell;
        }
        if (params.inv.sta_guide.configured && !params.inv.sta_guide.cell.isEmpty()) {
            lastCell = params.inv.sta_guide.cell;
        }
        if (params.sta_guide.configured && !params.sta_guide.cell.isEmpty()) {
            lastCell = params.sta_guide.cell;
        }
        if (!lastCell.isEmpty()) {
            const_cast<PrcScene *>(scene)->setLastStaGuideCell(lastCell);
        }
    }

    return params;
}

/* Controller Dialog Implementation */

PrcControllerDialog::PrcControllerDialog(
    ControllerType type, const QString &name, PrcScene *scene, QWidget *parent)
    : QDialog(parent)
    , type(type)
    , name(name)
    , scene(scene)
    , mainLayout(nullptr)
    , nameEdit(nullptr)
    , testEnableEdit(nullptr)
    , hostClockEdit(nullptr)
    , hostResetEdit(nullptr)
    , elementsList(nullptr)
    , deleteBtn(nullptr)
{
    /* Set window title based on type */
    QString typeStr;
    switch (type) {
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

    mainLayout = new QVBoxLayout(this);

    createForm();

    /* Dialog buttons */
    auto *buttonLayout = new QHBoxLayout();

    deleteBtn = new QPushButton(tr("Delete Controller"), this);
    deleteBtn->setStyleSheet("color: #c00;");
    connect(deleteBtn, &QPushButton::clicked, this, &PrcControllerDialog::onDeleteClicked);
    buttonLayout->addWidget(deleteBtn);

    buttonLayout->addStretch();

    auto *buttonBox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttonBox, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);
    buttonLayout->addWidget(buttonBox);

    mainLayout->addLayout(buttonLayout);

    setLayout(mainLayout);
}

void PrcControllerDialog::createForm()
{
    /* Basic Information Group */
    auto *basicGroup  = new QGroupBox(tr("Basic Information"), this);
    auto *basicLayout = new QFormLayout(basicGroup);

    nameEdit = new QLineEdit(name, this);
    nameEdit->setReadOnly(true);
    nameEdit->setStyleSheet("background-color: #f0f0f0;");
    basicLayout->addRow(tr("Name:"), nameEdit);

    mainLayout->addWidget(basicGroup);

    /* DFT Settings Group */
    auto *dftGroup  = new QGroupBox(tr("DFT Settings"), this);
    auto *dftLayout = new QFormLayout(dftGroup);

    /* Create test_enable field with Auto button */
    auto   *testEnableContainer = new QWidget(this);
    auto   *testEnableLayout    = new QHBoxLayout(testEnableContainer);
    QString autoTestEnable      = "test_en";
    testEnableLayout->setContentsMargins(0, 0, 0, 0);
    testEnableLayout->setSpacing(4);

    testEnableEdit = new QLineEdit(this);
    testEnableEdit->setPlaceholderText(autoTestEnable);
    testEnableLayout->addWidget(testEnableEdit, 1);

    auto *testEnableAutoBtn = new QPushButton(tr("Auto"), this);
    testEnableAutoBtn->setFixedWidth(50);
    testEnableAutoBtn->setToolTip(tr("Auto-fill: %1").arg(autoTestEnable));
    testEnableLayout->addWidget(testEnableAutoBtn);

    connect(testEnableAutoBtn, &QPushButton::clicked, [this, autoTestEnable]() {
        if (testEnableEdit->text().isEmpty()) {
            testEnableEdit->setText(autoTestEnable);
        }
    });

    /* Load existing value */
    if (scene) {
        switch (type) {
        case ClockController:
            if (scene->hasClockController(name)) {
                testEnableEdit->setText(scene->clockController(name).test_enable);
            }
            break;
        case ResetController:
            if (scene->hasResetController(name)) {
                testEnableEdit->setText(scene->resetController(name).test_enable);
            }
            break;
        case PowerController:
            if (scene->hasPowerController(name)) {
                testEnableEdit->setText(scene->powerController(name).test_enable);
            }
            break;
        }
    }

    dftLayout->addRow(tr("Test Enable:"), testEnableContainer);

    auto *dftHint = new QLabel(tr("DFT bypass signal for scan testing"), this);
    dftHint->setStyleSheet("color: #666; font-style: italic;");
    dftLayout->addRow(dftHint);

    mainLayout->addWidget(dftGroup);

    /* AO Domain Settings (Power Controller only) */
    if (type == PowerController) {
        auto *aoGroup  = new QGroupBox(tr("AO Domain Settings"), this);
        auto *aoLayout = new QFormLayout(aoGroup);

        /* Create host_clock field with Auto button */
        auto   *hostClockContainer = new QWidget(this);
        auto   *hostClockLayout    = new QHBoxLayout(hostClockContainer);
        QString autoHostClock      = "ao_clk";
        hostClockLayout->setContentsMargins(0, 0, 0, 0);
        hostClockLayout->setSpacing(4);

        hostClockEdit = new QLineEdit(this);
        hostClockEdit->setPlaceholderText(autoHostClock);
        hostClockLayout->addWidget(hostClockEdit, 1);

        auto *hostClockAutoBtn = new QPushButton(tr("Auto"), this);
        hostClockAutoBtn->setFixedWidth(50);
        hostClockAutoBtn->setToolTip(tr("Auto-fill: %1").arg(autoHostClock));
        hostClockLayout->addWidget(hostClockAutoBtn);

        connect(hostClockAutoBtn, &QPushButton::clicked, [this, autoHostClock]() {
            if (hostClockEdit->text().isEmpty()) {
                hostClockEdit->setText(autoHostClock);
            }
        });

        /* Create host_reset field with Auto button */
        auto   *hostResetContainer = new QWidget(this);
        auto   *hostResetLayout    = new QHBoxLayout(hostResetContainer);
        QString autoHostReset      = "ao_rst_n";
        hostResetLayout->setContentsMargins(0, 0, 0, 0);
        hostResetLayout->setSpacing(4);

        hostResetEdit = new QLineEdit(this);
        hostResetEdit->setPlaceholderText(autoHostReset);
        hostResetLayout->addWidget(hostResetEdit, 1);

        auto *hostResetAutoBtn = new QPushButton(tr("Auto"), this);
        hostResetAutoBtn->setFixedWidth(50);
        hostResetAutoBtn->setToolTip(tr("Auto-fill: %1").arg(autoHostReset));
        hostResetLayout->addWidget(hostResetAutoBtn);

        connect(hostResetAutoBtn, &QPushButton::clicked, [this, autoHostReset]() {
            if (hostResetEdit->text().isEmpty()) {
                hostResetEdit->setText(autoHostReset);
            }
        });

        /* Load existing values */
        if (scene && scene->hasPowerController(name)) {
            auto def = scene->powerController(name);
            hostClockEdit->setText(def.host_clock);
            hostResetEdit->setText(def.host_reset);
        }

        aoLayout->addRow(tr("Host Clock:"), hostClockContainer);
        aoLayout->addRow(tr("Host Reset:"), hostResetContainer);

        auto *aoHint = new QLabel(tr("Always-on domain clock and reset signals"), this);
        aoHint->setStyleSheet("color: #666; font-style: italic;");
        aoLayout->addRow(aoHint);

        mainLayout->addWidget(aoGroup);
    }

    /* Assigned Elements Group */
    auto *elemGroup  = new QGroupBox(tr("Assigned Elements"), this);
    auto *elemLayout = new QVBoxLayout(elemGroup);

    elementsList = new QListWidget(this);
    elementsList->setMaximumHeight(120);
    elementsList->setSelectionMode(QAbstractItemView::NoSelection);
    populateElementsList();

    elemLayout->addWidget(elementsList);

    auto *elemHint = new QLabel(tr("Elements using this controller (read-only)"), this);
    elemHint->setStyleSheet("color: #666; font-style: italic;");
    elemLayout->addWidget(elemHint);

    mainLayout->addWidget(elemGroup);
}

void PrcControllerDialog::populateElementsList()
{
    if (!scene || !elementsList) {
        return;
    }

    elementsList->clear();

    /* Find all elements assigned to this controller */
    for (const auto &node : scene->nodes()) {
        auto prcItem = std::dynamic_pointer_cast<PrcPrimitiveItem>(node);
        if (!prcItem) {
            continue;
        }

        QString     itemController;
        QString     itemName;
        QString     itemType;
        const auto &params = prcItem->params();

        switch (type) {
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

        if (itemController == name && !itemName.isEmpty()) {
            auto *item = new QListWidgetItem(QString("%1 (%2)").arg(itemName, itemType));
            elementsList->addItem(item);
        }
    }

    if (elementsList->count() == 0) {
        auto *item = new QListWidgetItem(tr("(no elements assigned)"));
        item->setForeground(Qt::gray);
        elementsList->addItem(item);
    }
}

void PrcControllerDialog::onDeleteClicked()
{
    /* Check if controller has assigned elements */
    if (elementsList && elementsList->count() > 0) {
        auto *firstItem = elementsList->item(0);
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
        tr("Are you sure you want to delete controller '%1'?").arg(name),
        QMessageBox::Yes | QMessageBox::No);

    if (result == QMessageBox::Yes) {
        emit deleteRequested();
        reject();
    }
}

ClockControllerDef PrcControllerDialog::getClockControllerDef() const
{
    ClockControllerDef def;
    def.name        = name;
    def.test_enable = testEnableEdit ? testEnableEdit->text() : QString();
    return def;
}

ResetControllerDef PrcControllerDialog::getResetControllerDef() const
{
    ResetControllerDef def;
    def.name        = name;
    def.test_enable = testEnableEdit ? testEnableEdit->text() : QString();
    return def;
}

PowerControllerDef PrcControllerDialog::getPowerControllerDef() const
{
    PowerControllerDef def;
    def.name        = name;
    def.test_enable = testEnableEdit ? testEnableEdit->text() : QString();
    def.host_clock  = hostClockEdit ? hostClockEdit->text() : QString();
    def.host_reset  = hostResetEdit ? hostResetEdit->text() : QString();
    return def;
}
