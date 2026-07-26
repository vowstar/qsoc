// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef PRCPROPERTYCOMMANDS_H
#define PRCPROPERTYCOMMANDS_H

#include "gui/prcwindow/prcprimitiveitem.h"
#include "gui/prcwindow/prcscene.h"

#include <memory>

#include <QString>
#include <QUndoCommand>

namespace PrcLibrary {

/**
 * @brief Undo command for a primitive's configuration.
 * @details The configuration dialog edits the item in place. Recording the
 *          parameters on both sides keeps the edit on the scene's own history,
 *          so it interleaves correctly with placing and deleting items.
 */
class PrcParamsCommand : public QUndoCommand
{
public:
    /**
     * @brief Constructor.
     * @param[in] item Primitive being configured.
     * @param[in] before Parameters before the dialog.
     * @param[in] after Parameters after the dialog.
     * @param[in] text Label shown in the undo history.
     */
    PrcParamsCommand(
        std::shared_ptr<PrcPrimitiveItem> item,
        PrcParams                         before,
        PrcParams                         after,
        const QString                    &text)
        : m_item(std::move(item))
        , m_before(std::move(before))
        , m_after(std::move(after))
    {
        setText(text);
    }

    void undo() override { apply(m_before); }
    void redo() override { apply(m_after); }

private:
    void apply(const PrcParams &params)
    {
        if (m_item) {
            m_item->setParams(params);
        }
    }

    std::shared_ptr<PrcPrimitiveItem> m_item;
    PrcParams                         m_before;
    PrcParams                         m_after;
};

/**
 * @brief Undo command for the operations configured on a wire.
 * @details Link settings and the net name that advertises them change
 *          together, so one command carries both.
 */
class PrcLinkParamsCommand : public QUndoCommand
{
public:
    /**
     * @brief State of one link, as stored in the scene.
     */
    struct LinkState
    {
        bool            present = false; /**< Whether the scene holds parameters */
        ClockLinkParams params;          /**< The parameters themselves */
        QString         netName;         /**< Wire net name carrying the markers */
    };

    /**
     * @brief Constructor.
     * @param[in] scene Scene owning the link parameters.
     * @param[in] wireNet Net the link belongs to.
     * @param[in] key Key the parameters are stored under.
     * @param[in] before State before the dialog.
     * @param[in] after State after the dialog.
     * @param[in] text Label shown in the undo history.
     */
    PrcLinkParamsCommand(
        PrcScene                   *scene,
        QSchematic::Items::WireNet *wireNet,
        QString                     key,
        LinkState                   before,
        LinkState                   after,
        const QString              &text)
        : m_scene(scene)
        , m_wireNet(wireNet)
        , m_key(std::move(key))
        , m_before(std::move(before))
        , m_after(std::move(after))
    {
        setText(text);
    }

    void undo() override { apply(m_before); }
    void redo() override { apply(m_after); }

private:
    void apply(const LinkState &state)
    {
        if (!m_scene) {
            return;
        }
        if (state.present) {
            m_scene->setLinkParameters(m_key, state.params);
        } else {
            m_scene->removeLinkParameters(m_key);
        }
        if (m_wireNet) {
            m_wireNet->set_name(state.netName);
        }
    }

    PrcScene                   *m_scene   = nullptr;
    QSchematic::Items::WireNet *m_wireNet = nullptr;
    QString                     m_key;
    LinkState                   m_before;
    LinkState                   m_after;
};

/**
 * @brief Undo command for a controller definition.
 * @details Covers both editing a controller and deleting it, since a deletion
 *          is the same record with nothing on the other side.
 */
class PrcControllerCommand : public QUndoCommand
{
public:
    /**
     * @brief State of one controller definition.
     */
    struct ControllerState
    {
        bool               present = false; /**< Whether the controller exists */
        ClockControllerDef clock;           /**< Clock definition, when applicable */
        ResetControllerDef reset;           /**< Reset definition, when applicable */
        PowerControllerDef power;           /**< Power definition, when applicable */
    };

    /**
     * @brief Constructor.
     * @param[in] scene Scene owning the controllers.
     * @param[in] type Controller family.
     * @param[in] name Controller name.
     * @param[in] before State before the dialog.
     * @param[in] after State after the dialog.
     * @param[in] text Label shown in the undo history.
     */
    PrcControllerCommand(
        PrcScene                *scene,
        PrcScene::ControllerType type,
        QString                  name,
        ControllerState          before,
        ControllerState          after,
        const QString           &text)
        : m_scene(scene)
        , m_type(type)
        , m_name(std::move(name))
        , m_before(std::move(before))
        , m_after(std::move(after))
    {
        setText(text);
    }

    void undo() override { apply(m_before); }
    void redo() override { apply(m_after); }

private:
    void apply(const ControllerState &state)
    {
        if (!m_scene) {
            return;
        }
        switch (m_type) {
        case PrcScene::ClockCtrl:
            if (state.present) {
                m_scene->setClockController(m_name, state.clock);
            } else {
                m_scene->removeClockController(m_name);
            }
            break;
        case PrcScene::ResetCtrl:
            if (state.present) {
                m_scene->setResetController(m_name, state.reset);
            } else {
                m_scene->removeResetController(m_name);
            }
            break;
        case PrcScene::PowerCtrl:
            if (state.present) {
                m_scene->setPowerController(m_name, state.power);
            } else {
                m_scene->removePowerController(m_name);
            }
            break;
        }
    }

    PrcScene                *m_scene = nullptr;
    PrcScene::ControllerType m_type;
    QString                  m_name;
    ControllerState          m_before;
    ControllerState          m_after;
};

} // namespace PrcLibrary

#endif // PRCPROPERTYCOMMANDS_H
