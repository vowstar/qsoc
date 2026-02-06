// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 Huang Rui <vowstar@gmail.com>

#include "cli/qagentreadline.h"

#include <QDir>
#include <QFileInfo>

#include <replxx.hxx>

QAgentReadline::QAgentReadline(QObject *parent)
    : QObject(parent)
    , replxxInstance(std::make_unique<replxx::Replxx>())
{
    initReplxx();
}

QAgentReadline::~QAgentReadline()
{
    /* Save history on destruction */
    if (!historyFilePath.isEmpty()) {
        saveHistory();
    }
}

void QAgentReadline::initReplxx()
{
    /* Configure replxx defaults */
    replxxInstance->set_max_history_size(1000);
    replxxInstance->set_unique_history(true);

    /* Word break characters for completion context */
    replxxInstance->set_word_break_characters(" \t\n\r\v\f!\"#$%&'()*+,-./:;<=>?@[\\]^`{|}~");

    /* Hint settings */
    replxxInstance->set_max_hint_rows(3);
    replxxInstance->set_hint_delay(200);

    /* Completion settings */
    replxxInstance->set_double_tab_completion(false);
    replxxInstance->set_complete_on_empty(false);
    replxxInstance->set_beep_on_ambiguous_completion(false);

    /* Color based on terminal capability */
    replxxInstance->set_no_color(!termCap.supportsColor());

    /* Setup key bindings */
    setupKeyBindings();

    /* Install window change handler for terminal resize */
    replxxInstance->install_window_change_handler();
}

void QAgentReadline::setupKeyBindings()
{
    /* Default bindings are good, but we can customize if needed */
    /* Ctrl+L to clear screen */
    replxxInstance->bind_key_internal(replxx::Replxx::KEY::control('L'), "clear_screen");

    /* Ctrl+W to delete word */
    replxxInstance->bind_key_internal(replxx::Replxx::KEY::control('W'), "kill_to_begining_of_word");
}

QString QAgentReadline::readLine(const QString &prompt)
{
    eofFlag = false;

    const char *result = replxxInstance->input(prompt.toStdString());

    if (result == nullptr) {
        eofFlag = true;
        return QString();
    }

    QString line = QString::fromUtf8(result);

    /* Add non-empty lines to history */
    if (!line.trimmed().isEmpty()) {
        addHistory(line);
    }

    return line;
}

bool QAgentReadline::isEof() const
{
    return eofFlag;
}

void QAgentReadline::setHistoryFile(const QString &path)
{
    historyFilePath = path;

    /* Ensure directory exists */
    QFileInfo fileInfo(path);
    QDir      directory = fileInfo.dir();
    if (!directory.exists()) {
        directory.mkpath(".");
    }

    /* Load existing history */
    loadHistory();
}

bool QAgentReadline::loadHistory()
{
    if (historyFilePath.isEmpty()) {
        return false;
    }

    return replxxInstance->history_load(historyFilePath.toStdString());
}

bool QAgentReadline::saveHistory()
{
    if (historyFilePath.isEmpty()) {
        return false;
    }

    return replxxInstance->history_save(historyFilePath.toStdString());
}

void QAgentReadline::addHistory(const QString &line)
{
    replxxInstance->history_add(line.toStdString());

    /* Auto-save to file if configured */
    if (!historyFilePath.isEmpty()) {
        replxxInstance->history_sync(historyFilePath.toStdString());
    }
}

void QAgentReadline::clearHistory()
{
    replxxInstance->history_clear();
}

int QAgentReadline::historySize() const
{
    return replxxInstance->history_size();
}

void QAgentReadline::setMaxHistorySize(int size)
{
    replxxInstance->set_max_history_size(size);
}

void QAgentReadline::setCompletionCallback(CompletionCallback callback)
{
    completionCallback = std::move(callback);

    if (completionCallback) {
        replxxInstance->set_completion_callback(
            [this](const std::string &input, int &contextLen) -> replxx::Replxx::completions_t {
                replxx::Replxx::completions_t completions;

                QStringList results = completionCallback(QString::fromStdString(input), contextLen);

                for (const QString &result : results) {
                    completions.emplace_back(result.toStdString());
                }

                return completions;
            });
    }
}

void QAgentReadline::setHintCallback(HintCallback callback)
{
    hintCallback = std::move(callback);

    if (hintCallback) {
        replxxInstance->set_hint_callback(
            [this](const std::string &input, int &contextLen, replxx::Replxx::Color &color)
                -> replxx::Replxx::hints_t {
                replxx::Replxx::hints_t hints;
                color = replxx::Replxx::Color::GRAY;

                QStringList results = hintCallback(QString::fromStdString(input), contextLen);

                for (const QString &result : results) {
                    hints.emplace_back(result.toStdString());
                }

                return hints;
            });
    }
}

void QAgentReadline::setWordBreakCharacters(const QString &chars)
{
    replxxInstance->set_word_break_characters(chars.toUtf8().constData());
}

void QAgentReadline::setColorEnabled(bool enabled)
{
    replxxInstance->set_no_color(!enabled);
}

void QAgentReadline::setUniqueHistory(bool enabled)
{
    replxxInstance->set_unique_history(enabled);
}

void QAgentReadline::print(const QString &text)
{
    replxxInstance->print("%s", text.toUtf8().constData());
}

void QAgentReadline::clearScreen()
{
    replxxInstance->clear_screen();
}

const QTerminalCapability &QAgentReadline::terminalCapability() const
{
    return termCap;
}
