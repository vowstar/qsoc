// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2023-2025 Huang Rui <vowstar@gmail.com>

#ifndef PRCCONFIGDIALOG_H
#define PRCCONFIGDIALOG_H

#include "prcprimitiveitem.h"

#include <QCheckBox>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QLineEdit>
#include <QSpinBox>

namespace PrcLibrary {

/**
 * @brief Dialog for configuring PRC primitive properties
 * @details Provides a form-based interface to edit primitive-specific configuration
 *          parameters such as clock frequencies, reset levels, and power domains.
 *          Uses type-appropriate widgets (QSpinBox for integers, QCheckBox for booleans).
 */
class PrcConfigDialog : public QDialog
{
    Q_OBJECT

public:
    /**
     * @brief Construct a configuration dialog for the given primitive
     * @param[in] item The primitive item to configure
     * @param[in] parent Parent widget
     */
    explicit PrcConfigDialog(PrcPrimitiveItem *item, QWidget *parent = nullptr);

    /**
     * @brief Apply configured values to the item
     */
    void applyConfiguration();

private:
    /**
     * @brief Populate form with type-specific configuration fields
     */
    void createClockSourceFields();
    void createClockTargetFields();
    void createResetSourceFields();
    void createResetTargetFields();
    void createPowerDomainFields();

    PrcPrimitiveItem *item_;       /**< The primitive item being configured */
    QFormLayout      *formLayout_; /**< Form layout for configuration fields */
    QLineEdit        *nameEdit_;   /**< Primitive name editor */

    /* Type-specific widgets */
    QDoubleSpinBox *freqSpin_;
    QDoubleSpinBox *phaseSpin_;
    QSpinBox       *dividerSpin_;
    QCheckBox      *enableGateCheck_;
    QCheckBox      *activeLowCheck_;
    QDoubleSpinBox *durationSpin_;
    QCheckBox      *synchronousCheck_;
    QSpinBox       *stagesSpin_;
    QDoubleSpinBox *voltageSpin_;
    QCheckBox      *isolationCheck_;
    QCheckBox      *retentionCheck_;
};

} // namespace PrcLibrary

#endif // PRCCONFIGDIALOG_H
