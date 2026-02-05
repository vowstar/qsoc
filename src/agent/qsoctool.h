// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 Huang Rui <vowstar@gmail.com>

#ifndef QSOCTOOL_H
#define QSOCTOOL_H

#include <nlohmann/json.hpp>
#include <QMap>
#include <QObject>
#include <QString>

using json = nlohmann::json;

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
     * @brief Get the tool definition in OpenAI function format
     * @return JSON object in OpenAI tool format
     */
    json getDefinition() const;
};

/**
 * @brief Registry for managing available tools
 * @details Maintains a collection of tools and provides methods
 *          to register, retrieve, and execute tools by name.
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
    QString executeTool(const QString &name, const json &arguments);

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

private:
    QMap<QString, QSocTool *> tools_;
};

#endif // QSOCTOOL_H
