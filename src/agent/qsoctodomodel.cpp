// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/qsoctodomodel.h"
#include "agent/qsocworkspacefs.h"
#include "smt/qsocsmtservice.h"

#include <algorithm>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <QMap>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QScopedValueRollback>
#include <QSet>

namespace {
std::mutex    workspaceMutex;
QSet<QString> editingWorkspaces;
using json                   = nlohmann::json;
const QString todoFile       = QStringLiteral(".qsoc/todos.md");
const QString markFile       = QStringLiteral(".qsoc/todos.hwm");
const QString metadataPrefix = QStringLiteral("<!-- qsoc-todo: ");

bool validText(const QString &text, int limit, bool multiline = false)
{
    return text.size() <= limit && std::all_of(text.cbegin(), text.cend(), [multiline](QChar ch) {
               return ch.isPrint() || ch.isSurrogate() || (multiline && ch == QLatin1Char('\n'));
           });
}

QList<QSocTodoItem> inPlan(const QList<QSocTodoItem> &items, const QString &plan)
{
    QList<QSocTodoItem> result;
    for (const auto &item : items)
        if (item.planId == plan)
            result.append(item);
    return result;
}

QSet<QPair<int, int>> edges(const QList<QSocTodoItem> &items)
{
    QSet<QPair<int, int>> result;
    for (const auto &item : items)
        for (int dependency : item.dependsOn)
            result.insert({item.id, dependency});
    return result;
}

bool parse(const QString &content, QList<QSocTodoItem> *items, QString *error)
{
    const QRegularExpression row(QStringLiteral(R"(^-\s*\[([ xX])\]\s*(?:#(\d+)\s+)?(.+)$)"));
    QString                  priority = QStringLiteral("medium");
    QSet<int>                ids;
    try {
        for (const QString &line : content.split(QLatin1Char('\n'))) {
            if (line.startsWith("## High Priority"))
                priority = QStringLiteral("high");
            if (line.startsWith("## Medium Priority"))
                priority = QStringLiteral("medium");
            if (line.startsWith("## Low Priority"))
                priority = QStringLiteral("low");
            const auto match = row.match(line.trimmed());
            if (match.hasMatch()) {
                QSocTodoItem item;
                item.id = match.captured(2).isEmpty() ? 0 : match.captured(2).toInt();
                if ((!match.captured(2).isEmpty() && item.id <= 0)
                    || (item.id > 0 && ids.contains(item.id)))
                    throw std::runtime_error("Duplicate or invalid todo ID");
                if (item.id > 0)
                    ids.insert(item.id);
                item.title    = match.captured(3).trimmed();
                item.priority = priority;
                item.status   = match.captured(1).toLower() == "x" ? "done" : "pending";
                items->append(item);
                continue;
            }
            const QString trimmed = line.trimmed();
            if (!trimmed.startsWith(metadataPrefix))
                continue;
            if (items->isEmpty() || !trimmed.endsWith(" -->"))
                throw std::runtime_error("Invalid todo metadata");
            const auto value = json::parse(
                trimmed.mid(metadataPrefix.size(), trimmed.size() - metadataPrefix.size() - 4)
                    .toStdString());
            if (!value.is_object())
                throw std::runtime_error("Invalid todo metadata");
            for (auto it = value.begin(); it != value.end(); ++it)
                if (it.key() != "status" && it.key() != "description" && it.key() != "plan_id"
                    && it.key() != "depends_on")
                    throw std::runtime_error("Unsupported todo metadata");
            auto      &item   = items->last();
            const auto status = value.value("status", std::string("pending"));
            if (status != "pending" && status != "in_progress" && status != "done")
                throw std::runtime_error("Invalid todo status");
            if (item.status != "done" && status == "in_progress")
                item.status = QStringLiteral("in_progress");
            item.description = QString::fromStdString(value.value("description", std::string()));
            item.planId      = QString::fromStdString(value.value("plan_id", std::string()));
            if (value.contains("depends_on")) {
                if (!value["depends_on"].is_array())
                    throw std::runtime_error("Invalid todo dependencies");
                for (const auto &id : value["depends_on"]) {
                    if (!id.is_number_integer() || id <= 0 || id > std::numeric_limits<int>::max())
                        throw std::runtime_error("Invalid todo dependency ID");
                    item.dependsOn.append(id.get<int>());
                }
            }
        }
        int next = 0;
        for (auto &item : *items) {
            if (item.id == 0) {
                do {
                    ++next;
                } while (ids.contains(next));
                item.id = next;
                ids.insert(next);
            }
            if (!validText(item.title, 512) || !validText(item.description, 4096, true)
                || !validText(item.planId, 256))
                throw std::runtime_error("Invalid todo text");
        }
    } catch (const std::exception &exception) {
        *error = QString::fromUtf8(exception.what());
        return false;
    }
    return true;
}

QByteArray serialize(const QList<QSocTodoItem> &items)
{
    QString result = QStringLiteral("# QSoC Todo List\n\n");
    for (const QString &priority :
         {QStringLiteral("high"), QStringLiteral("medium"), QStringLiteral("low")}) {
        QString group;
        for (const auto &item : items) {
            if (item.priority != priority)
                continue;
            group += QStringLiteral("- [%1] #%2 %3\n")
                         .arg(item.status == "done" ? "x" : " ")
                         .arg(item.id)
                         .arg(item.title);
            if (item.status != "in_progress" && item.description.isEmpty() && item.planId.isEmpty()
                && item.dependsOn.isEmpty())
                continue;
            json value
                = {{"status", item.status.toStdString()},
                   {"description", item.description.toStdString()},
                   {"plan_id", item.planId.toStdString()},
                   {"depends_on", json::array()}};
            for (int id : item.dependsOn)
                value["depends_on"].push_back(id);
            QString encoded = QString::fromStdString(value.dump());
            encoded.replace('<', QStringLiteral("\\u003c")).replace('>', QStringLiteral("\\u003e"));
            group += QStringLiteral("  ") + metadataPrefix + encoded + QStringLiteral(" -->\n");
        }
        if (!group.isEmpty())
            result += QStringLiteral("## %1 Priority\n\n")
                          .arg(priority.first(1).toUpper() + priority.sliced(1))
                      + group + QLatin1Char('\n');
    }
    return result.toUtf8();
}
} // namespace

QSocTodoModel::QSocTodoModel(QSocWorkspaceFs *fs)
    : fs_(fs)
{}

QSocTodoModel::~QSocTodoModel()
{
    proofThread_.request_stop();
    if (proofThread_.joinable())
        proofThread_.join();
}

void QSocTodoModel::checkDependencies()
{
    if (!active_ || !loaded_ || !error_.isEmpty())
        return;
    const QString smt = dependencySmt();
    const QString key = root_ + QLatin1Char('\n') + currentPlan(scope_) + QLatin1Char('\n') + smt;
    if (checkedGraph_ == key)
        return;
    if (checking_) {
        dependencyCheck_ = QStringLiteral("Checking updated dependencies");
        return;
    }
    checkedGraph_ = key;
    if (!dependencyError(visibleItems()).isEmpty()) {
        dependencyCheck_ = QStringLiteral("Not verified: repair dependencies");
        return;
    }
    if (!smt.contains("(assert")) {
        dependencyCheck_ = QStringLiteral("No dependency edges");
        return;
    }
    if (!QSocSmtService::supported()) {
        dependencyCheck_ = QStringLiteral("SMT audit unavailable; topological check passed");
        return;
    }
    dependencyCheck_ = QStringLiteral("SMT audit pending; topological check passed");
    checking_        = true;
    const QJsonObject request{
        {"smtlib", smt},
        {"timeout_ms", 1000},
        {"return_model", false},
        {"return_unsat_core", false}};
    try {
        proofThread_ = std::jthread([this, key, request](std::stop_token stop) {
            QJsonObject result;
            try {
                result = QSocSmtService::solve(request, stop);
            } catch (const std::exception &) {
                result = QSocSmtService::failure("error", "Dependency audit failed");
            }
            QMetaObject::invokeMethod(
                this,
                [this, key, result] {
                    if (proofThread_.joinable())
                        proofThread_.join();
                    checking_             = false;
                    const QString current = root_ + QLatin1Char('\n') + currentPlan(scope_)
                                            + QLatin1Char('\n') + dependencySmt();
                    if (loaded_ && error_.isEmpty() && key == current) {
                        const auto status = result.value("solver_status").toString();
                        dependencyCheck_  = result.value("execution") == "completed"
                                                    && status == "sat"
                                                ? QStringLiteral("Verified acyclic by SMT")
                                                : QStringLiteral("SMT audit unverified: ")
                                                      + result.value("execution").toString()
                                                      + QLatin1Char('/') + status;
                    }
                    checkDependencies();
                    emit tasksChanged();
                },
                Qt::QueuedConnection);
        });
    } catch (const std::system_error &) {
        checking_        = false;
        dependencyCheck_ = QStringLiteral("SMT audit unavailable; topological check passed");
    }
}

bool QSocTodoModel::refresh(QString *error, bool force)
{
    if (refreshing_) {
        *error = QStringLiteral("Todo storage is busy");
        return false;
    }
    const QString root = fs_->identity();
    if (!force && loaded_ && root == root_) {
        *error = error_;
        return error_.isEmpty();
    }
    const QScopedValueRollback<bool> guard(refreshing_, true);
    QByteArray                       bytes;
    QList<QSocTodoItem>              items;
    QString                          failure;
    const auto                       result = fs_->read(todoFile, &bytes, &failure);
    if (result == QSocWorkspaceFs::Result::Failed && failure.isEmpty())
        failure = QStringLiteral("Could not read todo storage");
    if (result == QSocWorkspaceFs::Result::Ok && bytes.size() > 1024 * 1024)
        failure = QStringLiteral("Todo file exceeds 1 MiB");
    if (result == QSocWorkspaceFs::Result::Ok && failure.isEmpty())
        parse(QString::fromUtf8(bytes), &items, &failure);
    if (root != fs_->identity())
        failure = QStringLiteral("Workspace changed while reading todos");
    root_   = root;
    loaded_ = true;
    error_  = failure;
    if (failure.isEmpty())
        items_ = items;
    else {
        items_.clear();
        checkedGraph_.clear();
        dependencyCheck_ = QStringLiteral("Not verified: todo storage is unavailable");
    }
    *error = failure;
    checkDependencies();
    emit tasksChanged();
    return failure.isEmpty();
}

void QSocTodoModel::setScope(const QString &scope, bool active)
{
    if (scope_ == scope && active_ == active)
        return;
    scope_  = scope;
    active_ = active;
    checkDependencies();
    emit tasksChanged();
}

QString QSocTodoModel::currentPlan(const QString &scope) const
{
    QString result = scope;
    int     latest = 0;
    for (const auto &item : items_)
        if ((item.planId == scope || (!scope.isEmpty() && item.planId.startsWith(scope + '/')))
            && item.id > latest) {
            latest = item.id;
            result = item.planId;
        }
    return result;
}

QList<QSocTodoItem> QSocTodoModel::visibleItems() const
{
    const auto current = inPlan(items_, currentPlan(scope_));
    return current.isEmpty() ? inPlan(items_, {}) : current;
}

QString QSocTodoModel::dependencyError(const QList<QSocTodoItem> &items)
{
    QMap<int, const QSocTodoItem *> known;
    QMap<int, int>                  pending;
    QMap<int, QList<int>>           children;
    for (const auto &item : items) {
        if (known.contains(item.id))
            return QStringLiteral("Duplicate todo #%1").arg(item.id);
        known[item.id]   = &item;
        pending[item.id] = item.dependsOn.size();
    }
    for (const auto &item : items) {
        QSet<int> unique;
        for (int dependency : item.dependsOn) {
            if (!known.contains(dependency))
                return QStringLiteral("Todo #%1 depends on missing todo #%2")
                    .arg(item.id)
                    .arg(dependency);
            if (unique.contains(dependency))
                return QStringLiteral("Todo #%1 repeats dependency #%2").arg(item.id).arg(dependency);
            unique.insert(dependency);
            children[dependency].append(item.id);
        }
    }
    QList<int> ready;
    for (auto it = pending.cbegin(); it != pending.cend(); ++it)
        if (it.value() == 0)
            ready.append(it.key());
    for (qsizetype at = 0; at < ready.size(); ++at)
        for (int child : children.value(ready[at]))
            if (--pending[child] == 0)
                ready.append(child);
    if (ready.size() == items.size())
        return {};
    int id = pending.firstKey();
    while (pending.value(id) == 0)
        id = pending.upperBound(id).key();
    QList<int> path;
    while (!path.contains(id)) {
        path.append(id);
        const auto &dependencies = known[id]->dependsOn;
        id = *std::find_if(dependencies.cbegin(), dependencies.cend(), [&](int parent) {
            return pending.value(parent) > 0;
        });
    }
    path = path.sliced(path.indexOf(id));
    path.append(id);
    QStringList cycle;
    for (int task : path)
        cycle.append(QStringLiteral("#%1").arg(task));
    return QStringLiteral("Dependency cycle: ") + cycle.join(QStringLiteral(", "));
}

QString QSocTodoModel::save(const QList<QSocTodoItem> &items, const QString &plan)
{
    const auto    candidate = inPlan(items, plan);
    const auto    previous  = inPlan(items_, plan);
    const QString problem   = dependencyError(candidate);
    if (!problem.isEmpty()) {
        const auto before   = edges(previous);
        const auto after    = edges(candidate);
        const bool removing = after.size() < before.size() && (after - before).isEmpty();
        if (!problem.startsWith("Dependency cycle:") || dependencyError(previous).isEmpty()
            || !removing)
            return problem;
    }
    if (root_ != fs_->identity())
        return QStringLiteral("Workspace changed before saving todos");
    const auto bytes = serialize(items);
    if (bytes.size() > 1024 * 1024)
        return QStringLiteral("Todo file exceeds 1 MiB");
    QString failure;
    if (!fs_->write(todoFile, bytes, &failure)) {
        loaded_ = false;
        checkedGraph_.clear();
        error_ = failure.isEmpty() ? QStringLiteral("Todo write was not confirmed") : failure;
        dependencyCheck_ = QStringLiteral("Not verified: reload after the failed write");
        emit tasksChanged();
        return error_;
    }
    if (root_ != fs_->identity())
        return QStringLiteral("Workspace changed while saving todos; reload before retrying");
    items_ = items;
    error_.clear();
    checkDependencies();
    emit tasksChanged();
    return {};
}

QString QSocTodoModel::execute(
    const QString &operation, const json &arguments, const QString &scope, bool child)
{
    const auto fail = [](const QString &reason) { return QStringLiteral("Error: ") + reason; };
    if (!arguments.is_object())
        return fail(QStringLiteral("Expected an object"));
    if (editing_)
        return fail(QStringLiteral("Todo storage is busy; retry after the current update"));
    const QString workspace = fs_->identity();
    {
        const std::lock_guard lock(workspaceMutex);
        if (editingWorkspaces.contains(workspace))
            return fail(QStringLiteral("Todo storage is busy; retry after the current update"));
        editingWorkspaces.insert(workspace);
    }
    const auto                       release = qScopeGuard([workspace] {
        const std::lock_guard lock(workspaceMutex);
        editingWorkspaces.remove(workspace);
    });
    const QScopedValueRollback<bool> guard(editing_, true);
    QString                          error;
    if (!refresh(&error, true))
        return fail(error);
    auto items = items_;
    try {
        if (operation == "todo_list") {
            const auto filter = arguments.value("filter", std::string("all"));
            QString    result;
            QString    requested;
            if (arguments.contains("plan_id"))
                requested = QString::fromStdString(arguments.at("plan_id").get<std::string>());
            if (requested == "current")
                requested = currentPlan(scope);
            for (const auto &item : items) {
                if ((filter == "done" && item.status != "done")
                    || (filter == "pending" && item.status == "done"))
                    continue;
                if (arguments.contains("plan_id") && item.planId != requested)
                    continue;
                result += QStringLiteral("[%1] %2. %3 (%4)%5\n")
                              .arg(item.status == "done" ? "x" : " ")
                              .arg(item.id)
                              .arg(
                                  item.title,
                                  item.priority,
                                  item.status == "in_progress" ? " [in_progress]" : "");
                if (!item.dependsOn.isEmpty()) {
                    QStringList ids;
                    for (int id : item.dependsOn)
                        ids.append(QString::number(id));
                    result
                        += QStringLiteral("  Depends on: %1\n").arg(ids.join(QStringLiteral(", ")));
                }
            }
            return result.isEmpty()
                       ? QStringLiteral("No todos found. Use todo_add to create new tasks.")
                       : QStringLiteral("Todo List:\n") + result;
        }
        QSocTodoItem added;
        auto         it = items.end();
        if (operation == "todo_add") {
            added.title = QString::fromStdString(arguments.at("title").get<std::string>()).trimmed();
            if (added.title.isEmpty() || !validText(added.title, 512))
                return fail(QStringLiteral("title must contain 1 to 512 printable characters"));
            added.priority = QString::fromStdString(
                arguments.value("priority", std::string("medium")));
            if (added.priority != "high" && added.priority != "medium" && added.priority != "low")
                added.priority = QStringLiteral("medium");
            added.planId = currentPlan(scope);
            if (arguments.contains("plan_id")) {
                const QString plan = QString::fromStdString(
                    arguments.at("plan_id").get<std::string>());
                added.planId = plan.isEmpty() || scope.isEmpty() || plan == scope
                                       || plan.startsWith(scope + '/')
                                   ? plan
                                   : scope + '/' + plan;
            }
            QByteArray bytes;
            const auto result = fs_->read(markFile, &bytes, &error);
            if (result == QSocWorkspaceFs::Result::Failed)
                return fail(error);
            int maximum = 0;
            if (result == QSocWorkspaceFs::Result::Ok) {
                bool ok = false;
                maximum = QString::fromUtf8(bytes).trimmed().toInt(&ok);
                if (!ok || maximum < 0)
                    return fail(QStringLiteral("Invalid todo ID mark"));
            }
            for (const auto &item : items)
                maximum = qMax(maximum, item.id);
            if (maximum == std::numeric_limits<int>::max())
                return fail(QStringLiteral("Todo IDs are exhausted"));
            added.id = maximum + 1;
            items.append(added);
            it = std::prev(items.end());
        } else {
            const auto &value = arguments.at("id");
            if (!value.is_number_integer() || value <= 0 || value > std::numeric_limits<int>::max())
                return fail(QStringLiteral("id must be a positive integer"));
            const int id = value.get<int>();
            it           = std::find_if(items.begin(), items.end(), [id](const auto &item) {
                return item.id == id;
            });
            if (it == items.end())
                return fail(QStringLiteral("Todo #%1 not found").arg(id));
            if (child && !it->planId.isEmpty()
                && (scope.isEmpty() || (it->planId != scope && !it->planId.startsWith(scope + '/'))))
                return fail(QStringLiteral("Only the owning agent can update this plan"));
        }
        const int     id    = it->id;
        const QString title = it->title;
        const QString plan  = it->planId;
        if (operation == "todo_delete") {
            items.erase(it);
        } else {
            if (arguments.contains("status"))
                it->status = QString::fromStdString(arguments.at("status").get<std::string>());
            if (it->status != "pending" && it->status != "in_progress" && it->status != "done")
                return fail(QStringLiteral("status must be 'done', 'pending', or 'in_progress'"));
            if (arguments.contains("description"))
                it->description = QString::fromStdString(
                    arguments.at("description").get<std::string>());
            if (!validText(it->description, 4096, true) || !validText(it->planId, 256))
                return fail(QStringLiteral("Invalid todo description or plan ID"));
            if (arguments.contains("depends_on")) {
                if (!arguments["depends_on"].is_array())
                    return fail(QStringLiteral("depends_on must be an array of IDs"));
                it->dependsOn.clear();
                for (const auto &dependency : arguments["depends_on"]) {
                    if (!dependency.is_number_integer() || dependency <= 0
                        || dependency > std::numeric_limits<int>::max())
                        return fail(QStringLiteral("Invalid dependency ID"));
                    it->dependsOn.append(dependency.get<int>());
                }
            }
        }
        error = dependencyError(inPlan(items, plan));
        if (operation == "todo_add" && !error.isEmpty())
            return fail(error);
        if (operation != "todo_delete" && it->status != "pending"
            && (error.isEmpty() || arguments.contains("status"))) {
            for (int dependency : it->dependsOn) {
                const auto parent
                    = std::find_if(items.cbegin(), items.cend(), [dependency](const auto &item) {
                          return item.id == dependency;
                      });
                if (parent == items.cend() || parent->status != "done")
                    return fail(
                        QStringLiteral(
                            "Finish prerequisite #%1 before starting or completing todo #%2")
                            .arg(dependency)
                            .arg(id));
            }
        }
        if (operation == "todo_add" && !fs_->write(markFile, QByteArray::number(id), &error))
            return fail(error);
        error = save(items, plan);
        if (!error.isEmpty())
            return fail(error);
        if (operation == "todo_add")
            return QStringLiteral("Added todo #%1: %2 (%3 priority)")
                .arg(id)
                .arg(title, added.priority);
        if (operation == "todo_delete")
            return QStringLiteral("Deleted todo #%1: %2").arg(id).arg(title);
        const QString remaining = dependencyError(inPlan(items, plan));
        return QStringLiteral("Updated todo #%1: %2 (status: %3)").arg(id).arg(title, it->status)
               + (remaining.isEmpty()
                      ? QString()
                      : QLatin1Char('\n') + remaining
                            + QStringLiteral(". Remove more dependency edges to repair the plan."));
    } catch (const json::exception &) {
        return fail(QStringLiteral("Missing or invalid todo argument"));
    }
}

QList<QSocTask::Row> QSocTodoModel::listTasks() const
{
    QList<QSocTask::Row> result;
    if (!active_)
        return result;
    const auto    items   = visibleItems();
    const QString problem = error_.isEmpty() ? dependencyError(items) : error_;
    if (!problem.isEmpty()) {
        QSocTask::Row row{};
        row.id        = QStringLiteral("error");
        row.label     = QStringLiteral("Todo dependency problem");
        row.summary   = problem;
        row.objective = problem;
        row.kind      = QSocTask::Kind::Todo;
        row.status    = QSocTask::Status::Failed;
        result.append(row);
    }
    QMap<int, QString> statuses;
    for (const auto &item : items)
        statuses[item.id] = item.status;
    for (const auto &item : items) {
        QSocTask::Row row{};
        row.id              = QString::number(item.id);
        row.label           = item.title;
        row.objective       = item.description;
        row.priority        = item.priority;
        row.kind            = QSocTask::Kind::Todo;
        row.planId          = item.planId;
        row.dependencyCheck = dependencyCheck_;
        row.status          = item.status == "done"          ? QSocTask::Status::Completed
                              : item.status == "in_progress" ? QSocTask::Status::Running
                                                             : QSocTask::Status::Pending;
        QStringList waiting;
        for (int dependency : item.dependsOn) {
            row.dependsOn.append(QString::number(dependency));
            if (statuses.value(dependency) != "done")
                waiting.append(QStringLiteral("#%1").arg(dependency));
        }
        row.summary = waiting.isEmpty()
                          ? (item.status == "done"          ? "Done"
                             : item.status == "in_progress" ? "In progress"
                                                            : "Ready")
                          : QStringLiteral("Waiting for ") + waiting.join(QStringLiteral(", "));
        result.append(row);
    }
    return result;
}

QString QSocTodoModel::tailFor(const QString &id, int maxBytes) const
{
    QString text;
    for (const auto &row : listTasks()) {
        if (row.id != id)
            continue;
        text = QStringLiteral("Todo #%1: %2\n%3\n").arg(row.id, row.label, row.summary);
        if (!row.objective.isEmpty())
            text += QStringLiteral("Completion: ") + row.objective + '\n';
        if (!row.dependsOn.isEmpty())
            text += QStringLiteral("Depends on: ") + row.dependsOn.join(QStringLiteral(", "))
                    + '\n';
    }
    if (text.isEmpty())
        return {};
    if (!dependencyCheck_.isEmpty())
        text += QStringLiteral("Dependency audit: ") + dependencyCheck_ + QLatin1Char('\n');
    return QString::fromUtf8(text.toUtf8().left(maxBytes));
}

QString QSocTodoModel::dependencySmt() const
{
    QString    result;
    const auto items = visibleItems();
    for (const auto &item : items)
        result += QStringLiteral("(declare-const t%1 Int)\n").arg(item.id);
    for (const auto &item : items)
        for (int dependency : item.dependsOn)
            result += QStringLiteral("(assert (< t%1 t%2))\n").arg(dependency).arg(item.id);
    return result;
}

#include "moc_qsoctodomodel.cpp"
