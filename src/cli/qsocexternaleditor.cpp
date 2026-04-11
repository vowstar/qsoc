// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "cli/qsocexternaleditor.h"

#include <QByteArray>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QTemporaryFile>

QString QSocExternalEditor::resolveEditor()
{
    const QByteArray envEditor = qgetenv("EDITOR");
    if (!envEditor.isEmpty()) {
        return QString::fromLocal8Bit(envEditor);
    }
    return QStringLiteral("vi");
}

bool QSocExternalEditor::editText(const QString &current, QString &result, QString &error)
{
    error.clear();

    /* QTemporaryFile uses auto-cleanup; keep it in scope for the whole call. */
    QTemporaryFile tempFile(QStringLiteral("qsoc-prompt-XXXXXX.txt"));
    tempFile.setAutoRemove(true);
    if (!tempFile.open()) {
        error = QStringLiteral("Failed to create temporary file: %1").arg(tempFile.errorString());
        return false;
    }

    const QString tempPath = tempFile.fileName();

    /* Write current text to the tempfile, always terminated with a newline
     * so editors that enforce trailing-newline conventions don't silently
     * change the content. We strip exactly one trailing newline on read
     * so the round-trip is transparent when the user didn't type one. */
    {
        QByteArray bytes = current.toUtf8();
        if (!bytes.endsWith('\n')) {
            bytes.append('\n');
        }
        if (tempFile.write(bytes) != bytes.size()) {
            error = QStringLiteral("Failed to write temporary file: %1").arg(tempFile.errorString());
            return false;
        }
        tempFile.close();
    }

    /* Parse $EDITOR into command + args — users often set things like
     * EDITOR="code -w" or EDITOR="emacsclient -nw". Split on whitespace
     * as a pragmatic middle ground (no shell evaluation, no quoting). */
    QString           editorCmd   = resolveEditor();
    const QStringList editorParts = editorCmd.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    if (editorParts.isEmpty()) {
        error = QStringLiteral("EDITOR is not set and no fallback is available");
        return false;
    }
    const QString &program = editorParts.first();
    QStringList    args    = editorParts.mid(1);
    args.append(tempPath);

    /* QProcess::execute is blocking and inherits the parent's stdin/stdout/stderr,
     * which is exactly what we need so terminal editors can draw over our TUI
     * after the compositor has paused the alt-screen. */
    const int exitCode = QProcess::execute(program, args);
    if (exitCode < 0) {
        error = QStringLiteral("Failed to launch editor '%1' (exit code %2)")
                    .arg(program)
                    .arg(exitCode);
        return false;
    }
    if (exitCode != 0) {
        error = QStringLiteral("Editor '%1' exited with status %2").arg(program).arg(exitCode);
        return false;
    }

    /* Read the edited content back. */
    QFile readFile(tempPath);
    if (!readFile.open(QIODevice::ReadOnly)) {
        error = QStringLiteral("Failed to read edited file: %1").arg(readFile.errorString());
        return false;
    }
    const QByteArray bytes = readFile.readAll();
    readFile.close();

    result = QString::fromUtf8(bytes);
    /* Editors typically append a trailing newline; strip exactly one so the
     * prompt doesn't end with a dangling blank line the user didn't type. */
    if (result.endsWith(QLatin1Char('\n'))) {
        result.chop(1);
    }
    return true;
}
