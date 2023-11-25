#ifndef QSOCCLIWORKER_H
#define QSOCCLIWORKER_H

#include <QApplication>
#include <QCommandLineParser>
#include <QFileInfo>
#include <QObject>
#include <QStringList>

/**
 * @brief   The QSocCliWorker class
 * @details This class is the main worker class for the socstudio application.
 *          It is responsible for parsing the command line arguments and
 *          executing the appropriate actions.
 */
class QSocCliWorker : public QObject
{
    Q_OBJECT
public:
    /**
     * @brief Constructor for QSocCliWorker
     * @details This constructor will initialize the command line parser.
     * @param[in] parent parent object
     */
    explicit QSocCliWorker(QObject *parent = nullptr);
    /**
     * @brief Destructor for QSocCliWorker
     * @details This destructor will free the command line parser.
     */
    ~QSocCliWorker();
    /**
     * @brief Setup the command line parser
     * @details This function will setup the command line parser.
     * @param appArguments command line arguments for the application
     * @param isGui indicates whether the application is running
     *              in GUI mode or not, default is false.
     *              - true indicates GUI mode
     *              - false indicates CLI mode
     */
    void setup(const QStringList &appArguments, bool isGui = false);
    /**
     * @brief Process the command line arguments
     * @details This function will process the command line arguments.
     */
    void process();

public slots:
    /**
     * @brief Run the command line parser
     * @details This function will run the command line parser.
     */
    void run();

private:
    /* Command line parser */
    QCommandLineParser parser;
    /* Command line arguments */
    QStringList cmdArguments;
    /* ExitCode of the application */
    int exitCode;

    /**
     * @brief Parse the application command line arguments
     * @details This function will parse the application command line arguments.
     * @param appArguments command line arguments
     * @retval true Parse successfully
     * @retval false Parse failed
     */
    bool parseRoot(const QStringList &appArguments);
    /**
     * @brief Parse the project command line arguments
     * @details This function will parse the project command line arguments.
     * @param appArguments command line arguments
     * @retval true Parse successfully
     * @retval false Parse failed
     */
    bool parseProject(const QStringList &appArguments);
    /**
     * @brief Parse the project create command line arguments
     * @details This function will parse the project create command line
     *          arguments.
     * @param appArguments command line arguments
     * @retval true Parse successfully
     * @retval false Parse failed

     */
    bool parseProjectCreate(const QStringList &appArguments);
    /**
     * @brief Parse the project update command line arguments
     * @details This function will parse the project update command line
     *          arguments.
     * @param appArguments command line arguments
     * @retval true Parse successfully
     * @retval false Parse failed
     */
    bool parseProjectUpdate(const QStringList &appArguments);
    /**
     * @brief Parse the project remove command line arguments
     * @details This function will parse the project remove command line
     *          arguments.
     * @param appArguments command line arguments
     * @retval true Parse successfully
     * @retval false Parse failed
     */
    bool parseProjectRemove(const QStringList &appArguments);
    /**
     * @brief Parse the project list command line arguments
     * @details This function will parse the project list command line
     *          arguments.
     * @param appArguments command line arguments
     * @retval true Parse successfully
     * @retval false Parse failed
     */
    bool parseProjectList(const QStringList &appArguments);
    /**
     * @brief Parse the project show command line arguments
     * @details This function will parse the project show command line
     *          arguments.
     * @param appArguments command line arguments
     * @retval true Parse successfully
     * @retval false Parse failed
     */
    bool parseProjectShow(const QStringList &appArguments);
    /**
     * @brief Parse the module command line arguments
     * @details This function will parse the module command line arguments.
     * @param appArguments command line arguments
     * @retval true Parse successfully
     * @retval false Parse failed
     */
    bool parseModule(const QStringList &appArguments);
    /**
     * @brief Parse the module import command line arguments
     * @details This function will parse the module import command line
     *          arguments.
     * @param appArguments command line arguments
     * @retval true Parse successfully
     * @retval false Parse failed
     */
    bool parseModuleImport(const QStringList &appArguments);
    /**
     * @brief Parse the module remove command line arguments
     * @details This function will parse the module remove command line
     *          arguments.
     * @param appArguments command line arguments
     * @retval true Parse successfully
     * @retval false Parse failed
     */
    bool parseModuleRemove(const QStringList &appArguments);
    /**
     * @brief Parse the module list command line arguments
     * @details This function will parse the module list command line arguments.
     * @param appArguments command line arguments
     * @retval true Parse successfully
     * @retval false Parse failed
     */
    bool parseModuleList(const QStringList &appArguments);
    /**
     * @brief Parse the module show command line arguments
     * @details This function will parse the module show command line arguments.
     * @param appArguments command line arguments
     * @retval true Parse successfully
     * @retval false Parse failed
     */
    bool parseModuleShow(const QStringList &appArguments);
    /**
     * @brief Parse the bus command line arguments
     * @details This function will parse the bus command line arguments.
     * @param appArguments command line arguments
     * @retval true Parse successfully
     * @retval false Parse failed
     */
    bool parseBus(const QStringList &appArguments);
    /**
     * @brief Parse the bus import command line arguments
     * @details This function will parse the bus import command line
     *          arguments.
     * @param appArguments command line arguments
     * @retval true Parse successfully
     * @retval false Parse failed
     */
    bool parseBusImport(const QStringList &appArguments);
    /**
     * @brief Parse the bus remove command line arguments
     * @details This function will parse the bus remove command line
     *          arguments.
     * @param appArguments command line arguments
     * @retval true Parse successfully
     * @retval false Parse failed
     */
    bool parseBusRemove(const QStringList &appArguments);
    /**
     * @brief Parse the bus list command line arguments
     * @details This function will parse the bus list command line arguments.
     * @param appArguments command line arguments
     * @retval true Parse successfully
     * @retval false Parse failed
     */
    bool parseBusList(const QStringList &appArguments);
    /**
     * @brief Parse the bus show command line arguments
     * @details This function will parse the bus show command line arguments.
     * @param appArguments command line arguments
     * @retval true Parse successfully
     * @retval false Parse failed
     */
    bool parseBusShow(const QStringList &appArguments);
    /**
     * @brief Show application name and version and emit exit with exitCode
     * @details This function will show application name and version and emit
     *          exit with exitCode.
     * @param exitCode exit code to emit
     * @return bool always true

     */
    bool showVersion(int exitCode);
    /**
     * @brief Show help message and emit exit with exitCode
     * @details This function will show help message and emit exit with
     *          exitCode.
     * @param exitCode The exit code to emit
     * @return bool always true
     */
    bool showHelp(int exitCode);
    /**
     * @brief Show error message and emit exit with exitCode
     * @details This function will show error message and emit exit with
     *          exitCode.
     * @param exitCode The exit code to emit
     * @param message The error message to show
     * @return bool always false

     */
    bool showError(int exitCode, const QString &message);
    /**
     * @brief Show info message and emit exit with exitCode
     * @details This function will show info message and emit exit with
     *          exitCode.
     * @param exitCode The exit code to emit
     * @param message The info message to show
     * @return bool always true
     */
    bool showInfo(int exitCode, const QString &message);
    /**
     * @brief Show help message or error message and emit exit
     * @details This function will show error message if no help flag is set,
     *          or show help message if help flag is set. It exit with exitCode
     *          when it is error, otherwise it exit with 0.
     * @param exitCode The exit code to emit
     * @param message The error message to show if no help flag is set
     * @retval true It is a help message
     * @retval false It is an error message
     */
    bool showHelpOrError(int exitCode, const QString &message);

signals:
    /**
     * @brief Exit the application
     * @details This signal will be emitted when exit the application.
     * @param returnCode The return code of the application
     */
    void exit(int returnCode = 0);
    /**
     * @brief Quit the application
     * @details This signal will be emitted when quit the application.
     */
    void quit();
};

#endif // QSOCCLIWORKER_H
