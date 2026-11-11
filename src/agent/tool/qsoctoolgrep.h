// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCTOOLGREP_H
#define QSOCTOOLGREP_H

#include "agent/qsoctool.h"

class QSocWorkspaceFs;

class QSocToolGrep : public QSocTool
{
    Q_OBJECT

public:
    explicit QSocToolGrep(QObject *parent = nullptr, QSocWorkspaceFs *fs = nullptr);

    QString getName() const override;
    QString getDescription() const override;
    json    getParametersSchema() const override;
    QString execute(const json &arguments) override;
    bool    isReadOnly() const override { return true; }
    void    setFilesystem(QSocWorkspaceFs *fs) { fs_ = fs; }

private:
    QSocWorkspaceFs *fs_;
};

#endif // QSOCTOOLGREP_H
