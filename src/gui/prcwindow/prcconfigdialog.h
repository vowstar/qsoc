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

    /**
     * @brief Create a QLineEdit with an Auto button
     * @param[in] initialValue Initial text value
     * @param[in] placeholder Placeholder text
     * @param[in] autoValue Value to fill when Auto is clicked
     * @param[in] parent Parent widget
     * @return Pair of QLineEdit and container widget
     */
    QWidget *createAutoLineEdit(
        QLineEdit    **lineEdit,
        const QString &initialValue,
        const QString &placeholder,
        const QString &autoValue,
        QWidget       *parent);

    /**
     * @brief Update all Cell field placeholders from scene's lastStaGuideCell
     */
    void updateCellPlaceholders();

    PrcPrimitiveItem *item;             /**< The primitive item being configured */
    PrcScene         *scene;            /**< PRC scene for controller management */
    QStringList       connectedSources; /**< Connected source names (for targets) */
    QVBoxLayout      *mainLayout;       /**< Main dialog layout */
    QLineEdit        *nameEdit;         /**< Primitive name editor */

    /* Controller selection widgets */
    QComboBox   *controllerCombo;   /**< Controller selection combo */
    QPushButton *editControllerBtn; /**< Edit controller button */

    /* Clock Input widgets */
    QLineEdit *inputFreqEdit;

    /* Clock Target widgets */
    QLineEdit *targetFreqEdit;
    QCheckBox *targetMuxCheck;
    QLineEdit *targetSelectEdit;
    QLineEdit *targetResetEdit;
    QLineEdit *targetTestClockEdit;
    QCheckBox *targetIcgCheck;
    QLineEdit *targetIcgEnableEdit;
    QComboBox *targetIcgPolarityCombo;
    QCheckBox *targetIcgClockOnResetCheck;
    QCheckBox *targetDivCheck;
    QSpinBox  *targetDivDefaultSpin;
    QLineEdit *targetDivValueEdit;
    QSpinBox  *targetDivWidthSpin;
    QLineEdit *targetDivResetEdit;
    QCheckBox *targetDivClockOnResetCheck;
    QCheckBox *targetInvCheck;

    /* Clock Target STA Guide widgets */
    QGroupBox *targetMuxStaGroup;
    QLineEdit *targetMuxStaCellEdit;
    QLineEdit *targetMuxStaInEdit;
    QLineEdit *targetMuxStaOutEdit;
    QLineEdit *targetMuxStaInstanceEdit;
    QGroupBox *targetIcgStaGroup;
    QLineEdit *targetIcgStaCellEdit;
    QLineEdit *targetIcgStaInEdit;
    QLineEdit *targetIcgStaOutEdit;
    QLineEdit *targetIcgStaInstanceEdit;
    QGroupBox *targetDivStaGroup;
    QLineEdit *targetDivStaCellEdit;
    QLineEdit *targetDivStaInEdit;
    QLineEdit *targetDivStaOutEdit;
    QLineEdit *targetDivStaInstanceEdit;
    QGroupBox *targetInvStaGroup;
    QLineEdit *targetInvStaCellEdit;
    QLineEdit *targetInvStaInEdit;
    QLineEdit *targetInvStaOutEdit;
    QLineEdit *targetInvStaInstanceEdit;

    /* Reset Source widgets */
    QComboBox *rstSrcActiveCombo;

    /* Reset Target widgets */
    QComboBox *rstTgtActiveCombo;
    QCheckBox *rstTgtAsyncCheck;
    QLineEdit *rstTgtAsyncClockEdit;
    QSpinBox  *rstTgtAsyncStageSpin;

    /* Power Domain widgets */
    QSpinBox  *pwrDomVoltageSpin;
    QLineEdit *pwrDomPgoodEdit;
    QSpinBox  *pwrDomWaitDepSpin;
    QSpinBox  *pwrDomSettleOnSpin;
    QSpinBox  *pwrDomSettleOffSpin;
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

    ControllerType type;           /**< Controller type */
    QString        name;           /**< Controller name */
    PrcScene      *scene;          /**< PRC scene reference */
    QVBoxLayout   *mainLayout;     /**< Main dialog layout */
    QLineEdit     *nameEdit;       /**< Name editor (read-only) */
    QLineEdit     *testEnableEdit; /**< Test enable signal editor */
    QLineEdit     *hostClockEdit;  /**< Host clock editor (power only) */
    QLineEdit     *hostResetEdit;  /**< Host reset editor (power only) */
    QListWidget   *elementsList;   /**< Assigned elements list */
    QPushButton   *deleteBtn;      /**< Delete button */
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
     * @param[in] scene PRC scene for session-level memory
     * @param[in] parent Parent widget
     */
    explicit PrcLinkConfigDialog(
        const QString         &sourceName,
        const QString         &targetName,
        const ClockLinkParams &linkParams,
        PrcScene              *scene  = nullptr,
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

    /**
     * @brief Create a QLineEdit with an Auto button
     * @param[out] lineEdit Pointer to store the created QLineEdit
     * @param[in] initialValue Initial text value
     * @param[in] placeholder Placeholder text
     * @param[in] autoValue Value to fill when Auto is clicked
     * @param[in] parent Parent widget
     * @return Container widget with QLineEdit and Auto button
     */
    QWidget *createAutoLineEdit(
        QLineEdit    **lineEdit,
        const QString &initialValue,
        const QString &placeholder,
        const QString &autoValue,
        QWidget       *parent);

    /**
     * @brief Update all Cell field placeholders from scene's lastStaGuideCell
     */
    void updateCellPlaceholders();

    QString         sourceName; /**< Source input name */
    QString         targetName; /**< Target output name */
    ClockLinkParams linkParams; /**< Link parameters being edited */
    PrcScene       *scene;      /**< PRC scene for session memory */
    QVBoxLayout    *mainLayout; /**< Main dialog layout */

    /* ICG widgets */
    QGroupBox *icgGroup;
    QLineEdit *icgEnableEdit;
    QComboBox *icgPolarityCombo;
    QLineEdit *icgTestEnableEdit;
    QLineEdit *icgResetEdit;
    QCheckBox *icgClockOnResetCheck;
    QGroupBox *icgStaGuideGroup;
    QLineEdit *icgStaCellEdit;
    QLineEdit *icgStaInEdit;
    QLineEdit *icgStaOutEdit;
    QLineEdit *icgStaInstanceEdit;

    /* DIV widgets */
    QGroupBox *divGroup;
    QSpinBox  *divDefaultSpin;
    QLineEdit *divValueEdit;
    QSpinBox  *divWidthSpin;
    QLineEdit *divResetEdit;
    QCheckBox *divClockOnResetCheck;
    QGroupBox *divStaGuideGroup;
    QLineEdit *divStaCellEdit;
    QLineEdit *divStaInEdit;
    QLineEdit *divStaOutEdit;
    QLineEdit *divStaInstanceEdit;

    /* INV widgets */
    QGroupBox *invGroup;
    QGroupBox *invStaGuideGroup;
    QLineEdit *invStaCellEdit;
    QLineEdit *invStaInEdit;
    QLineEdit *invStaOutEdit;
    QLineEdit *invStaInstanceEdit;

    /* Link-level STA guide widgets */
    QGroupBox *linkStaGuideGroup;
    QLineEdit *linkStaCellEdit;
    QLineEdit *linkStaInEdit;
    QLineEdit *linkStaOutEdit;
    QLineEdit *linkStaInstanceEdit;
};

} // namespace PrcLibrary

#endif // PRCCONFIGDIALOG_H
