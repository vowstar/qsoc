// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/qsocgoal.h"

#include <nlohmann/json.hpp>
#include <yaml-cpp/yaml.h>

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QUuid>

namespace {

constexpr auto kProjectRelativeYamlPath = ".qsoc/goal.yml";
constexpr auto kProjectRelativeLogPath  = ".qsoc/goal_log.jsonl";

QString stds(const std::string &raw)
{
    return QString::fromStdString(raw);
}

std::string asStd(const QString &str)
{
    return str.toStdString();
}

QByteArray emitYaml(const YAML::Node &node)
{
    YAML::Emitter emitter;
    emitter.SetIndent(2);
    emitter << node;
    QByteArray payload(emitter.c_str(), static_cast<int>(emitter.size()));
    if (!payload.endsWith('\n')) {
        payload.append('\n');
    }
    return payload;
}

} // namespace

QString qSocGoalStatusToString(QSocGoalStatus status)
{
    switch (status) {
    case QSocGoalStatus::Active:
        return QStringLiteral("active");
    case QSocGoalStatus::Paused:
        return QStringLiteral("paused");
    case QSocGoalStatus::BudgetLimited:
        return QStringLiteral("budget_limited");
    case QSocGoalStatus::Complete:
        return QStringLiteral("complete");
    }
    return {};
}

std::optional<QSocGoalStatus> qSocGoalStatusFromString(const QString &raw)
{
    if (raw == QStringLiteral("active")) {
        return QSocGoalStatus::Active;
    }
    if (raw == QStringLiteral("paused")) {
        return QSocGoalStatus::Paused;
    }
    if (raw == QStringLiteral("budget_limited")) {
        return QSocGoalStatus::BudgetLimited;
    }
    if (raw == QStringLiteral("complete")) {
        return QSocGoalStatus::Complete;
    }
    return std::nullopt;
}

QSocGoalCatalog::QSocGoalCatalog(QObject *parent)
    : QObject(parent)
{}

QString QSocGoalCatalog::projectFilePath() const
{
    if (projectDir_.isEmpty()) {
        return {};
    }
    return QDir(projectDir_).absoluteFilePath(QString::fromLatin1(kProjectRelativeYamlPath));
}

QString QSocGoalCatalog::logFilePath() const
{
    if (projectDir_.isEmpty()) {
        return {};
    }
    return QDir(projectDir_).absoluteFilePath(QString::fromLatin1(kProjectRelativeLogPath));
}

void QSocGoalCatalog::load(const QString &projectDir)
{
    projectDir_ = projectDir;
    current_.reset();
    const QString path = projectFilePath();
    if (path.isEmpty() || !QFileInfo::exists(path)) {
        return;
    }
    try {
        const YAML::Node root = YAML::LoadFile(path.toStdString());
        if (!root || !root.IsMap() || !root["goal"]) {
            return;
        }
        const YAML::Node node = root["goal"];
        if (!node.IsMap() || !node["objective"]) {
            return;
        }
        QSocGoal goal;
        goal.id = stds(node["id"].as<std::string>(""));
        if (goal.id.isEmpty()) {
            goal.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        }
        goal.objective = stds(node["objective"].as<std::string>(""));
        const auto status = qSocGoalStatusFromString(stds(node["status"].as<std::string>("active")));
        goal.status = status.value_or(QSocGoalStatus::Active);
        if (node["token_budget"]) {
            goal.tokenBudget = node["token_budget"].as<int>(0);
        }
        if (node["tokens_used"]) {
            goal.tokensUsed = node["tokens_used"].as<int>(0);
        }
        if (node["seconds_used"]) {
            goal.secondsUsed = node["seconds_used"].as<qint64>(0);
        }
        if (node["created_at"]) {
            goal.createdAt
                = QDateTime::fromString(stds(node["created_at"].as<std::string>("")), Qt::ISODate);
        }
        if (node["updated_at"]) {
            goal.updatedAt
                = QDateTime::fromString(stds(node["updated_at"].as<std::string>("")), Qt::ISODate);
        }
        if (goal.objective.isEmpty()) {
            return;
        }
        current_ = goal;
    } catch (const YAML::Exception &exc) {
        qInfo() << "goal catalog: malformed YAML in" << path << ":" << exc.what();
    }
}

std::optional<QSocGoal> QSocGoalCatalog::current() const
{
    return current_;
}

bool QSocGoalCatalog::writeYaml(QString *errorMessage)
{
    const QString path = projectFilePath();
    if (path.isEmpty()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("project scope is not set");
        }
        return false;
    }
    const QString dirPath = QFileInfo(path).absolutePath();
    if (!QDir().mkpath(dirPath)) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("cannot create %1").arg(dirPath);
        }
        return false;
    }

    YAML::Node root(YAML::NodeType::Map);
    if (current_.has_value()) {
        const auto &goal = *current_;
        YAML::Node  node(YAML::NodeType::Map);
        node["id"]        = asStd(goal.id);
        node["objective"] = asStd(goal.objective);
        node["status"]    = asStd(qSocGoalStatusToString(goal.status));
        if (goal.tokenBudget > 0) {
            node["token_budget"] = goal.tokenBudget;
        }
        node["tokens_used"]  = goal.tokensUsed;
        node["seconds_used"] = static_cast<long long>(goal.secondsUsed);
        if (goal.createdAt.isValid()) {
            node["created_at"] = asStd(goal.createdAt.toString(Qt::ISODate));
        }
        if (goal.updatedAt.isValid()) {
            node["updated_at"] = asStd(goal.updatedAt.toString(Qt::ISODate));
        }
        root["goal"] = node;
    }

    const QByteArray payload = emitYaml(root);
    QSaveFile        saver(path);
    if (!saver.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("cannot open %1 for write").arg(path);
        }
        return false;
    }
    saver.write(payload);
    if (!saver.commit()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("atomic commit failed for %1").arg(path);
        }
        return false;
    }
    return true;
}

void QSocGoalCatalog::appendLog(const QString &event, const QString &extraJsonFields)
{
    const QString path = logFilePath();
    if (path.isEmpty()) {
        return;
    }
    const QString dirPath = QFileInfo(path).absolutePath();
    if (!QDir().mkpath(dirPath)) {
        qInfo() << "goal log: cannot create" << dirPath;
        return;
    }
    nlohmann::json payload = nlohmann::json::object();
    payload["ts"]          = asStd(QDateTime::currentDateTime().toString(Qt::ISODate));
    payload["goal_id"]     = asStd(current_.has_value() ? current_->id : QString());
    payload["event"]       = asStd(event);
    if (!extraJsonFields.isEmpty()) {
        try {
            nlohmann::json extra = nlohmann::json::parse(extraJsonFields.toStdString());
            if (extra.is_object()) {
                for (auto it = extra.begin(); it != extra.end(); ++it) {
                    payload[it.key()] = it.value();
                }
            }
        } catch (const nlohmann::json::exception &) {
            /* Bad extra blob is non-fatal; record the goal event anyway. */
        }
    }
    QString line = QString::fromStdString(payload.dump()) + QLatin1Char('\n');
    QFile   file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append)) {
        qInfo() << "goal log: cannot append to" << path;
        return;
    }
    file.write(line.toUtf8());
}

bool QSocGoalCatalog::create(const QString &objective, int tokenBudget, QString *errorMessage)
{
    if (current_.has_value()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral(
                "a goal is already active; use replace to discard the current one first");
        }
        return false;
    }
    const QString trimmed = objective.trimmed();
    if (trimmed.isEmpty()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("objective is empty");
        }
        return false;
    }

    QSocGoal goal;
    goal.id          = QUuid::createUuid().toString(QUuid::WithoutBraces);
    goal.objective   = trimmed;
    goal.status      = QSocGoalStatus::Active;
    goal.tokenBudget = tokenBudget > 0 ? tokenBudget : 0;
    goal.createdAt   = QDateTime::currentDateTime();
    goal.updatedAt   = goal.createdAt;
    current_         = goal;

    if (!writeYaml(errorMessage)) {
        current_.reset();
        return false;
    }
    appendLog(
        QStringLiteral("created"),
        QString::fromStdString(
            nlohmann::json{{"objective", asStd(trimmed)}, {"token_budget", goal.tokenBudget}}
                .dump()));
    emit goalChanged();
    return true;
}

bool QSocGoalCatalog::replace(const QString &newObjective, int tokenBudget, QString *errorMessage)
{
    if (current_.has_value()) {
        const QString oldId = current_->id;
        appendLog(
            QStringLiteral("discarded"),
            QString::fromStdString(nlohmann::json{{"reason", "replaced"}}.dump()));
        current_.reset();
        Q_UNUSED(oldId);
    }
    return create(newObjective, tokenBudget, errorMessage);
}

bool QSocGoalCatalog::clear(QString *errorMessage)
{
    if (!current_.has_value()) {
        return true;
    }
    appendLog(QStringLiteral("cleared"));
    current_.reset();
    if (!writeYaml(errorMessage)) {
        return false;
    }
    emit goalChanged();
    return true;
}

bool QSocGoalCatalog::setStatus(QSocGoalStatus newStatus, QString *errorMessage)
{
    if (!current_.has_value()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("no active goal");
        }
        return false;
    }
    if (current_->status == newStatus) {
        return true;
    }
    const QSocGoalStatus from = current_->status;
    current_->status          = newStatus;
    current_->updatedAt       = QDateTime::currentDateTime();
    if (!writeYaml(errorMessage)) {
        current_->status = from;
        return false;
    }
    appendLog(
        QStringLiteral("status_changed"),
        QString::fromStdString(
            nlohmann::json{
                {"from", asStd(qSocGoalStatusToString(from))},
                {"to", asStd(qSocGoalStatusToString(newStatus))}}
                .dump()));
    emit goalChanged();
    /* Complete is terminal: drop the goal so a fresh one can be set
     * without a manual /goal clear, mirroring codex's delete-on-
     * complete behavior. The event log retains the trail. */
    if (newStatus == QSocGoalStatus::Complete) {
        current_.reset();
        if (!writeYaml(errorMessage)) {
            return false;
        }
        emit goalChanged();
    }
    return true;
}

bool QSocGoalCatalog::updateObjective(const QString &newObjective, QString *errorMessage)
{
    if (!current_.has_value()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("no active goal");
        }
        return false;
    }
    const QString trimmed = newObjective.trimmed();
    if (trimmed.isEmpty()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("objective is empty");
        }
        return false;
    }
    if (trimmed == current_->objective) {
        return true;
    }
    const QString from  = current_->objective;
    current_->objective = trimmed;
    current_->updatedAt = QDateTime::currentDateTime();
    if (!writeYaml(errorMessage)) {
        current_->objective = from;
        return false;
    }
    appendLog(
        QStringLiteral("objective_updated"),
        QString::fromStdString(nlohmann::json{{"objective", asStd(trimmed)}}.dump()));
    emit goalChanged();
    return true;
}

bool QSocGoalCatalog::accountUsage(int tokensDelta, qint64 secondsDelta, QString *errorMessage)
{
    if (!current_.has_value()) {
        return true;
    }
    if (tokensDelta < 0) {
        tokensDelta = 0;
    }
    if (secondsDelta < 0) {
        secondsDelta = 0;
    }
    current_->tokensUsed += tokensDelta;
    current_->secondsUsed += secondsDelta;
    current_->updatedAt = QDateTime::currentDateTime();
    if (!writeYaml(errorMessage)) {
        current_->tokensUsed -= tokensDelta;
        current_->secondsUsed -= secondsDelta;
        return false;
    }
    appendLog(
        QStringLiteral("usage_accounted"),
        QString::fromStdString(
            nlohmann::json{
                {"tokens_used", current_->tokensUsed},
                {"seconds_used", static_cast<long long>(current_->secondsUsed)}}
                .dump()));
    emit goalChanged();

    if (current_->tokenBudget > 0 && current_->tokensUsed >= current_->tokenBudget
        && current_->status == QSocGoalStatus::Active) {
        /* Use setStatus so the transition itself goes through the
         * regular log + write + signal path. */
        QString innerErr;
        if (!setStatus(QSocGoalStatus::BudgetLimited, &innerErr)) {
            qInfo() << "goal: BudgetLimited transition write failed:" << innerErr;
        }
    }
    return true;
}

void QSocGoalCatalog::noteContinuation(const QString &reason)
{
    appendLog(
        QStringLiteral("continued"),
        QString::fromStdString(nlohmann::json{{"reason", asStd(reason)}}.dump()));
}

bool QSocGoalCatalog::setTokenBudget(int newBudget, QString *errorMessage)
{
    if (!current_.has_value()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("no active goal");
        }
        return false;
    }
    if (newBudget < 0) {
        newBudget = 0;
    }
    const int from        = current_->tokenBudget;
    current_->tokenBudget = newBudget;
    current_->updatedAt   = QDateTime::currentDateTime();
    if (!writeYaml(errorMessage)) {
        current_->tokenBudget = from;
        return false;
    }
    appendLog(
        QStringLiteral("budget_updated"),
        QString::fromStdString(nlohmann::json{{"from", from}, {"to", newBudget}}.dump()));
    emit goalChanged();
    return true;
}
