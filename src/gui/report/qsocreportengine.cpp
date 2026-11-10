// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "gui/report/qsocreportengine.h"

#include <QEventLoop>
#include <QFile>
#include <QJSEngine>
#include <QJSValue>
#include <QJsonDocument>
#include <QMap>
#include <QTimer>

namespace {
class Tasks final : public QObject
{
    Q_OBJECT
public:
    using QObject::QObject;

    Q_INVOKABLE int defer(QJSValue callback, int delay)
    {
        const int id    = ++serial;
        auto     *timer = new QTimer(this);
        timer->setSingleShot(true);
        timers.insert(id, timer);
        connect(timer, &QTimer::timeout, this, [this, timer, id, callback]() mutable {
            timers.remove(id);
            timer->deleteLater();
            const auto result = callback.call();
            if (result.isError())
                emit finished({}, result.toString());
        });
        timer->start(qBound(0, delay, 60000));
        return id;
    }

    Q_INVOKABLE void cancel(int id) { delete timers.take(id); }

    Q_INVOKABLE void complete(const QString &result, const QString &error)
    {
        emit finished(result, error);
    }

signals:
    void finished(const QString &result, const QString &error);

private:
    QMap<int, QTimer *> timers;
    int                 serial = 0;
};
} // namespace

class QSocReportLayout::Engine::Runtime
{
public:
    Runtime()
    {
        engine.installExtensions(QJSEngine::ConsoleExtension);
        engine.globalObject().setProperty("__layoutTasks", engine.newQObject(&tasks));
        const auto environment = engine.evaluate(QStringLiteral(R"(
var self = this;
var window = this;
function setTimeout(callback, delay) {
    return __layoutTasks.defer(callback, Number(delay) || 0);
}
function clearTimeout(id) { __layoutTasks.cancel(id); }
)"));
        if (environment.isError()) {
            initializationError = environment.toString();
            return;
        }
        QFile source(QStringLiteral(":/primitive-report/layout.js"));
        if (!source.open(QIODevice::ReadOnly)) {
            initializationError = QStringLiteral("Missing report layout resource");
            return;
        }
        const auto loaded = engine.evaluate(QString::fromUtf8(source.readAll()), "layout.js");
        if (loaded.isError()) {
            initializationError = loaded.toString();
            return;
        }
        const auto instance = engine.evaluate(QStringLiteral("var __layoutEngine = new ELK();"));
        if (instance.isError())
            initializationError = instance.toString();
    }

    QJsonObject layout(const QJsonObject &graph, QString &error)
    {
        error.clear();
        if (!initializationError.isEmpty()) {
            error = initializationError;
            return {};
        }
        QEventLoop loop;
        QString    result;
        bool       completed = false;
        QObject::connect(
            &tasks, &Tasks::finished, &loop, [&](const QString &value, const QString &failure) {
                result    = value;
                error     = failure;
                completed = true;
                loop.quit();
            });
        engine.globalObject().setProperty(
            "__layoutInput", QString::fromUtf8(QJsonDocument(graph).toJson(QJsonDocument::Compact)));
        const auto started = engine.evaluate(QStringLiteral(R"(
__layoutEngine.layout(JSON.parse(__layoutInput)).then(function(graph) {
    __layoutTasks.complete(JSON.stringify(graph), '');
}, function(error) {
    __layoutTasks.complete('', String(error));
});
)"));
        if (started.isError()) {
            error = started.toString();
            return {};
        }
        QTimer::singleShot(60000, &loop, [&] {
            error = QStringLiteral("Report layout timed out");
            loop.quit();
        });
        if (!completed)
            loop.exec(QEventLoop::ExcludeUserInputEvents);
        if (!error.isEmpty())
            return {};
        QJsonParseError parseError;
        const auto      output = QJsonDocument::fromJson(result.toUtf8(), &parseError);
        if (parseError.error != QJsonParseError::NoError || !output.isObject()) {
            error = QStringLiteral("Invalid report layout result");
            return {};
        }
        return output.object();
    }

private:
    QJSEngine engine;
    Tasks     tasks;
    QString   initializationError;
};

QSocReportLayout::Engine::Engine()  = default;
QSocReportLayout::Engine::~Engine() = default;

QJsonObject QSocReportLayout::Engine::layout(const QJsonObject &graph, QString &error)
{
    if (!runtime)
        runtime = std::make_unique<Runtime>();
    const auto result = runtime->layout(graph, error);
    if (!error.isEmpty())
        runtime.reset();
    return result;
}

#include "qsocreportengine.moc"
