#ifndef QSOCGENERATEMANAGER_H
#define QSOCGENERATEMANAGER_H

#include "common/qllmservice.h"
#include "common/qsocbusmanager.h"
#include "common/qsocmodulemanager.h"
#include "common/qsocprojectmanager.h"

#include <QObject>
#include <QString>
#include <QStringList>

#include <yaml-cpp/yaml.h>

/**
 * @brief The QSoCGenerateManager class.
 * @details This class is used to generate RTL code from netlist files.
 */
class QSoCGenerateManager : public QObject
{
    Q_OBJECT
public:
    /**
     * @brief Constructor.
     * @details This constructor will create an instance of this object.
     * @param[in] parent parent object.
     * @param[in] projectManager project manager.
     * @param[in] moduleManager module manager.
     * @param[in] busManager bus manager.
     * @param[in] llmService LLM service.
     */
    explicit QSoCGenerateManager(
        QObject            *parent         = nullptr,
        QSocProjectManager *projectManager = nullptr,
        QSocModuleManager  *moduleManager  = nullptr,
        QSocBusManager     *busManager     = nullptr,
        QLLMService        *llmService     = nullptr);

public slots:
    /**
     * @brief Set the project manager.
     * @details Assigns a new project manager to this object. The project
     *          manager is used for managing various project-related
     *          functionalities.
     * @param projectManager Pointer to the new project manager.
     */
    void setProjectManager(QSocProjectManager *projectManager);

    /**
     * @brief Set the module manager.
     * @details Assigns a new module manager to this object. The module manager
     *          is used for managing module-related functionalities.
     * @param moduleManager Pointer to the new module manager.
     */
    void setModuleManager(QSocModuleManager *moduleManager);

    /**
     * @brief Set the bus manager.
     * @details Assigns a new bus manager to this object. The bus manager
     *          is used for managing bus-related functionalities.
     * @param busManager Pointer to the new bus manager.
     */
    void setBusManager(QSocBusManager *busManager);

    /**
     * @brief Set the LLM service.
     * @details Assigns a new LLM service to this object.
     * @param llmService Pointer to the new LLM service.
     */
    void setLLMService(QLLMService *llmService);

    /**
     * @brief Get the project manager.
     * @details Retrieves the currently assigned project manager.
     * @return QSocProjectManager * Pointer to the current project manager.
     */
    QSocProjectManager *getProjectManager();

    /**
     * @brief Get the module manager.
     * @details Retrieves the currently assigned module manager.
     * @return QSocModuleManager * Pointer to the current module manager.
     */
    QSocModuleManager *getModuleManager();

    /**
     * @brief Get the bus manager.
     * @details Retrieves the currently assigned bus manager.
     * @return QSocBusManager * Pointer to the current bus manager.
     */
    QSocBusManager *getBusManager();

    /**
     * @brief Get the LLM service.
     * @details Retrieves the currently assigned LLM service.
     * @return QLLMService * Pointer to the current LLM service.
     */
    QLLMService *getLLMService();

    /**
     * @brief Load netlist file.
     * @details Loads a netlist file and creates an in-memory representation.
     * @param netlistFilePath Path to the netlist file.
     * @retval true Netlist file loaded successfully.
     * @retval false Failed to load netlist file.
     */
    bool loadNetlist(const QString &netlistFilePath);

    /**
     * @brief Process and expand the netlist.
     * @details Processes the loaded netlist, expanding buses into individual signals.
     * @retval true Netlist processed successfully.
     * @retval false Failed to process netlist.
     */
    bool processNetlist();

    /**
     * @brief Generate Verilog code from the processed netlist.
     * @details Generates Verilog code and saves it to the output directory.
     * @param outputFileName Output file name (without extension).
     * @retval true Verilog code generated and saved successfully.
     * @retval false Failed to generate or save Verilog code.
     */
    bool generateVerilog(const QString &outputFileName);

private:
    /** Project manager. */
    QSocProjectManager *projectManager = nullptr;
    /** Module manager. */
    QSocModuleManager *moduleManager = nullptr;
    /** Bus manager. */
    QSocBusManager *busManager = nullptr;
    /** LLM service. */
    QLLMService *llmService = nullptr;
    /** Netlist data. */
    YAML::Node netlistData;
};

#endif // QSOCGENERATEMANAGER_H