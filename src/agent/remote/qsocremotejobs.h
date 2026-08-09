// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCREMOTEJOBS_H
#define QSOCREMOTEJOBS_H

#include <QList>
#include <QString>
#include <QStringList>

#include <cstdint>

/**
 * @brief Whether two identity strings name the same thing.
 * @details Unknown is a third answer, not a soft mismatch: it says the
 *          question could not be asked. A caller that folds it into Differs
 *          reports a reboot that may not have happened; one that folds it
 *          into Equal signals a pid it never identified.
 */
enum class QSocRemoteIdentityMatch : std::uint8_t {
    Equal,   /**< Same scheme, same value. */
    Differs, /**< Same scheme, different value. */
    Unknown, /**< Missing, untagged, or two different schemes. */
};

/**
 * @brief Compare two scheme-tagged identity strings.
 * @details An identity is `<scheme>:<value>`. Two identities are comparable
 *          only when their schemes match, so a host that answered
 *          `boot_id:` once and `init_lstart:` the next time yields Unknown
 *          rather than a fabricated mismatch. `unknown:`, an empty string,
 *          an untagged string and an empty value all yield Unknown.
 *
 *          The reference form of the rule the generated scripts apply on the
 *          host. It decides nothing on its own: whether a pid may be signalled
 *          is answered once, by @ref jobSignalScript.
 * @param recorded Identity captured when the job was launched.
 * @param live Identity the host reports now.
 * @return What the two establish about each other.
 */
QSocRemoteIdentityMatch compareIdentity(const QString &recorded, const QString &live);

/**
 * @brief What the generated job script established, as one word.
 * @details The scripts print at most one `token: <name>` line, and print it
 *          before the `log:` fence past which the job's own bytes begin.
 *          Absent means the script answered with no reservation, which only
 *          the status and output scripts can do.
 */
enum class QSocRemoteJobToken : std::uint8_t {
    Absent,       /**< No token line: nothing was in doubt. */
    NoJob,        /**< No job directory on the host. */
    NoPid,        /**< No usable pid recorded for the job. */
    Unverifiable, /**< The host cannot identify its own incarnation or pid. */
    BootMismatch, /**< The host restarted; the recorded pid is not the job. */
    ProcessGone,  /**< Same host incarnation, and the pid is not there. */
    PidReused,    /**< The pid exists but started at a different time. */
    Signalled,    /**< The signal was delivered to the verified pid. */
    SignalFailed, /**< The guards passed and the host refused the signal. */
    Unrecognized, /**< A token line this build does not know. */
};

/** @brief Wire name of @p token, as the generated scripts print it. */
QString jobTokenName(QSocRemoteJobToken token);

/**
 * @brief Line past which a script's stdout is the job's own output.
 * @details The script prints it; everything after it was written by the
 *          payload and establishes nothing about the host.
 */
QString jobLogFence();

/**
 * @brief Read the `token:` line out of one script's stdout.
 * @details Reading stops at @ref jobLogFence, so a job that prints
 *          `token: boot_mismatch` into its own log cannot hand the client a
 *          verdict about the host it is running on.
 * @param scriptOutput Raw stdout of a job script.
 * @return The token, or Absent when the script printed none.
 */
QSocRemoteJobToken parseJobToken(const QString &scriptOutput);

/**
 * @brief One script's stdout with its verdict lines removed.
 * @details The composed result text already carries the verdict, so echoing
 *          the script's own copy would state it twice. Only the region ahead
 *          of @ref jobLogFence is filtered: stripping a `token:` line out of
 *          the tailed log would hide the one place a reader can see that the
 *          job, not the host, wrote it.
 */
QString jobScriptEvidence(const QString &scriptOutput);

/**
 * @brief Exit code the status script reported, as the host printed it.
 * @return The value of the `exit_code=` line, or an empty string when the job
 *         has not finished or the host printed nothing readable.
 */
QString parseJobStatusExitCode(const QString &statusOutput);

/**
 * @brief One background job as the client last observed it.
 * @details The three identity fields are what make a pid signallable. Each
 *          covers what the others cannot: @ref generation is the only thing
 *          that knows the transport was replaced, @ref bootIdentity is the
 *          only thing that knows the host restarted, and @ref pidStart is the
 *          only thing that catches a reused pid on a host that never
 *          restarted.
 */
struct QSocRemoteJobRecord
{
    QString jobId;              /**< Opaque id, also the job directory name. */
    QString commandLine;        /**< Command as handed to the remote shell. */
    quint64 generation = 0;     /**< Transport the job was launched over. */
    QString bootIdentity;       /**< Host incarnation at launch, scheme-tagged. */
    QString pidStart;           /**< Start identity of @ref pid, scheme-tagged. */
    qint64  pid        = 0;     /**< Remote pid; 0 when none was captured. */
    qint64  launchedMs = 0;     /**< Client clock at launch, ms since epoch. */
    bool    settled    = false; /**< An exit code was observed for this job. */
};

/**
 * @brief Insertion-ordered set of background jobs, keyed by job id.
 * @details A plain value type: no QObject, no signals, no ownership of
 *          anything. Meant to be held by whatever owns the transport, so a
 *          reconnect can name exactly the jobs whose outcome is now unknown.
 *
 *          The cap bounds a long session without ever losing an unsettled
 *          job: @ref note drops the oldest settled record to make room and
 *          refuses to insert when every record is still unsettled. A caller
 *          that ignores the refusal reports a job it cannot later verify,
 *          which is the failure the record exists to prevent.
 */
class QSocRemoteJobLedger
{
public:
    /** @brief Records kept at once. */
    static constexpr int kCapacity = 64;

    /**
     * @brief Remember a job that was just launched.
     * @details An id already present keeps its existing record and position,
     *          so a repeated note cannot rewrite the identity a signal is
     *          checked against.
     * @param record Job to remember; its jobId must not be empty.
     * @return True when the ledger holds a record for the id afterwards.
     */
    bool note(const QSocRemoteJobRecord &record);

    /** @brief Whether a record exists for @p jobId. */
    bool has(const QString &jobId) const;

    /**
     * @brief The record for @p jobId.
     * @return The record, or a default-constructed one (empty jobId) when the
     *         ledger has never heard of the id.
     */
    QSocRemoteJobRecord record(const QString &jobId) const;

    /**
     * @brief Restamp a verified job onto the transport that is bound now.
     * @details Only for a job whose host-side identity was just confirmed;
     *          the stamp then stops naming it as unverified on every later
     *          call. Identity fields are never touched here.
     * @return True when a record was restamped.
     */
    bool rebind(const QString &jobId, quint64 generation);

    /**
     * @brief Mark @p jobId as having a known outcome.
     * @details The outcome is an exit code the host reported for the job. It is
     *          the only thing that settles a record: it is what says the pid
     *          can no longer be signalled, so it is what makes the record safe
     *          to evict and pointless to re-observe after a reconnect.
     * @return True when found.
     */
    bool markSettled(const QString &jobId);

    /** @brief Drop the record for @p jobId. @return True when one was dropped. */
    bool forget(const QString &jobId);

    /** @brief Ids of jobs with no known outcome, in launch order. */
    QStringList liveJobIds() const;

    /** @brief Drop every record. */
    void clear();

    /**
     * @brief Whether @ref note would refuse a new id.
     * @details True only when the ledger is at capacity and has no settled
     *          record to evict.
     */
    bool isFull() const;

    /** @brief Records held now. */
    int size() const { return static_cast<int>(m_records.size()); }

private:
    QList<QSocRemoteJobRecord> m_records;
};

/** @brief What one signal attempt established, and the text that says so. */
struct QSocRemoteJobJudgement
{
    bool    signalled = false; /**< The host delivered the signal to the pid. */
    QString text;              /**< Composed result text; never empty. */
    /* The host-incarnation part of the same verdict. Carried out rather than
     * left for the caller to re-derive: a caller that compares identities
     * again to decide whether to drop the record is a second place the same
     * question is answered, and the two can disagree. */
    QSocRemoteIdentityMatch boot = QSocRemoteIdentityMatch::Unknown;
};

/**
 * @brief Refuse a signal for an id this session never recorded.
 * @details The only part of the question no host can answer: the ledger is
 *          what ties an id to a pid, and a remote job directory cannot say who
 *          launched it. Costs no round trip, so it runs before the script.
 * @param jobId Id the caller asked about.
 * @param ledger Jobs this session recorded.
 * @return Refusal text, or an empty string when the id is on record.
 */
QString refuseUnrecordedJob(const QString &jobId, const QSocRemoteJobLedger &ledger);

/**
 * @brief Read the signal script's verdict into a decision to act on.
 * @details The recorded identities are compared against the live ones in one
 *          place only, inside @ref jobSignalScript: the comparison has to be
 *          on the same round trip as the `kill` it guards, or the pid can exit
 *          and its number be reused in the window between deciding and
 *          signalling. What is compared there is the ledger's copy: the host
 *          decides, and the client says what it decides about. This translates
 *          that verdict and compares nothing.
 *
 *          The transport generation is not an input. It tracks the client's
 *          socket, and neither the host incarnation nor the pid nor the pid's
 *          start time is a function of that socket, so it can neither verify
 *          nor veto: refusing a verified pid would push the caller toward a
 *          command-line matching `pkill`, which is broader than the one pid it
 *          wanted.
 * @param jobId Job that was addressed.
 * @param signalName Signal name for the reader, e.g. `SIGTERM`.
 * @param token What the signal script established.
 * @param detail Extra host output, e.g. the message `kill` printed.
 * @return The verdict, its host-incarnation part, and the text to return.
 */
QSocRemoteJobJudgement judgeSignal(
    const QString     &jobId,
    const QString     &signalName,
    QSocRemoteJobToken token,
    const QString     &detail);

/* Generated shell. The LIVE identity is probed on the host, inside the job
 * scripts: only the host knows its own incarnation, and a separate
 * health-check round trip would be paid by every tool call. The RECORDED
 * identity is always baked in as a literal from the ledger. A job directory
 * lives in a writable remote directory, so a `pid` file read on the host is an
 * expectation the attacker supplied, and comparing it against the live host
 * attests to nothing.
 *
 * Portability of the answers: exact on Linux with procfs (`boot_id` and
 * `proc_starttime`), second-resolution on macOS and other BSD hosts
 * (`init_lstart`, `kern_boottime`, `pid_lstart`), and `unknown:` on a host
 * with neither procfs nor a `ps` that takes `-o`. Unknown is the state that
 * says so; it is never silently treated as a match. */

/** @brief Shell expression printing the host boot identity, scheme-tagged. */
QString bootIdentityProbe();

/**
 * @brief Shell definition of `__cmp`, the host-side form of @ref
 *        compareIdentity.
 * @details Answers `unknown`, `equal` or `differ` for the pair of identities it
 *          is handed, by the same rule, including the one an empty payload
 *          falls under: `boot_id:` against `boot_id:` is Unknown, not a match.
 */
QString identityCompareShell();

/**
 * @brief Shell expression printing one pid's start identity, scheme-tagged.
 * @param pidRef Shell expression naming the pid, already quoted by the caller.
 */
QString pidStartProbe(const QString &pidRef);

/**
 * @brief Script that launches a background job and reports its identity.
 * @details Prints `job_id=`, `boot_id=`, `pid=` and `pid_start=` on the same
 *          round trip that starts the job, so the client records the binding
 *          with no second call. The shell that waits for the payload is the
 *          one that must survive the link dropping: it ignores SIGHUP before
 *          it spawns anything, so `exit_code` is still written for a job that
 *          finished after the channel closed.
 * @param jobDir Absolute remote job directory.
 * @param cwd Working directory for the payload.
 * @param jobId Id echoed back on stdout.
 * @param command Payload command, unescaped.
 */
QString jobLaunchScript(
    const QString &jobDir, const QString &cwd, const QString &jobId, const QString &command);

/**
 * @brief Script reporting one job's state.
 * @details Prints no `running=` key at all when the binding cannot be
 *          verified: `running=no` would read as a definite answer about a
 *          process nobody identified. The pid it probes is @p record's, and
 *          every value it echoes from the job directory is narrowed to digits
 *          first, so a state file cannot inject a `token:` line ahead of the
 *          verdict.
 * @param jobDir Absolute remote job directory, read for the log and exit code.
 * @param record What this session recorded for the job; a default-constructed
 *        one is an id this session cannot verify, and says so.
 */
QString jobStatusScript(const QString &jobDir, const QSocRemoteJobRecord &record);

/**
 * @brief Script tailing one job's log, prefixed by its verification token.
 * @details The token comes before @ref jobLogFence and the log after it, so
 *          the tail cannot be read as the host's verdict about itself.
 */
QString jobOutputScript(const QString &jobDir, int maxLines, const QSocRemoteJobRecord &record);

/**
 * @brief Script signalling one job, with every guard ahead of the signal.
 * @details The one place a recorded identity is compared against a live one to
 *          decide whether to signal. Each mismatch path prints its token and
 *          exits before the single `kill`, so no host state reachable by the
 *          guards can fall through to it. No compiler and no static analyser
 *          reads shell, so the guards are held in place behaviourally: the
 *          script is run against a real process, and the process must survive
 *          every mismatch branch.
 *
 *          It reads nothing from the job directory, and takes no job directory
 *          to read: the pid it signals and the two identities it checks that
 *          pid against are @p record's, baked in as literals. A recorded
 *          `unknown:` therefore refuses the signal instead of falling back to
 *          a value the host could rewrite.
 * @param record What this session recorded for the job.
 * @param signal Signal flag for `kill`, e.g. `-TERM`.
 */
QString jobSignalScript(const QSocRemoteJobRecord &record, const QString &signal);

/** @brief The four identity fields a launch script printed. */
struct QSocRemoteJobLaunchReport
{
    QString jobId;        /**< Id the script echoed. */
    QString bootIdentity; /**< Host incarnation at launch. */
    QString pidStart;     /**< Start identity of the payload pid. */
    qint64  pid = 0;      /**< Payload pid; 0 when the script printed none. */
};

/** @brief Parse the `key=value` lines a launch script printed. */
QSocRemoteJobLaunchReport parseJobLaunchOutput(const QString &stdoutText);

/**
 * @brief The one composer for an uncertain job result.
 * @details Every refusal goes through here, so an uncertain outcome is never
 *          reachable by omitting a status line. A refusal is never Failed:
 *          nothing about the job was established, and a Failed result invites
 *          the retry that a broad `pkill` is made of.
 */
QString composeJobUncertain(const QString &jobId, const QString &reason, const QString &next);

/** @brief Uncertain text for @p token, with its reason and next step. */
QString composeJobRefusal(const QString &jobId, QSocRemoteJobToken token);

/**
 * @brief Result text for one signal attempt.
 * @param jobId Job that was addressed.
 * @param signalName Signal name for the reader, e.g. `SIGTERM`.
 * @param token What the signal script established.
 * @param detail Extra host output, e.g. the message `kill` printed.
 */
QString composeJobSignalResult(
    const QString     &jobId,
    const QString     &signalName,
    QSocRemoteJobToken token,
    const QString     &detail);

/**
 * @brief Result text for a launched job, including any honest degradation.
 * @details A host that answered `unknown:` to either probe still gets its job
 *          launched and still reports ok. What it also gets is the line
 *          saying the job cannot be signalled after a reconnect, because
 *          silently degrading to a pid-only kill is the defect this binding
 *          exists to remove.
 */
QString composeJobLaunchResult(const QSocRemoteJobRecord &record, const QString &jobDir);

/** @brief The line to add when a full ledger could not record a launched job. */
QString jobLedgerFullNote();

#endif // QSOCREMOTEJOBS_H
