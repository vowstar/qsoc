// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef SNAPSHOTCOMMAND_H
#define SNAPSHOTCOMMAND_H

#include <functional>
#include <utility>

#include <QString>
#include <QUndoCommand>
#include <QUndoStack>

/**
 * @brief Undo command that swaps a whole document snapshot.
 * @details Bulk edits such as a netlist import or an auto-match touch dozens
 *          of objects at once. Expressing each of them as its own command
 *          would mean rewriting the operation; capturing the document before
 *          and after gives the same user-visible result, one step of undo,
 *          without touching the operation at all.
 *
 *          Snapshot must be copyable and must not alias the live document.
 *          For YAML-backed values that means deep copying the node.
 */
template<typename Snapshot>
class SnapshotCommand : public QUndoCommand
{
public:
    using Restore = std::function<void(const Snapshot &)>;

    /**
     * @brief Constructor.
     * @param[in] before Document state before the operation.
     * @param[in] after Document state after the operation.
     * @param[in] restore Applies a snapshot to the live document.
     * @param[in] text Label shown in the undo history.
     */
    SnapshotCommand(Snapshot before, Snapshot after, Restore restore, const QString &text)
        : m_before(std::move(before))
        , m_after(std::move(after))
        , m_restore(std::move(restore))
    {
        setText(text);
    }

    void undo() override
    {
        if (m_restore) {
            m_restore(m_before);
        }
    }

    void redo() override
    {
        /* QUndoStack::push calls redo() immediately, but the operation has
           already run by then and the document is in the after state. */
        if (m_firstRedo) {
            m_firstRedo = false;
            return;
        }
        if (m_restore) {
            m_restore(m_after);
        }
    }

private:
    Snapshot m_before;
    Snapshot m_after;
    Restore  m_restore;
    bool     m_firstRedo = true;
};

/**
 * @brief Scope guard that turns a bulk edit into one undo step.
 * @details Captures the document on construction and pushes a snapshot
 *          command on destruction. Call cancel() when the operation bailed
 *          out, so an aborted dialog leaves no entry in the history.
 */
template<typename Snapshot>
class SnapshotScope
{
public:
    using Capture = std::function<Snapshot()>;
    using Restore = std::function<void(const Snapshot &)>;

    /**
     * @brief Constructor.
     * @param[in] stack Undo stack receiving the command.
     * @param[in] capture Produces a snapshot of the live document.
     * @param[in] restore Applies a snapshot to the live document.
     * @param[in] text Label shown in the undo history.
     */
    SnapshotScope(QUndoStack *stack, Capture capture, Restore restore, QString text)
        : m_stack(stack)
        , m_capture(std::move(capture))
        , m_restore(std::move(restore))
        , m_text(std::move(text))
    {
        if (m_stack && m_capture) {
            m_before = m_capture();
        } else {
            m_cancelled = true;
        }
    }

    ~SnapshotScope()
    {
        if (m_cancelled || !m_stack || !m_capture) {
            return;
        }
        Snapshot after = m_capture();
        m_stack->push(
            new SnapshotCommand<Snapshot>(std::move(m_before), std::move(after), m_restore, m_text));
    }

    SnapshotScope(const SnapshotScope &)            = delete;
    SnapshotScope &operator=(const SnapshotScope &) = delete;

    /**
     * @brief Drop the pending command.
     * @details Used when the operation did not change the document.
     */
    void cancel() { m_cancelled = true; }

private:
    QUndoStack *m_stack = nullptr;
    Capture     m_capture;
    Restore     m_restore;
    QString     m_text;
    Snapshot    m_before{};
    bool        m_cancelled = false;
};

#endif // SNAPSHOTCOMMAND_H
