// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/qsocagent.h"
#include "agent/qsocdispatchpolicy.h"
#include "agent/qsoctool.h"
#include "agent/remote/qsochostprofile.h"
#include "agent/remote/qsocsshconfigparser.h"
#include "agent/runtime/qsocagentruntime.h"
#include "qsoc_test.h"

#include <nlohmann/json.hpp>
#include <yaml-cpp/yaml.h>

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

#include <memory>

using json = nlohmann::json;

/*
 * agent.dispatch names the hosts and models a sub-agent may use. Each case
 * builds the policy from YAML written here, an ssh config and a host catalog
 * written to a temporary directory, so no real host or key is involved.
 */

namespace {

/* Strings that must never reach the model: endpoint URL, API key, the ssh
 * HostName and User, and a catalog target. */
const QStringList kSecrets
    = {QStringLiteral("model-endpoint.invalid"),
       QStringLiteral("sk-dispatch-sentinel"),
       QStringLiteral("sim1-real.invalid"),
       QStringLiteral("operator-sentinel"),
       QStringLiteral("builder@catbox.invalid")};

const char *kModels = R"(
main-model:
  url: http://model-endpoint.invalid/v1/chat/completions
  key: sk-dispatch-sentinel
  context: 64000
pro:
  name: Pro Model
  url: http://model-endpoint.invalid/v1/chat/completions
  key: sk-dispatch-sentinel
  context: 400000
  effort: high
omni:
  url: http://model-endpoint.invalid/v1/chat/completions
  context: 32000
)";

bool spill(const QString &path, const QByteArray &content)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate)
           && file.write(content) == content.size();
}

QSocMainState localMain()
{
    QSocMainState mainAgent;
    mainAgent.modelId = QStringLiteral("main-model");
    mainAgent.effort  = QStringLiteral("medium");
    return mainAgent;
}

QSocChildRequest onHost(const QString &host, const QString &model = QString())
{
    QSocChildRequest request;
    request.host  = host;
    request.model = model;
    return request;
}

} // namespace

class Test : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();
    void anUngrantedHostIsRefused();
    void aHostBoundModelIsForced();
    void omittedFieldsInheritTheMainAgent();
    void anotherModelBringsItsOwnEffortAndWindow();
    void aForkCannotChangeModel();
    void typosFailClosed();
    void localMeansThisMachineFromARemote();
    void aNamedWorkspaceStaysInsideTheGrant();
    void undeclaredHostsKeepTheCatalog();
    void nothingSecretReachesTheModel();
    void theSnapshotChangesOnlyOnReload();

private:
    QSocDispatchPolicy policy(const char *dispatch) const
    {
        return QSocDispatchPolicy::fromNodes(
            YAML::Load(dispatch), YAML::Load(kModels), &m_catalog, &m_ssh);
    }

    std::optional<QSocChildPlan> resolve(
        const QSocDispatchPolicy &policy,
        const QSocChildRequest   &request,
        const QSocMainState      &mainAgent,
        QString                  *error) const
    {
        return policy.resolveChild(request, mainAgent, &m_catalog, {}, error);
    }

    QTemporaryDir       m_dir;
    QSocHostCatalog     m_catalog;
    QSocSshConfigParser m_ssh;
};

void Test::initTestCase()
{
    QVERIFY(m_dir.isValid());
    const QString sshConfig = m_dir.filePath(QStringLiteral("ssh_config"));
    QVERIFY(spill(
        sshConfig,
        "Host sim1\n  HostName sim1-real.invalid\n  User operator-sentinel\n"
        "Host spare\n  HostName spare.invalid\n"));
    QVERIFY(m_ssh.parse(sshConfig));
    const QString project = m_dir.filePath(QStringLiteral("project"));
    QVERIFY(QDir().mkpath(project));
    m_catalog.load(QString(), project);
    QSocHostProfile profile;
    profile.alias      = QStringLiteral("catbox");
    profile.target     = QStringLiteral("builder@catbox.invalid:22");
    profile.workspace  = QStringLiteral("/srv/cat");
    profile.capability = QStringLiteral("catalog capability sentinel");
    QVERIFY(m_catalog.upsert(profile, true));
}

void Test::cleanupTestCase()
{
    /* QSOC_TEST_MAIN calls _exit(), so QTemporaryDir's destructor never runs. */
    QVERIFY(m_dir.remove());
}

/* Counterexample: a named host the user never granted was dispatched, and the
 * refusal listed hosts from ~/.ssh/config the user had not granted either. */
void Test::anUngrantedHostIsRefused()
{
    const auto granted = policy("hosts:\n  sim1: {}\n");
    QCOMPARE(granted.hostChoices(), QStringList({"local", "sim1"}));
    QString error;
    QVERIFY(!resolve(granted, onHost(QStringLiteral("catbox")), localMain(), &error));
    QVERIFY2(error.contains(QStringLiteral("not in agent.dispatch.hosts")), qPrintable(error));
    QVERIFY2(error.contains(QStringLiteral("local, sim1")), qPrintable(error));
    QVERIFY2(!error.contains(QStringLiteral("spare")), qPrintable(error));
    QVERIFY(resolve(granted, onHost(QStringLiteral("sim1")), localMain(), &error));
}

/* Counterexample: a host the user bound to one model ran a child on another. */
void Test::aHostBoundModelIsForced()
{
    const auto bound = policy("hosts:\n  sim1: {model: pro}\nmodels: [omni]\n");
    QString    error;
    const auto plan = resolve(bound, onHost(QStringLiteral("sim1")), localMain(), &error);
    QVERIFY2(plan && plan->model, qPrintable(error));
    QCOMPARE(plan->model->id, QStringLiteral("pro"));
    QVERIFY(
        resolve(bound, onHost(QStringLiteral("sim1"), QStringLiteral("pro")), localMain(), &error));
    QVERIFY(
        !resolve(bound, onHost(QStringLiteral("sim1"), QStringLiteral("omni")), localMain(), &error));
    QVERIFY2(error.contains(QStringLiteral("runs on model pro")), qPrintable(error));

    /* The main agent's own binding counts as the host an omitted host means. */
    QSocMainState remote = localMain();
    remote.remote        = true;
    remote.alias         = QStringLiteral("sim1");
    const auto inherited = resolve(bound, QSocChildRequest(), remote, &error);
    QVERIFY(inherited && inherited->model && inherited->host.isEmpty());
    QCOMPARE(inherited->model->id, QStringLiteral("pro"));
}

void Test::omittedFieldsInheritTheMainAgent()
{
    const auto declared = policy("hosts:\n  sim1: {}\nmodels: [pro]\n");
    QString    error;
    const auto plan = resolve(declared, QSocChildRequest(), localMain(), &error);
    QVERIFY2(plan, qPrintable(error));
    QVERIFY(plan->host.isEmpty());
    QVERIFY(!plan->model);
    QCOMPARE(plan->effort, QStringLiteral("medium"));
    /* Naming the main agent's own model is the same as omitting it. */
    const auto same
        = resolve(declared, onHost(QString(), QStringLiteral("main-model")), localMain(), &error);
    QVERIFY(same && !same->model);
}

/* Counterexample: a child on another model reasoned at the main agent's effort
 * although that model's entry names its own. */
void Test::anotherModelBringsItsOwnEffortAndWindow()
{
    const auto models = policy("models: [pro, omni]\n");
    QString    error;
    const auto pro = resolve(models, onHost(QString(), QStringLiteral("pro")), localMain(), &error);
    QVERIFY2(pro && pro->model, qPrintable(error));
    QCOMPARE(pro->effort, QStringLiteral("high"));
    QCOMPARE(pro->model->contextTokens, 400000);
    const auto omni
        = resolve(models, onHost(QString(), QStringLiteral("omni")), localMain(), &error);
    QVERIFY(omni && omni->model);
    QCOMPARE(omni->effort, QStringLiteral("medium"));
    QVERIFY(!resolve(
        policy("models: [omni]\n"), onHost(QString(), QStringLiteral("pro")), localMain(), &error));
    QVERIFY2(error.contains(QStringLiteral("use one of: omni")), qPrintable(error));
    QVERIFY(!resolve(policy("{}"), onHost(QString(), QStringLiteral("pro")), localMain(), &error));
    QVERIFY2(error.contains(QStringLiteral("omit model")), qPrintable(error));
}

void Test::aForkCannotChangeModel()
{
    const auto       bound = policy("hosts:\n  sim1: {model: pro}\nmodels: [omni]\n");
    QSocChildRequest fork  = onHost(QString(), QStringLiteral("omni"));
    fork.fork              = true;
    QString error;
    QVERIFY(!resolve(bound, fork, localMain(), &error));
    QVERIFY2(error.contains(QStringLiteral("fork mode keeps")), qPrintable(error));
    fork.model.clear();
    fork.host = QStringLiteral("sim1");
    QVERIFY(!resolve(bound, fork, localMain(), &error));
    fork.host.clear();
    QVERIFY2(resolve(bound, fork, localMain(), &error), qPrintable(error));
}

/* Counterexample: a misspelt field left the host granted with no model, so its
 * children silently ran on the main model the user meant to replace. */
void Test::typosFailClosed()
{
    QString    error;
    const auto misspelt = policy("hosts:\n  sim1: {modle: pro}\n");
    QVERIFY(misspelt.hostChoices() == QStringList({"local"}));
    QVERIFY(!resolve(misspelt, onHost(QStringLiteral("sim1")), localMain(), &error));
    QVERIFY2(error.contains(QStringLiteral("unknown field 'modle'")), qPrintable(error));
    QCOMPARE(misspelt.warnings().size(), 1);

    const auto unknownModel = policy("hosts:\n  sim1: {model: nope}\nmodels: [nope2]\n");
    QCOMPARE(unknownModel.warnings().size(), 2);
    QVERIFY(!resolve(unknownModel, onHost(QStringLiteral("sim1")), localMain(), &error));
    QVERIFY2(error.contains(QStringLiteral("'nope' is not in llm.models")), qPrintable(error));
    QVERIFY(!resolve(unknownModel, onHost(QString(), QStringLiteral("nope2")), localMain(), &error));
    QVERIFY2(error.contains(QStringLiteral("nope2")), qPrintable(error));

    /* A misspelt section name declares hosts, so the catalog stops granting. */
    const auto section = policy("host:\n  catbox: {}\n");
    QVERIFY(section.hostsDeclared());
    QVERIFY(!resolve(section, onHost(QStringLiteral("catbox")), localMain(), &error));

    const auto unreachable = policy("hosts:\n  ghost: {}\n");
    QVERIFY(!resolve(unreachable, onHost(QStringLiteral("ghost")), localMain(), &error));
    QVERIFY2(error.contains(QStringLiteral("neither in ~/.ssh/config")), qPrintable(error));
}

void Test::localMeansThisMachineFromARemote()
{
    const auto    open   = policy("{}");
    QSocMainState remote = localMain();
    remote.remote        = true;
    remote.alias         = QStringLiteral("sim1");
    QString    error;
    const auto plan = resolve(open, onHost(QStringLiteral("local")), remote, &error);
    QVERIFY(plan);
    QCOMPARE(plan->host, QStringLiteral("local"));
    QVERIFY(resolve(open, QSocChildRequest(), remote, &error));
}

void Test::aNamedWorkspaceStaysInsideTheGrant()
{
    const auto       rooted  = policy("hosts:\n  sim1: {workspace: /work/proj}\n");
    QSocChildRequest request = onHost(QStringLiteral("sim1"));
    QString          error;
    const auto       plain = resolve(rooted, request, localMain(), &error);
    QVERIFY(plain && !plain->workspaceNamed);
    QCOMPARE(plain->workspace, QStringLiteral("/work/proj"));
    request.workspace = QStringLiteral("/work/proj/sub/../block");
    const auto inside = resolve(rooted, request, localMain(), &error);
    QVERIFY2(inside && inside->workspaceNamed, qPrintable(error));
    QCOMPARE(inside->workspace, QStringLiteral("/work/proj/block"));
    QCOMPARE(inside->workspaceRoot, QStringLiteral("/work/proj"));
    for (const char *outside : {"/work/projx", "/work/proj/../other", "/", "relative"}) {
        request.workspace = QString::fromLatin1(outside);
        QVERIFY2(!resolve(rooted, request, localMain(), &error), outside);
    }
    request.host      = QString();
    request.workspace = QStringLiteral("/work/proj");
    QVERIFY(!resolve(rooted, request, localMain(), &error));
    QVERIFY2(error.contains(QStringLiteral("named remote host")), qPrintable(error));
}

/* Without agent.dispatch.hosts the catalog decides, as before; models alone do
 * not take hosts away. */
void Test::undeclaredHostsKeepTheCatalog()
{
    const auto modelsOnly = policy("models: [pro]\n");
    QVERIFY(!modelsOnly.hostsDeclared());
    QVERIFY(modelsOnly.hostChoices().isEmpty());
    QString error;
    QVERIFY2(
        resolve(modelsOnly, onHost(QStringLiteral("catbox")), localMain(), &error),
        qPrintable(error));
    QVERIFY(!resolve(modelsOnly, onHost(QStringLiteral("spare")), localMain(), &error));
    QVERIFY2(error.contains(QStringLiteral("unknown host 'spare'")), qPrintable(error));
    QVERIFY(policy("{}").promptSection().isEmpty());
}

void Test::nothingSecretReachesTheModel()
{
    const auto full = policy("hosts:\n  sim1: {model: pro}\n  catbox: {}\nmodels: [pro, omni]\n");
    const QString text = full.promptSection() + full.describe()
                         + full.hostChoices().join(QLatin1Char(' '))
                         + full.modelChoices().join(QLatin1Char(' '));
    for (const QString &secret : kSecrets) {
        QVERIFY2(!text.contains(secret), qPrintable(secret + QStringLiteral(" in ") + text));
    }
    QVERIFY2(text.contains(QStringLiteral("catalog capability sentinel")), qPrintable(text));
    QVERIFY2(text.contains(QStringLiteral("pro: Pro Model, context 400000")), qPrintable(text));
    QCOMPARE(
        full.promptSection(),
        policy("hosts:\n  catbox: {}\n  sim1: {model: pro}\nmodels: [pro, omni]\n").promptSection());
}

/* Counterexample: a config file edited during a session, by anyone with a
 * shell, changed what the next child could use without the user asking. */
void Test::theSnapshotChangesOnlyOnReload()
{
    const QString root       = m_dir.filePath(QStringLiteral("runtime"));
    const QString configHome = root + QStringLiteral("/config");
    const QString configFile = configHome + QStringLiteral("/qsoc/qsoc.yml");
    for (const QString &dir :
         {root + QStringLiteral("/home"),
          configHome + QStringLiteral("/qsoc"),
          root + QStringLiteral("/data"),
          root + QStringLiteral("/run"),
          root + QStringLiteral("/project")}) {
        QVERIFY(QDir().mkpath(dir));
    }
    QFile::setPermissions(
        root + QStringLiteral("/run"),
        QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    const QList<QPair<const char *, QString>> env = {
        {"HOME", root + QStringLiteral("/home")},
        {"XDG_CONFIG_HOME", configHome},
        {"XDG_DATA_HOME", root + QStringLiteral("/data")},
        {"XDG_RUNTIME_DIR", root + QStringLiteral("/run")},
        {"QSOC_HOME", configHome + QStringLiteral("/qsoc")},
    };
    for (const auto &[key, value] : env) {
        QVERIFY(qputenv(key, value.toUtf8()));
    }
    const auto config = [](const char *dispatch) {
        return QByteArrayLiteral(
                   "proxy:\n  type: none\n"
                   "llm:\n  model: main-model\n  models:\n")
               + QByteArray(kModels).replace("\n", "\n    ")
               + QByteArrayLiteral(
                   "\nagent:\n  session_title: false\n  away_summary: false\n"
                   "  memory_extract: false\n  memory_dream: false\n  dispatch:\n")
               + dispatch;
    };
    QVERIFY(spill(configFile, config("    models: [pro]\n")));

    QSocAgentRuntimeOptions options;
    options.projectDirectory = root + QStringLiteral("/project");
    options.singleQuery      = true;
    QSocAgentRuntime session(options);
    QString          output;
    connect(
        &session,
        &QSocAgentRuntime::eventRaised,
        this,
        [&output](const QSocAgentRuntimeEvent &event) {
            if (event.kind == QSocAgentRuntimeEvent::Kind::Output) {
                output += event.text;
            }
        });
    const auto schema = [&session] {
        return session.agent()
            ->getToolRegistry()
            ->getTool(QStringLiteral("agent"))
            ->getParametersSchema();
    };
    const json before = schema();
    QCOMPARE(before["properties"]["model"]["enum"], json::array({"pro"}));
    QVERIFY(session.agent()->buildSystemPromptWithMemory().contains(
        QStringLiteral("- pro: Pro Model")));

    QVERIFY(spill(configFile, config("    models: [omni]\n    hosts:\n      sim1: {}\n")));
    QCOMPARE(schema(), before);

    QVERIFY(session.executeCommand(QStringLiteral("/dispatch")));
    QVERIFY2(output.contains(QStringLiteral("Dispatch models: pro")), qPrintable(output));
    QVERIFY(session.executeCommand(QStringLiteral("/dispatch reload")));
    QVERIFY2(output.contains(QStringLiteral("Dispatch models: omni")), qPrintable(output));
    QCOMPARE(schema()["properties"]["model"]["enum"], json::array({"omni"}));
    QVERIFY(schema()["properties"]["host"].contains("enum"));
    const QString prompt = session.agent()->buildSystemPromptWithMemory();
    QVERIFY2(prompt.contains(QStringLiteral("- omni: omni")), qPrintable(prompt));
    QVERIFY2(!prompt.contains(QStringLiteral("- pro: Pro Model")), qPrintable(prompt));
}

QSOC_TEST_MAIN(Test)
#include "test_qsocdispatchpolicy.moc"
