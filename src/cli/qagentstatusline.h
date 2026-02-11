// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 Huang Rui <vowstar@gmail.com>

#ifndef QAGENTSTATUSLINE_H
#define QAGENTSTATUSLINE_H

#include <QElapsedTimer>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>

/**
 * @brief Dynamic status line with spinner for agent operations
 * @details Displays a spinning indicator and status text while agent is working.
 *          Uses ANSI escape sequences for in-place updates.
 */
class QAgentStatusLine : public QObject
{
    Q_OBJECT

public:
    /**
     * @brief Todo item structure for persistent display
     */
    struct TodoItem
    {
        int     id;
        QString title;
        QString priority;
        QString status; /* "done", "pending", "in_progress" */
    };

    /**
     * @brief Constructor
     * @param parent Parent QObject
     */
    explicit QAgentStatusLine(QObject *parent = nullptr);

    /**
     * @brief Destructor - ensures cleanup
     */
    ~QAgentStatusLine() override;

    /**
     * @brief Start the status line with initial message
     * @param status Initial status message (default: "Thinking...")
     */
    void start(const QString &status = "Thinking...");

    /**
     * @brief Update the status message
     * @param status New status message
     */
    void update(const QString &status);

    /**
     * @brief Increment tool call counter and update status
     * @param toolName Name of the tool being called
     * @param detail Optional detail (e.g., command for shell tools)
     */
    void toolCalled(const QString &toolName, const QString &detail = QString());

    /**
     * @brief Reset progress timer (call when new progress is detected)
     * @details Resets elapsed timer to clear slow/timeout warnings
     */
    void resetProgress();

    /**
     * @brief Stop the status line and clear it
     */
    void stop();

    /**
     * @brief Check if status line is currently active
     * @return True if active
     */
    bool isActive() const;

    /**
     * @brief Output content while keeping status line at bottom
     * @details Clears status line, prints content, then redraws status line.
     *          Content can include newlines and will scroll above the status.
     * @param content Content to output (will be printed as-is)
     */
    void printContent(const QString &content);

private slots:
    /**
     * @brief Timer callback to render spinner frame
     */
    void onTimer();

private:
    QTimer       *spinnerTimer = nullptr;
    QElapsedTimer stepElapsedTimer;  /* Current step timer (resets on tool call) */
    QElapsedTimer totalElapsedTimer; /* Total timer (from start) */
    int           spinnerIndex  = 0;
    int           dotIndex      = 0; /* For animated dots */
    int           toolCallCount = 0;
    QString       currentStatus;
    bool          active = false;

    /* Token tracking */
    qint64 inputTokens  = 0;
    qint64 outputTokens = 0;

    /* Thinking level for display */
    QString thinkingLevel;

    /* Pure ASCII spinner frames for maximum terminal compatibility */
    const QStringList spinnerFrames = {"-", "\\", "|", "/"};

    /* Animated dots: cycle through "   " -> ".  " -> ".. " -> "..." for equal width */
    const QStringList dotFrames = {"   ", ".  ", ".. ", "..."};

    /**
     * @brief Get terminal width
     * @return Terminal width in columns (default 80 if unavailable)
     */
    int getTerminalWidth() const;

    /**
     * @brief Render the current status line
     */
    void render();

    /**
     * @brief Clear the current line
     */
    void clearLine();

    /**
     * @brief Cached todo list for persistent display
     */
    QString todoListCache;

    /**
     * @brief Parsed TODO items for persistent display
     */
    QList<TodoItem> todoItems;

    /**
     * @brief Currently active TODO ID (-1 if none)
     */
    int activeTodoId = -1;

    /**
     * @brief Number of TODO lines currently displayed
     */
    int displayedTodoLineCount = 0;

    /**
     * @brief Buffered content waiting to be flushed by render()
     */
    QString pendingContent;

    /**
     * @brief Current input line text for display below status bar
     */
    QString inputLineText;

    /**
     * @brief Queued requests for persistent display above status bar
     */
    QStringList queuedRequests;

    /**
     * @brief The current incomplete line of streaming content (ephemeral zone)
     */
    QString currentPartialLine;

public:
    /**
     * @brief Update and display todo list
     * @param todoResult The result from any todo tool
     */
    void updateTodoDisplay(const QString &todoResult);

    /**
     * @brief Set TODO list for persistent display
     * @param items List of todo items
     */
    void setTodoList(const QList<TodoItem> &items);

    /**
     * @brief Mark a TODO as currently active (spinner in checkbox)
     * @param todoId ID of the active todo
     */
    void setActiveTodo(int todoId);

    /**
     * @brief Clear active TODO
     */
    void clearActiveTodo();

    /**
     * @brief Add a single TODO item to the list
     * @param item The todo item to add
     */
    void addTodoItem(const TodoItem &item);

    /**
     * @brief Update a TODO item's status by ID
     * @param todoId The ID of the todo item
     * @param newStatus The new status ("done", "pending", "in_progress")
     */
    void updateTodoStatus(int todoId, const QString &newStatus);

    /**
     * @brief Get a TODO item's title by ID
     * @param todoId The ID of the todo item
     * @return Title string, or empty if not found
     */
    QString getTodoTitle(int todoId) const;

    /**
     * @brief Update token usage counters
     * @param input Input (prompt) tokens
     * @param output Output (completion) tokens
     */
    void updateTokens(qint64 input, qint64 output);

    /**
     * @brief Set thinking level for status bar display
     * @param level Thinking level (empty=hidden, "low"/"medium"/"high")
     */
    void setThinkingLevel(const QString &level);

    /**
     * @brief Set the input line text for display below status bar
     * @param text Current user input text (empty to hide)
     */
    void setInputLine(const QString &text);

    /**
     * @brief Clear the input line display
     */
    void clearInputLine();

    /**
     * @brief Add a queued request to the persistent display
     * @param text The queued request text
     */
    void addQueuedRequest(const QString &text);

    /**
     * @brief Remove a queued request from the persistent display
     * @param text The request text to remove (first occurrence)
     */
    void removeQueuedRequest(const QString &text);

    /**
     * @brief Clear all queued requests from the display
     */
    void clearQueuedRequests();

private:
    /**
     * @brief Format number with SI prefix (k/M/G)
     * @param value Number to format
     * @return Compact string like "1.2k", "3.4M"
     */
    static QString formatNumber(qint64 value);

    /**
     * @brief Format duration compactly
     * @param seconds Duration in seconds
     * @return Compact string like "45s", "1:23", "1:23:45", "1d2:34"
     */
    static QString formatDuration(qint64 seconds);
};

#endif // QAGENTSTATUSLINE_H
