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
     * @brief Constructor for QSocCliWorker
     * @param parent parent object
     * @details This constructor will initialize the command line parser
     */
    explicit QSocCliWorker(QObject *parent = nullptr);
    /**
     * @brief Destructor for QSocCliWorker
     * @details This destructor will free the command line parser
     */
    ~QSocCliWorker();
    /**
     * @brief Setup the command line parser
     * @param isGui indicates whether the application is running
     *              in GUI mode or not, default is false
     * @details This function will setup the command line parser
     */
    void setup(bool isGui = false);
    /**
     * @brief Process the command line arguments
     * @details This function will process the command line arguments
     */
    void process();

public slots:
    /**
     * @brief Run the command line parser
     * @details This function will run the command line parser
     */
    void run();

private:
    /* Command line parser */
    QCommandLineParser parser;

    /**
     * @brief Parse the application command line arguments
     * @param appArguments command line arguments
     * @details This function will parse the application command line arguments
     */
    void parseRoot(const QStringList &appArguments);
    /**
     * @brief Parse the project command line arguments
     * @param appArguments command line arguments
     * @details This function will parse the project command line arguments
     */
    void parseProject(const QStringList &appArguments);
    /**
     * @brief Parse the project create command line arguments
     * @param appArguments command line arguments
     * @details This function will parse the project create command line arguments
     */
    void parseProjectCreate(const QStringList &appArguments);
    /**
     * @brief Parse the project update command line arguments
     * @param appArguments command line arguments
     * @details This function will parse the project update command line arguments
     */
    void parseProjectUpdate(const QStringList &appArguments);
    /**
     * @brief Parse the symbol command line arguments
     * @param appArguments command line arguments
     * @details This function will parse the symbol command line arguments
     */
    void parseSymbol(const QStringList &appArguments);
    /**
     * @brief Parse the symbol import command line arguments
     * @param appArguments command line arguments
     * @details This function will parse the symbol import command line arguments
     */
    void parseSymbolImport(const QStringList &appArguments);
    /**
     * @brief Parse the symbol update command line arguments
     * @param appArguments command line arguments
     * @details This function will parse the symbol update command line arguments
     */
    void parseSymbolUpdate(const QStringList &appArguments);
    /**
     * @brief Parse the symbol remove command line arguments
     * @param appArguments command line arguments
     * @details This function will parse the symbol remove command line arguments
     */
    void parseSymbolRemove(const QStringList &appArguments);
    void processFileList(const QString &fileListPath, const QStringList &filePathList);

signals:
    /**
     * @brief Exit the application
     * @details This signal will be emitted when exit the application
     */
    void exit(int returnCode = 0);
    /**
     * @brief Quit the application
     * @details This signal will be emitted when quit the application
     */
    void quit();
};

#endif // QSOCCLIWORKER_H
