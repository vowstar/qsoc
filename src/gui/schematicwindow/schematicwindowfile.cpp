// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2023-2025 Huang Rui <vowstar@gmail.com>

#include "common/qsocyamlutils.h"
#include "gui/schematicwindow/schematicwindow.h"

#include "./ui_schematicwindow.h"
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

#include <gpds/archiver_yaml.hpp>
#include <gpds/serialize.hpp>
#include <qschematic/scene.hpp>

#include "common/qsocprojectmanager.h"

void SchematicWindow::on_actionOpen_triggered()
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
        this, tr("Open Schematic"), defaultPath, tr("SOC Schematic Files (*.soc_sch)"));

    if (fileName.isEmpty()) {
        return;
    }

    openFile(fileName);
}

void SchematicWindow::on_actionSave_triggered()
{
    if (m_currentFilePath.isEmpty()) {
        // Untitled file - convert to Save As
        on_actionSaveAs_triggered();
    } else {
        // Save to current file path
        saveToFile(m_currentFilePath);
    }
}

void SchematicWindow::on_actionSaveAs_triggered()
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
        this, tr("Save Schematic As"), defaultPath, tr("SOC Schematic Files (*.soc_sch)"));

    if (fileName.isEmpty()) {
        return;
    }

    if (!fileName.endsWith(".soc_sch")) {
        fileName += ".soc_sch";
    }

    saveToFile(fileName);
}

void SchematicWindow::on_actionClose_triggered()
{
    // Check for unsaved changes
    if (!checkSaveBeforeClose()) {
        return; // User cancelled
    }

    // Close the file and reset to untitled
    closeFile();
}

void SchematicWindow::on_actionPrint_triggered()
{
    QPrinter printer(QPrinter::HighResolution);
    if (QPrintDialog(&printer).exec() == QDialog::Accepted) {
        QPainter painter(&printer);
        painter.setRenderHint(QPainter::Antialiasing);
        scene.render(&painter);
    }
}

void SchematicWindow::openFile(const QString &filePath)
{
    /* Check the format version before the current document is discarded. A
     * mismatch used to load as an empty scene, adopt the path, and let the
     * next save overwrite the file. */
    int     fileVersion = 0;
    QString versionError;
    if (!QSocYamlUtils::readDocumentVersion(
            filePath, QString::fromUtf8(QSchematic::Scene::gpds_name), fileVersion, versionError)) {
        QMessageBox::critical(
            this,
            tr("Open Error"),
            tr("Failed to read %1: %2").arg(QFileInfo(filePath).fileName(), versionError));
        return;
    }
    if (fileVersion != static_cast<int>(QSchematic::Scene::serdes_version)) {
        QMessageBox::critical(
            this,
            tr("Open Error"),
            tr("%1 was written in format version %2, but this release reads "
               "version %3. The file was left unchanged.")
                .arg(QFileInfo(filePath).fileName())
                .arg(fileVersion)
                .arg(static_cast<int>(QSchematic::Scene::serdes_version)));
        return;
    }

    /* Deserialize into a throwaway scene first, so a malformed file cannot
     * destroy the document that is already open. */
    {
        QSchematic::Scene probe;
        try {
            const auto &[probeOk, probeMessage] = gpds::from_file<gpds::archiver_yaml>(
                filePath.toStdString(), probe, QSchematic::Scene::gpds_name);
            if (!probeOk) {
                QMessageBox::critical(
                    this,
                    tr("Open Error"),
                    tr("Failed to load %1: %2")
                        .arg(QFileInfo(filePath).fileName(), QString::fromStdString(probeMessage)));
                return;
            }
        } catch (const std::exception &error) {
            QMessageBox::critical(
                this,
                tr("Open Error"),
                tr("Failed to load %1: %2")
                    .arg(QFileInfo(filePath).fileName(), QString::fromUtf8(error.what())));
            return;
        }
    }

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

void SchematicWindow::saveToFile(const QString &path)
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

void SchematicWindow::closeFile()
{
    // Clear scene content
    scene.clear();

    // Clear undo history
    scene.undoStack()->clear();

    // Reset to untitled state
    m_currentFilePath = "";
    updateWindowTitle();
}

bool SchematicWindow::isModified() const
{
    return !scene.undoStack()->isClean();
}

bool SchematicWindow::checkSaveBeforeClose()
{
    if (!isModified()) {
        return true; // No changes, safe to proceed
    }

    QMessageBox::StandardButton result = QMessageBox::question(
        this,
        tr("Save Changes?"),
        tr("Do you want to save changes to %1?").arg(getCurrentFileName()),
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);

    if (result == QMessageBox::Save) {
        on_actionSave_triggered();
        return !isModified(); // Return true if save succeeded
    } else if (result == QMessageBox::Discard) {
        return true; // Discard changes, safe to proceed
    } else {
        return false; // Cancel
    }
}

QString SchematicWindow::getCurrentFileName() const
{
    return m_currentFilePath.isEmpty() ? "untitled"
                                       : QFileInfo(m_currentFilePath).completeBaseName();
}

void SchematicWindow::updateWindowTitle()
{
    QString filename;
    if (m_currentFilePath.isEmpty()) {
        filename = "untitled";
    } else {
        filename = QFileInfo(m_currentFilePath).completeBaseName();
    }

    if (isModified()) {
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

void SchematicWindow::closeEvent(QCloseEvent *event)
{
    if (checkSaveBeforeClose()) {
        // Clean up before closing window
        closeFile();
        event->accept();
    } else {
        event->ignore();
    }
}
