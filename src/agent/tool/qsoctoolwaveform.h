// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCTOOLWAVEFORM_H
#define QSOCTOOLWAVEFORM_H

#include "agent/qsoctool.h"

#include <memory>

class QSocPathContext;
class QSocRemoteConnection;

class QSocToolWaveformRead : public QSocTool
{
    Q_OBJECT

public:
    explicit QSocToolWaveformRead(
        QObject              *parent = nullptr,
        QSocPathContext      *paths  = nullptr,
        QSocRemoteConnection *remote = nullptr);
    ~QSocToolWaveformRead() override;

    QString getName() const override;
    QString getDescription() const override;
    json    getParametersSchema() const override;
    QString execute(const json &arguments) override;
    void    abort() override;
    bool    isReadOnly() const override { return true; }

private:
    struct State;
    std::unique_ptr<State> state_;
    QSocPathContext       *paths_  = nullptr;
    QSocRemoteConnection  *remote_ = nullptr;
};

#endif
