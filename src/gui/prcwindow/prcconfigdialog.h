// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2023-2025 Huang Rui <vowstar@gmail.com>

#ifndef PRCCONFIGDIALOG_H
#define PRCCONFIGDIALOG_H

#include "prcprimitiveitem.h"
#include "prcscene.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSpinBox>
#include <QTabWidget>

namespace PrcLibrary {

/**
 * @brief Dialog for configuring PRC primitive properties
 * @details Provides dynamic form-based interface to edit primitive-specific
 *          configuration parameters matching soc_net format.
 */
class PrcConfigDialog : public QDialog
{
    Q_OBJECT

public:
    /**
     * @brief Construct a configuration dialog for the given primitive
     * @param[in] item The primitive item to configure
     * @param[in] scene The PRC scene (for controller management)
     * @param[in] connectedSources List of source names connected to this target (for targets)
     * @param[in] parent Parent widget
     */
    explicit PrcConfigDialog(
        PrcPrimitiveItem  *item,
        PrcScene          *scene,
        const QStringList &connectedSources = QStringList(),
        QWidget           *parent           = nullptr);

    /**
     * @brief Apply configured values to the item
     */
    void applyConfiguration();

private slots:
    /**
     * @brief Handle controller combo box selection change
     * @param[in] index Selected index
     */
    void onControllerChanged(int index);

    /**
     * @brief Handle "Edit Controller..." button click
     */
    void onEditControllerClicked();

private:
    /* Form creation methods */
    void createClockInputForm();
    void createClockTargetForm();
    void createResetSourceForm();
    void createResetTargetForm();
    void createPowerDomainForm();

    /**
     * @brief Create controller selection group
     */
    void createControllerGroup();

    /**
     * @brief Populate controller combo box with existing controllers
     */
    void populateControllerCombo();

    PrcPrimitiveItem *item_;             /**< The primitive item being configured */
    PrcScene         *scene_;            /**< PRC scene for controller management */
    QStringList       connectedSources_; /**< Connected source names (for targets) */
    QVBoxLayout      *mainLayout_;       /**< Main dialog layout */
    QLineEdit        *nameEdit_;         /**< Primitive name editor */

    /* Controller selection widgets */
    QComboBox   *controllerCombo_;   /**< Controller selection combo */
    QPushButton *editControllerBtn_; /**< Edit controller button */

    /* Clock Input widgets */
    QLineEdit *inputFreqEdit_;

    /* Clock Target widgets */
    QLineEdit *targetFreqEdit_;
    QCheckBox *targetMuxCheck_;
    QLineEdit *targetSelectEdit_;
    QLineEdit *targetResetEdit_;
    QLineEdit *targetTestClockEdit_;
    QCheckBox *targetIcgCheck_;
    QLineEdit *targetIcgEnableEdit_;
    QComboBox *targetIcgPolarityCombo_;
    QCheckBox *targetIcgClockOnResetCheck_;
    QCheckBox *targetDivCheck_;
    QSpinBox  *targetDivDefaultSpin_;
    QLineEdit *targetDivValueEdit_;
    QSpinBox  *targetDivWidthSpin_;
    QLineEdit *targetDivResetEdit_;
    QCheckBox *targetDivClockOnResetCheck_;
    QCheckBox *targetInvCheck_;

    /* Reset Source widgets */
    QComboBox *rstSrcActiveCombo_;

    /* Reset Target widgets */
    QComboBox *rstTgtActiveCombo_;
    QCheckBox *rstTgtAsyncCheck_;
    QLineEdit *rstTgtAsyncClockEdit_;
    QSpinBox  *rstTgtAsyncStageSpin_;

    /* Power Domain widgets */
    QSpinBox  *pwrDomVoltageSpin_;
    QLineEdit *pwrDomPgoodEdit_;
    QSpinBox  *pwrDomWaitDepSpin_;
    QSpinBox  *pwrDomSettleOnSpin_;
    QSpinBox  *pwrDomSettleOffSpin_;
};

/**
 * @brief Dialog for configuring controller properties
 * @details Provides form-based interface to edit controller-level settings
 *          including test_enable (all types) and host_clock/host_reset (power).
 */
class PrcControllerDialog : public QDialog
{
    Q_OBJECT

public:
    /**
     * @brief Controller type enumeration
     */
    enum ControllerType { ClockController, ResetController, PowerController };

    /**
     * @brief Construct a controller configuration dialog
     * @param[in] type Controller type (Clock/Reset/Power)
     * @param[in] name Controller name
     * @param[in] scene PRC scene for element lookup
     * @param[in] parent Parent widget
     */
    explicit PrcControllerDialog(
        ControllerType type, const QString &name, PrcScene *scene, QWidget *parent = nullptr);

    /**
     * @brief Get configured clock controller definition
     * @return Clock controller definition
     */
    ClockControllerDef getClockControllerDef() const;

    /**
     * @brief Get configured reset controller definition
     * @return Reset controller definition
     */
    ResetControllerDef getResetControllerDef() const;

    /**
     * @brief Get configured power controller definition
     * @return Power controller definition
     */
    PowerControllerDef getPowerControllerDef() const;

signals:
    /**
     * @brief Signal emitted when delete is requested
     */
    void deleteRequested();

private slots:
    /**
     * @brief Handle delete button click
     */
    void onDeleteClicked();

private:
    /**
     * @brief Create form widgets based on controller type
     */
    void createForm();

    /**
     * @brief Populate elements list with assigned primitives
     */
    void populateElementsList();

    ControllerType type_;           /**< Controller type */
    QString        name_;           /**< Controller name */
    PrcScene      *scene_;          /**< PRC scene reference */
    QVBoxLayout   *mainLayout_;     /**< Main dialog layout */
    QLineEdit     *nameEdit_;       /**< Name editor (read-only) */
    QLineEdit     *testEnableEdit_; /**< Test enable signal editor */
    QLineEdit     *hostClockEdit_;  /**< Host clock editor (power only) */
    QLineEdit     *hostResetEdit_;  /**< Host reset editor (power only) */
    QListWidget   *elementsList_;   /**< Assigned elements list */
    QPushButton   *deleteBtn_;      /**< Delete button */
};

/**
 * @brief Dialog for configuring clock link (wire) operations
 * @details Provides form-based interface to edit link-level ICG, DIV, INV,
 *          and STA guide parameters for connections between inputs and targets.
 */
class PrcLinkConfigDialog : public QDialog
{
    Q_OBJECT

public:
    /**
     * @brief Construct a link configuration dialog
     * @param[in] sourceName Name of the source (input) this link connects from
     * @param[in] targetName Name of the target this link connects to
     * @param[in] linkParams Current link parameters
     * @param[in] parent Parent widget
     */
    explicit PrcLinkConfigDialog(
        const QString         &sourceName,
        const QString         &targetName,
        const ClockLinkParams &linkParams,
        QWidget               *parent = nullptr);

    /**
     * @brief Get configured link parameters
     * @return Configured ClockLinkParams
     */
    ClockLinkParams getLinkParams() const;

private:
    /* Form creation */
    void createForm();

    /* Helper methods */
    QGroupBox *createSTAGuideGroup(const QString &title, bool *configured);
    QGroupBox *createICGGroup();
    QGroupBox *createDIVGroup();
    QGroupBox *createINVGroup();

    QString         sourceName_; /**< Source input name */
    QString         targetName_; /**< Target output name */
    ClockLinkParams linkParams_; /**< Link parameters being edited */
    QVBoxLayout    *mainLayout_; /**< Main dialog layout */

    /* ICG widgets */
    QGroupBox *icgGroup_;
    QLineEdit *icgEnableEdit_;
    QComboBox *icgPolarityCombo_;
    QLineEdit *icgTestEnableEdit_;
    QLineEdit *icgResetEdit_;
    QCheckBox *icgClockOnResetCheck_;
    QGroupBox *icgStaGuideGroup_;
    QLineEdit *icgStaCellEdit_;
    QLineEdit *icgStaInEdit_;
    QLineEdit *icgStaOutEdit_;
    QLineEdit *icgStaInstanceEdit_;

    /* DIV widgets */
    QGroupBox *divGroup_;
    QSpinBox  *divDefaultSpin_;
    QLineEdit *divValueEdit_;
    QSpinBox  *divWidthSpin_;
    QLineEdit *divResetEdit_;
    QCheckBox *divClockOnResetCheck_;
    QGroupBox *divStaGuideGroup_;
    QLineEdit *divStaCellEdit_;
    QLineEdit *divStaInEdit_;
    QLineEdit *divStaOutEdit_;
    QLineEdit *divStaInstanceEdit_;

    /* INV widgets */
    QGroupBox *invGroup_;
    QGroupBox *invStaGuideGroup_;
    QLineEdit *invStaCellEdit_;
    QLineEdit *invStaInEdit_;
    QLineEdit *invStaOutEdit_;
    QLineEdit *invStaInstanceEdit_;

    /* Link-level STA guide widgets */
    QGroupBox *linkStaGuideGroup_;
    QLineEdit *linkStaCellEdit_;
    QLineEdit *linkStaInEdit_;
    QLineEdit *linkStaOutEdit_;
    QLineEdit *linkStaInstanceEdit_;
};

} // namespace PrcLibrary

#endif // PRCCONFIGDIALOG_H
