// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2023-2025 Huang Rui <vowstar@gmail.com>

#include "gui/prcwindow/prcwindow.h"

#include "./ui_prcwindow.h"
#include "common/qstringutils.h"

#include <QCloseEvent>
#include <QDebug>
#include <QFileDialog>
#include <QFileInfo>
#include <QIcon>
#include <QMessageBox>
#include <QPrintDialog>
#include <QPrinter>
#include <QStandardPaths>
#include <QTimer>

#include <gpds/archiver_yaml.hpp>
#include <gpds/serialize.hpp>
#include <qschematic/scene.hpp>

#include "common/qsocprojectmanager.h"

void PrcWindow::on_actionOpen_triggered()
{
    // Check if there are unsaved changes
    if (!checkSaveBeforeClose()) {
        return;
    }

    if (!projectManager) {
        QMessageBox::warning(this, tr("Open Error"), tr("No project manager available"));
        return;
    }

    QString defaultPath = projectManager->getSchematicPath();
    if (defaultPath.isEmpty()) {
        defaultPath = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    }

    QString fileName = QFileDialog::getOpenFileName(
        this, tr("Open Schematic"), defaultPath, tr("SOC Schematic Files (*.soc_prc)"));

    if (fileName.isEmpty()) {
        return;
    }

    openFile(fileName);
}

void PrcWindow::on_actionSave_triggered()
{
    if (m_currentFilePath.isEmpty()) {
        // Untitled file - convert to Save As
        on_actionSaveAs_triggered();
    } else {
        // Save to current file path
        saveToFile(m_currentFilePath);
    }
}

void PrcWindow::on_actionSaveAs_triggered()
{
    if (!projectManager) {
        QMessageBox::warning(this, tr("Save Error"), tr("No project manager available"));
        return;
    }

    QString defaultPath = projectManager->getSchematicPath();
    if (defaultPath.isEmpty()) {
        defaultPath = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    }

    QString fileName = QFileDialog::getSaveFileName(
        this, tr("Save Schematic As"), defaultPath, tr("SOC Schematic Files (*.soc_prc)"));

    if (fileName.isEmpty()) {
        return;
    }

    if (!fileName.endsWith(".soc_prc")) {
        fileName += ".soc_prc";
    }

    saveToFile(fileName);
}

void PrcWindow::on_actionClose_triggered()
{
    // Check for unsaved changes
    if (!checkSaveBeforeClose()) {
        return; // User cancelled
    }

    // Close the file and reset to untitled
    closeFile();
}

void PrcWindow::on_actionPrint_triggered()
{
    QPrinter printer(QPrinter::HighResolution);
    if (QPrintDialog(&printer).exec() == QDialog::Accepted) {
        QPainter painter(&printer);
        painter.setRenderHint(QPainter::Antialiasing);
        scene.render(&painter);
    }
}

void PrcWindow::openFile(const QString &filePath)
{
    // Clear existing scene and undo stack
    scene.clear();
    scene.undoStack()->clear();

    // Use standard gpds API to deserialize Scene directly
    const std::filesystem::path path = filePath.toStdString();

    try {
        const auto &[success, message]
            = gpds::from_file<gpds::archiver_yaml>(path, scene, QSchematic::Scene::gpds_name);

        if (!success) {
            QMessageBox::critical(
                this,
                tr("Open Error"),
                tr("Failed to load schematic: %1").arg(QString::fromStdString(message)));
            return;
        }

        // Successfully loaded
        m_currentFilePath = filePath;
        scene.undoStack()->setClean();
        updateWindowTitle();

        // Update connector states based on wire connections
        updateAllDynamicPorts();

    } catch (const std::bad_optional_access &e) {
        QMessageBox::critical(
            this,
            tr("Open Error"),
            tr("Incompatible file format. This file was created with an older version.\n"
               "Please create a new schematic file."));
    } catch (const std::exception &e) {
        QMessageBox::critical(
            this, tr("Open Error"), tr("Failed to load schematic: %1").arg(e.what()));
    }
}

void PrcWindow::saveToFile(const QString &path)
{
    // Use standard gpds API to serialize Scene directly
    const std::filesystem::path fsPath = path.toStdString();
    const auto &[success, message]
        = gpds::to_file<gpds::archiver_yaml>(fsPath, scene, QSchematic::Scene::gpds_name);

    if (!success) {
        QMessageBox::critical(
            this,
            tr("Save Error"),
            tr("Failed to save schematic: %1").arg(QString::fromStdString(message)));
        return;
    }

    // Successfully saved
    m_currentFilePath = path;
    scene.undoStack()->setClean();
    updateWindowTitle();
}

void PrcWindow::closeFile()
{
    // Clear scene content
    scene.clear();

    // Clear undo history
    scene.undoStack()->clear();

    // Reset to untitled state
    m_currentFilePath = "";
    updateWindowTitle();
}

bool PrcWindow::checkSaveBeforeClose()
{
    if (scene.undoStack()->isClean()) {
        return true; // No changes, safe to proceed
    }

    QMessageBox::StandardButton result = QMessageBox::question(
        this,
        tr("Save Changes?"),
        tr("Do you want to save changes to %1?").arg(getCurrentFileName()),
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);

    if (result == QMessageBox::Save) {
        on_actionSave_triggered();
        return scene.undoStack()->isClean(); // Return true if save succeeded
    } else if (result == QMessageBox::Discard) {
        return true; // Discard changes, safe to proceed
    } else {
        return false; // Cancel
    }
}

QString PrcWindow::getCurrentFileName() const
{
    return m_currentFilePath.isEmpty() ? "untitled"
                                       : QFileInfo(m_currentFilePath).completeBaseName();
}

void PrcWindow::updateWindowTitle()
{
    QString filename;
    if (m_currentFilePath.isEmpty()) {
        filename = "untitled";
    } else {
        filename = QFileInfo(m_currentFilePath).completeBaseName();
    }

    if (!scene.undoStack()->isClean()) {
        filename = "*" + filename;
    }

    setWindowTitle(QString("Schematic Editor - %1").arg(filename));

    /* Update status bar permanent label */
    if (statusBarPermanentLabel) {
        if (m_currentFilePath.isEmpty()) {
            statusBarPermanentLabel->clear();
        } else {
            const QString displayPath = QStringUtils::truncateMiddle(m_currentFilePath, 60);
            statusBarPermanentLabel->setText(QString("Schematic: %1").arg(displayPath));
        }
    }
}

void PrcWindow::closeEvent(QCloseEvent *event)
{
    if (checkSaveBeforeClose()) {
        // Clean up before closing window
        closeFile();
        event->accept();
    } else {
        event->ignore();
    }
}
