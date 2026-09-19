// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCPRCMDOCUMENT_H
#define QSOCPRCMDOCUMENT_H

#include "common/qsocprcminput.h"

struct QSocPrcmDocument
{
    YAML::Node node;
    QString    file;
    /* Merged path prefix to original file and path prefix. */
    QMap<QString, QSocPrcmSource> origin;
};

struct QSocPrcmDocumentResult
{
    std::optional<QSocPrcmDocument> document;
    QList<QSocPrcmDiagnostic>       diagnostic;
};

class QSocPrcmDocumentLoader
{
public:
    static QSocPrcmDocumentResult load(const QStringList &files);
};

#endif // QSOCPRCMDOCUMENT_H
