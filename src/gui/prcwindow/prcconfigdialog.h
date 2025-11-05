// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2023-2025 Huang Rui <vowstar@gmail.com>

#ifndef PRCCONFIGDIALOG_H
#define PRCCONFIGDIALOG_H

#include "prcprimitiveitem.h"

#include <QDialog>
#include <QFormLayout>
#include <QLineEdit>
#include <QMap>
#include <QString>
#include <QVariant>

namespace PrcLibrary {

/**
 * @brief Dialog for configuring PRC primitive properties
 * @details Provides a form-based interface to edit primitive-specific configuration
 *          parameters such as clock frequencies, reset levels, and power domains.
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
     * @brief Retrieve all configured field values
     * @return Map of field names to configured values
     */
    QMap<QString, QVariant> getConfiguration() const;

private:
    /**
     * @brief Populate form with type-specific configuration fields
     * @param[in] type The primitive type determining which fields to create
     */
    void createFieldsForType(PrimitiveType type);

    PrcPrimitiveItem          *item_;       /**< The primitive item being configured */
    QFormLayout               *formLayout_; /**< Form layout for configuration fields */
    QMap<QString, QLineEdit *> fields_;     /**< Map of field names to input widgets */
};

} // namespace PrcLibrary

#endif // PRCCONFIGDIALOG_H
