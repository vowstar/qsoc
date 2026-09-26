// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "qsocsmtinput.h"
#include "qsocsmtservice.h"

#include <stdexcept>
#include <QSet>

namespace {

struct Node
{
    QByteArray        atom;
    std::vector<Node> children;
    int               begin  = 0;
    int               end    = 0;
    bool              list   = false;
    bool              quoted = false;
};

class Parser
{
public:
    explicit Parser(const QByteArray &input)
        : source(input)
    {}

    std::vector<Node> commands()
    {
        std::vector<Node> result;
        whitespace();
        while (position < source.size()) {
            if (result.size() >= 10000 || source.at(position) != '(') {
                throw std::runtime_error("Expected a top-level command within the command limit");
            }
            result.push_back(node(0));
            whitespace();
        }
        return result;
    }

private:
    void whitespace()
    {
        while (position < source.size()) {
            const char ch = source.at(position);
            if (ch == ';') {
                while (position < source.size() && source.at(position) != '\n') {
                    ++position;
                }
            } else if (ch == ' ' || ch == '\n' || ch == '\r' || ch == '\t') {
                ++position;
            } else {
                return;
            }
        }
    }

    Node quotedAtom(char delimiter)
    {
        Node result;
        result.begin  = position++;
        result.quoted = true;
        while (position < source.size()) {
            const char ch = source.at(position++);
            if (ch == delimiter) {
                if (delimiter == '"' && position < source.size() && source.at(position) == '"') {
                    ++position;
                    continue;
                }
                result.end  = position;
                result.atom = source.mid(result.begin + 1, result.end - result.begin - 2);
                return result;
            }
            if (delimiter == '|' && ch == '\\') {
                throw std::runtime_error("Backslash is not supported in quoted symbols");
            }
        }
        throw std::runtime_error("Unterminated string or quoted symbol");
    }

    Node node(int depth)
    {
        if (depth >= 64 || position >= source.size()) {
            throw std::runtime_error("Unterminated or excessively nested expression");
        }
        const char first = source.at(position);
        if (first == '"' || first == '|') {
            return quotedAtom(first);
        }
        Node result;
        result.begin = position;
        if (first != '(') {
            while (position < source.size()) {
                const char ch = source.at(position);
                if (ch == '(' || ch == ')' || ch == ';' || ch == ' ' || ch == '\n' || ch == '\r'
                    || ch == '\t' || ch == '"' || ch == '|') {
                    break;
                }
                ++position;
            }
            if (position == result.begin) {
                throw std::runtime_error("Unexpected token");
            }
            result.end  = position;
            result.atom = source.mid(result.begin, result.end - result.begin);
            return result;
        }
        result.list = true;
        ++position;
        whitespace();
        while (position < source.size() && source.at(position) != ')') {
            result.children.push_back(node(depth + 1));
            whitespace();
        }
        if (position >= source.size()) {
            throw std::runtime_error("Unterminated expression");
        }
        result.end = ++position;
        return result;
    }

    const QByteArray &source;
    int               position = 0;
};

void checkAnnotations(const Node &node, const Node *allowed)
{
    if (!node.quoted && node.atom == ":named" && &node != allowed) {
        throw std::runtime_error("Only top-level assertion names are supported");
    }
    for (const auto &child : node.children) {
        checkAnnotations(child, allowed);
    }
}

void collectLabel(const Node &command, QSocSmtInput &result)
{
    const Node *attribute = nullptr;
    const Node &assertion = command.children.at(1);
    if (assertion.list && !assertion.children.empty() && assertion.children.front().atom == "!") {
        for (size_t i = 2; i < assertion.children.size(); ++i) {
            const auto &item = assertion.children[i];
            if (item.quoted || item.atom != ":named") {
                continue;
            }
            if (attribute || i + 1 >= assertion.children.size()) {
                throw std::runtime_error("Invalid assertion name");
            }
            attribute         = &item;
            const auto &label = assertion.children[++i];
            if (label.list || label.atom.isEmpty() || label.atom.startsWith(':')) {
                throw std::runtime_error("Invalid assertion name");
            }
            const auto name = QString::fromUtf8(label.atom);
            if (result.labels.contains(name)) {
                throw std::runtime_error("Duplicate assertion name");
            }
            result.labels.append(name);
        }
    }
    checkAnnotations(command, attribute);
}

void inspect(
    const Node       &command,
    const QByteArray &source,
    bool              optimize,
    bool             &hasCommand,
    QSocSmtInput     &result)
{
    static const QSet<QByteArray> allowed
        = {"set-logic",
           "declare-sort",
           "declare-const",
           "declare-fun",
           "define-fun",
           "assert",
           "minimize",
           "maximize"};
    if (command.children.empty() || command.children.front().list
        || command.children.front().quoted) {
        throw std::runtime_error("Invalid command name");
    }
    const auto &name = command.children.front().atom;
    if (!allowed.contains(name)) {
        throw std::runtime_error("Unsupported SMT command");
    }
    if (name == "set-logic" && hasCommand) {
        throw std::runtime_error("set-logic is allowed only as the first command");
    }
    hasCommand = true;
    if (name == "assert") {
        if (command.children.size() != 2) {
            throw std::runtime_error("assert requires one expression");
        }
        collectLabel(command, result);
        return;
    }
    checkAnnotations(command, nullptr);
    if (name != "minimize" && name != "maximize") {
        return;
    }
    if (!optimize || command.children.size() != 2 || result.objectives.size() >= 16) {
        throw std::runtime_error("Optimize requires from 1 to 16 unary objectives");
    }
    const auto &expression = command.children.at(1);
    result.objectives.push_back(
        {name == "maximize",
         QString::fromUtf8(source.mid(expression.begin, expression.end - expression.begin))});
}

} // namespace

QSocSmtInput QSocSmtInput::scan(const QByteArray &input, bool optimize)
{
    QSocSmtInput result;
    if (input.size() > QSocSmtService::inputLimit || QString::fromUtf8(input).toUtf8() != input) {
        result.error = QStringLiteral("Invalid input size or UTF-8 encoding");
        return result;
    }
    for (const char ch : input) {
        if ((static_cast<unsigned char>(ch) < 32 && ch != '\t' && ch != '\r' && ch != '\n')
            || ch == 127) {
            result.error = QStringLiteral("Unsupported control character");
            return result;
        }
    }
    try {
        Parser     parser(input);
        const auto commands   = parser.commands();
        bool       hasCommand = false;
        for (const auto &command : commands) {
            inspect(command, input, optimize, hasCommand, result);
        }
        if (commands.empty() || (optimize && result.objectives.empty())) {
            throw std::runtime_error("The request contains no command or no optimization target");
        }
    } catch (const std::runtime_error &error) {
        result.error = QString::fromUtf8(error.what());
    }
    return result;
}
