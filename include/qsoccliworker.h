#ifndef QSOCCLIWORKER_H
#define QSOCCLIWORKER_H

#include <QApplication>
#include <QCommandLineParser>
#include <QDebug>
#include <QFileInfo>
#include <QObject>

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
     * @brief QSocCliWorker
     * @param parent
     * @details This constructor will initialize the command line parser
     */
    explicit QSocCliWorker(QObject *parent = nullptr);
    /**
     * @brief ~QSocCliWorker
     * @details This destructor will free the command line parser
     */
    ~QSocCliWorker();
    /**
     * @brief setup
     * @param isGui
     * @details This function will setup the command line parser
     */
    void setup(bool isGui = false);
    /**
     * @brief process
     * @details This function will process the command line arguments
     */
    void process();

public slots:
    /**
     * @brief run
     * @details This function will run the command line parser
     */
    void run();

private:
    /**
     * @brief parser
     * @details This variable is the command line parser
     */
    QCommandLineParser parser;
    /**
     * @brief isGui
     * @details This variable indicates whether the application is running
     *          in GUI mode or not
     */
    void parseRoot(const QStringList &appArguments);
    /**
     * @brief parseProject
     * @param appArguments
     * @details This function will parse the project command line arguments
     */
    void parseProject(const QStringList &appArguments);
    /**
     * @brief parseProjectCreate
     * @param appArguments
     * @details This function will parse the project create command line arguments
     */
    void parseProjectCreate(const QStringList &appArguments);
    /**
     * @brief parseProjectUpdate
     * @param appArguments
     * @details This function will parse the project update command line arguments
     */
    void parseProjectUpdate(const QStringList &appArguments);
    /**
     * @brief parseSymbol
     * @param appArguments
     * @details This function will parse the symbol command line arguments
     */
    void parseSymbol(const QStringList &appArguments);
    /**
     * @brief parseSymbolImport
     * @param appArguments
     * @details This function will parse the symbol import command line arguments
     */
    void parseSymbolImport(const QStringList &appArguments);
    /**
     * @brief parseSymbolUpdate
     * @param appArguments
     * @details This function will parse the symbol update command line arguments
     */
    void parseSymbolUpdate(const QStringList &appArguments);
    /**
     * @brief parseSymbolRemove
     * @param appArguments
     * @details This function will parse the symbol remove command line arguments
     */
    void parseSymbolRemove(const QStringList &appArguments);
    void processFileList(const QString &fileListPath, const QStringList &filePathList);

signals:
    /**
     * @brief exit
     * @details This signal will be emitted when the application is exiting
     */
    void exit(int returnCode = 0);
    /**
     * @brief quit
     * @details This signal will be emitted when the application is quitting
     */
    void quit();
};

#endif // QSOCCLIWORKER_H
