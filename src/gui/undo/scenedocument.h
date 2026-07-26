// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef SCENEDOCUMENT_H
#define SCENEDOCUMENT_H

#include "common/qsocconsole.h"

#include <string>

#include <qschematic/scene.hpp>

#include <gpds/archiver_yaml.hpp>
#include <gpds/serialize.hpp>

#include <QByteArray>
#include <QString>

/**
 * @brief Serialized form of a schematic scene.
 * @details Used to give bulk edits a single step of undo. The same
 *          serialization backs the on-disk format, so a snapshot restores
 *          exactly what a save and reload would.
 */
class SceneDocument
{
public:
    /**
     * @brief Serialize a scene.
     * @param[in] scene Scene to capture.
     * @return Serialized document, empty when serialization failed.
     */
    static QByteArray capture(QSchematic::Scene &scene)
    {
        std::string serialized;
        const auto &[succeeded, message]
            = gpds::to_string<gpds::archiver_yaml>(serialized, scene, QSchematic::Scene::gpds_name);
        if (!succeeded) {
            QSocConsole::warn() << "Failed to capture the scene:"
                                << QString::fromStdString(message);
            return {};
        }
        return QByteArray::fromStdString(serialized);
    }

    /**
     * @brief Empty a scene without touching its undo stack.
     * @details QSchematic::Scene::clear() also clears the undo stack. Calling
     *          it from inside a command's undo() destroys the very command
     *          being executed, so take the scene apart by hand instead.
     * @param[in,out] scene Scene to empty.
     */
    static void clearKeepingHistory(QSchematic::Scene &scene)
    {
        scene.clearSelection();
        const auto items = scene.items();
        for (const auto &item : items) {
            scene.removeItem(item);
        }
        if (const auto manager = scene.wire_manager()) {
            manager->clear();
        }
    }

    /**
     * @brief Replace a scene with a serialized document.
     * @param[in,out] scene Scene to overwrite.
     * @param[in] document Serialized document from capture().
     */
    static void restore(QSchematic::Scene &scene, const QByteArray &document)
    {
        clearKeepingHistory(scene);
        if (document.isEmpty()) {
            return;
        }
        const auto &[succeeded, message] = gpds::from_string<gpds::archiver_yaml>(
            document.toStdString(), scene, QSchematic::Scene::gpds_name);
        if (!succeeded) {
            QSocConsole::warn() << "Failed to restore the scene:"
                                << QString::fromStdString(message);
        }
    }
};

#endif // SCENEDOCUMENT_H
