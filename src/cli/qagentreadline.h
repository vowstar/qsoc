// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 Huang Rui <vowstar@gmail.com>

#ifndef QAGENTREADLINE_H
#define QAGENTREADLINE_H

#include "cli/qterminalcapability.h"

#include <functional>
#include <memory>
#include <QObject>
#include <QString>
#include <QStringList>

namespace replxx {
class Replxx;
}

/**
 * @brief Readline-like input handler for agent CLI with history and completion
 * @details Wraps replxx library to provide enhanced line editing:
 *          - Line editing (backspace, Ctrl+A/E/K, arrow keys)
 *          - History browsing (up/down arrows)
 *          - History search (Ctrl+R)
 *          - Tab completion
 *          - History persistence to project directory
 */
class QAgentReadline : public QObject
{
    Q_OBJECT

public:
    /**
     * @brief Completion callback type
     * @param input Current input text
     * @param contextLen Length of context for completion
     * @return List of completion suggestions
     */
    using CompletionCallback = std::function<QStringList(const QString &input, int &contextLen)>;

    /**
     * @brief Hint callback type
     * @param input Current input text
     * @param contextLen Length of context for hint
     * @return List of hint suggestions
     */
    using HintCallback = std::function<QStringList(const QString &input, int &contextLen)>;

    /**
     * @brief Constructor
     * @param parent Parent QObject
     */
    explicit QAgentReadline(QObject *parent = nullptr);

    /**
     * @brief Destructor
     */
    ~QAgentReadline() override;

    /**
     * @brief Read a line of input with the given prompt
     * @param prompt The prompt to display
     * @return User input, or empty QString if EOF/error
     */
    QString readLine(const QString &prompt);

    /**
     * @brief Check if last input was EOF (Ctrl+D)
     * @return True if last readLine() returned due to EOF
     */
    bool isEof() const;

    /**
     * @brief Set the history file path
     * @param path Path to history file (will be created if not exists)
     */
    void setHistoryFile(const QString &path);

    /**
     * @brief Load history from file
     * @return True if history was loaded successfully
     */
    bool loadHistory();

    /**
     * @brief Save history to file
     * @return True if history was saved successfully
     */
    bool saveHistory();

    /**
     * @brief Add entry to history
     * @param line Line to add to history
     */
    void addHistory(const QString &line);

    /**
     * @brief Clear history
     */
    void clearHistory();

    /**
     * @brief Get history size
     * @return Number of entries in history
     */
    int historySize() const;

    /**
     * @brief Set maximum history size
     * @param size Maximum number of history entries
     */
    void setMaxHistorySize(int size);

    /**
     * @brief Set completion callback
     * @param callback Function to generate completions
     */
    void setCompletionCallback(CompletionCallback callback);

    /**
     * @brief Set hint callback
     * @param callback Function to generate hints
     */
    void setHintCallback(HintCallback callback);

    /**
     * @brief Set word break characters for completion
     * @param chars Characters that break words
     */
    void setWordBreakCharacters(const QString &chars);

    /**
     * @brief Enable or disable colors
     * @param enabled True to enable colors
     */
    void setColorEnabled(bool enabled);

    /**
     * @brief Enable or disable unique history (no duplicates)
     * @param enabled True to filter duplicates
     */
    void setUniqueHistory(bool enabled);

    /**
     * @brief Print text to output (handles ANSI sequences properly)
     * @param text Text to print
     */
    void print(const QString &text);

    /**
     * @brief Clear the screen
     */
    void clearScreen();

    /**
     * @brief Get terminal capability info
     * @return Reference to terminal capability object
     */
    const QTerminalCapability &terminalCapability() const;

private:
    std::unique_ptr<replxx::Replxx> replxxInstance;
    QTerminalCapability             termCap;
    QString                         historyFilePath;
    bool                            eofFlag = false;
    CompletionCallback              completionCallback;
    HintCallback                    hintCallback;

    /**
     * @brief Initialize replxx with default settings
     */
    void initReplxx();

    /**
     * @brief Setup default key bindings
     */
    void setupKeyBindings();
};

#endif // QAGENTREADLINE_H
