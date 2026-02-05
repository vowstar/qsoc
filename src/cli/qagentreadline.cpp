// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 Huang Rui <vowstar@gmail.com>

#include "cli/qagentreadline.h"

#include <QDir>
#include <QFileInfo>

#include <replxx.hxx>

QAgentReadline::QAgentReadline(QObject *parent)
    : QObject(parent)
    , replxx_(std::make_unique<replxx::Replxx>())
{
    initReplxx();
}

QAgentReadline::~QAgentReadline()
{
    /* Save history on destruction */
    if (!historyFile_.isEmpty()) {
        saveHistory();
    }
}

void QAgentReadline::initReplxx()
{
    /* Configure replxx defaults */
    replxx_->set_max_history_size(1000);
    replxx_->set_unique_history(true);

    /* Word break characters for completion context */
    replxx_->set_word_break_characters(" \t\n\r\v\f!\"#$%&'()*+,-./:;<=>?@[\\]^`{|}~");

    /* Hint settings */
    replxx_->set_max_hint_rows(3);
    replxx_->set_hint_delay(200);

    /* Completion settings */
    replxx_->set_double_tab_completion(false);
    replxx_->set_complete_on_empty(false);
    replxx_->set_beep_on_ambiguous_completion(false);

    /* Color based on terminal capability */
    replxx_->set_no_color(!termCap_.supportsColor());

    /* Setup key bindings */
    setupKeyBindings();

    /* Install window change handler for terminal resize */
    replxx_->install_window_change_handler();
}

void QAgentReadline::setupKeyBindings()
{
    /* Default bindings are good, but we can customize if needed */
    /* Ctrl+L to clear screen */
    replxx_->bind_key_internal(replxx::Replxx::KEY::control('L'), "clear_screen");

    /* Ctrl+W to delete word */
    replxx_->bind_key_internal(replxx::Replxx::KEY::control('W'), "kill_to_begining_of_word");
}

QString QAgentReadline::readLine(const QString &prompt)
{
    eof_ = false;

    const char *result = replxx_->input(prompt.toStdString());

    if (result == nullptr) {
        eof_ = true;
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
    return eof_;
}

void QAgentReadline::setHistoryFile(const QString &path)
{
    historyFile_ = path;

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
    if (historyFile_.isEmpty()) {
        return false;
    }

    return replxx_->history_load(historyFile_.toStdString());
}

bool QAgentReadline::saveHistory()
{
    if (historyFile_.isEmpty()) {
        return false;
    }

    return replxx_->history_save(historyFile_.toStdString());
}

void QAgentReadline::addHistory(const QString &line)
{
    replxx_->history_add(line.toStdString());

    /* Auto-save to file if configured */
    if (!historyFile_.isEmpty()) {
        replxx_->history_sync(historyFile_.toStdString());
    }
}

void QAgentReadline::clearHistory()
{
    replxx_->history_clear();
}

int QAgentReadline::historySize() const
{
    return replxx_->history_size();
}

void QAgentReadline::setMaxHistorySize(int size)
{
    replxx_->set_max_history_size(size);
}

void QAgentReadline::setCompletionCallback(CompletionCallback callback)
{
    completionCallback_ = std::move(callback);

    if (completionCallback_) {
        replxx_->set_completion_callback(
            [this](const std::string &input, int &contextLen) -> replxx::Replxx::completions_t {
                replxx::Replxx::completions_t completions;

                QStringList results = completionCallback_(QString::fromStdString(input), contextLen);

                for (const QString &result : results) {
                    completions.emplace_back(result.toStdString());
                }

                return completions;
            });
    }
}

void QAgentReadline::setHintCallback(HintCallback callback)
{
    hintCallback_ = std::move(callback);

    if (hintCallback_) {
        replxx_->set_hint_callback(
            [this](const std::string &input, int &contextLen, replxx::Replxx::Color &color)
                -> replxx::Replxx::hints_t {
                replxx::Replxx::hints_t hints;
                color = replxx::Replxx::Color::GRAY;

                QStringList results = hintCallback_(QString::fromStdString(input), contextLen);

                for (const QString &result : results) {
                    hints.emplace_back(result.toStdString());
                }

                return hints;
            });
    }
}

void QAgentReadline::setWordBreakCharacters(const QString &chars)
{
    replxx_->set_word_break_characters(chars.toUtf8().constData());
}

void QAgentReadline::setColorEnabled(bool enabled)
{
    replxx_->set_no_color(!enabled);
}

void QAgentReadline::setUniqueHistory(bool enabled)
{
    replxx_->set_unique_history(enabled);
}

void QAgentReadline::print(const QString &text)
{
    replxx_->print("%s", text.toUtf8().constData());
}

void QAgentReadline::clearScreen()
{
    replxx_->clear_screen();
}

const QTerminalCapability &QAgentReadline::terminalCapability() const
{
    return termCap_;
}
