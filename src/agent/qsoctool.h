// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 Huang Rui <vowstar@gmail.com>

#ifndef QSOCTOOL_H
#define QSOCTOOL_H

#include <nlohmann/json.hpp>
#include <QMap>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QString>

using json = nlohmann::json;

/**
 * @brief Cancellation state for one tool invocation
 */
class QSocToolCallContext : public QObject
{
    Q_OBJECT

public:
    explicit QSocToolCallContext(QObject *owner = nullptr);

    bool isCancellationRequested() const;

signals:
    void cancellationRequested();

private:
    void requestCancellation();

    QPointer<QObject> owner_;
    bool              cancellationRequested_ = false;

    friend class QSocToolRegistry;
};

/**
 * @brief Base class for all agent tools
 * @details Abstract base class that defines the interface for tools
 *          that can be called by the QSocAgent during LLM interactions.
 */
class QSocTool : public QObject
{
    Q_OBJECT

public:
    /**
     * @brief Constructor
     * @param parent Parent QObject
     */
    explicit QSocTool(QObject *parent = nullptr);

    /**
     * @brief Virtual destructor
     */
    ~QSocTool() override;

    /**
     * @brief Get the tool name
     * @return Tool name used for function calling
     */
    virtual QString getName() const = 0;

    /**
     * @brief Get the tool description
     * @return Human-readable description of what the tool does
     */
    virtual QString getDescription() const = 0;

    /**
     * @brief Get the JSON Schema for tool parameters
     * @return JSON Schema describing the tool's parameters
     */
    virtual json getParametersSchema() const = 0;

    /**
     * @brief Execute the tool with given arguments
     * @param arguments JSON object containing the tool arguments
     * @return Result of the tool execution as a string
     */
    virtual QString execute(const json &arguments) = 0;

    /**
     * @brief Abort the current tool execution
     * @details Default implementation is a no-op. Override in tools that run
     *          long operations (e.g. bash processes) to support interruption.
     */
    virtual void abort();

    /**
     * @brief Whether the tool only observes state and never mutates it
     * @details Fail-closed default: tools are assumed to write unless they
     *          explicitly opt in. Plan mode uses this to permit read-only
     *          tools while blocking anything that could change the project,
     *          filesystem, or remote host. Shell tools stay false (their
     *          per-command safety is judged at dispatch instead).
     * @return true if the tool is side-effect-free
     */
    virtual bool isReadOnly() const { return false; }

    /**
     * @brief Get the tool definition in OpenAI function format
     * @return JSON object in OpenAI tool format
     */
    json getDefinition() const;

protected:
    /**
     * @brief Get the cancellation state for this invocation
     * @details Capture this at execute() entry. A shared tool can be
     *          re-entered while a nested event loop is running.
     * @return Current call context, or nullptr outside registry dispatch
     */
    QSocToolCallContext *currentCallContext() const;

private:
    QList<QPointer<QSocToolCallContext>> callContexts_;

    friend class QSocToolRegistry;
};

/**
 * @brief Registry for managing available tools
 * @details Observes a non-owning collection of tools and provides methods
 *          to register, retrieve, and execute live tools by name.
 */
class QSocToolRegistry : public QObject
{
    Q_OBJECT

public:
    /**
     * @brief Constructor
     * @param parent Parent QObject
     */
    explicit QSocToolRegistry(QObject *parent = nullptr);

    /**
     * @brief Destructor
     */
    ~QSocToolRegistry() override;

    /**
     * @brief Register a tool with the registry
     * @param tool Pointer to the tool to register
     */
    void registerTool(QSocTool *tool);

    /**
     * @brief Remove every current entry referring to a tool
     * @param tool Pointer previously passed to registerTool()
     * @return true when at least one matching entry was removed
     */
    bool unregisterTool(QSocTool *tool);

    /**
     * @brief Get a tool by name
     * @param name The name of the tool to retrieve
     * @return Pointer to the tool, or nullptr if not found
     */
    QSocTool *getTool(const QString &name) const;

    /**
     * @brief Check if a tool exists
     * @param name The name of the tool to check
     * @return true if tool exists, false otherwise
     */
    bool hasTool(const QString &name) const;

    /**
     * @brief Get all tool definitions for LLM
     * @return JSON array of tool definitions in OpenAI format
     */
    json getToolDefinitions() const;

    /**
     * @brief Execute a tool by name
     * @param name The name of the tool to execute
     * @param arguments JSON object containing tool arguments
     * @return Result of the tool execution
     */
    QString executeTool(const QString &name, const json &arguments, QObject *owner = nullptr);

    /**
     * @brief Get the number of registered tools
     * @return Number of tools in the registry
     */
    int count() const;

    /**
     * @brief Get list of all registered tool names
     * @return List of tool names
     */
    QStringList toolNames() const;

    /**
     * @brief Abort all currently executing tools
     * @details Calls abort() on every registered or in-flight tool
     */
    void abortAll();

    /**
     * @brief Cancel active calls belonging to one execution owner
     * @param owner Owner passed to executeTool()
     */
    void abortCalls(QObject *owner);

private:
    struct ActiveCall
    {
        ActiveCall(QSocTool *tool, QObject *owner)
            : tool(tool)
            , context(owner)
        {}

        QPointer<QSocTool>  tool;
        QSocToolCallContext context;
    };

    QMap<QString, QPointer<QSocTool>> tools_;
    QSet<ActiveCall *>                activeCalls_;
};

#endif // QSOCTOOL_H
