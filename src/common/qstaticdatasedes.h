#ifndef QSTATICDATASEDES_H
#define QSTATICDATASEDES_H

#include <QObject>
#include <QString>

#include <nlohmann/json.hpp>
#include <yaml-cpp/yaml.h>

using json = nlohmann::json;

/**
 * @brief The QStaticDataSedes class.
 * @details This class provides static methods for serializing and deserializing
 *          data between YAML::Node, JSON (nlohmann::json), and QString. It is
 *          designed as a utility class in a Qt environment, leveraging Qt's
 *          string handling capabilities. It's a static-only class, meaning it
 *          cannot be instantiated, but provides its functionality through
 *          static methods.
 */
class QStaticDataSedes : public QObject
{
    Q_OBJECT

public:
    /**
     * @brief Get the static instance of this object.
     * @details This function returns the static instance of this object. It is
     *          used to provide a singleton instance of the class, ensuring that
     *          only one instance of the class exists throughout the
     *          application.
     * @return The static instance of QStaticDataSedes.
     */
    static QStaticDataSedes &instance()
    {
        static QStaticDataSedes instance;
        return instance;
    }

public slots:
    /**
     * @brief Serialize YAML Node to QString.
     * @details This function serializes a YAML::Node to a QString.
     * @param node The YAML::Node to serialize.
     * @return Serialized QString representation of the YAML::Node.
     */
    static QString serializeYaml(const YAML::Node &node);

    /**
     * @brief Deserialize QString to YAML Node.
     * @details This function deserializes a QString to a YAML::Node.
     * @param str The QString to deserialize.
     * @return Deserialized YAML::Node.
     */
    static YAML::Node deserializeYaml(const QString &str);

    /**
     * @brief Serialize JSON to QString.
     * @details This function serializes a JSON object to a QString.
     * @param jsonObject The JSON object to serialize.
     * @return Serialized QString representation of the JSON object.
     */
    static QString serializeJson(const json &jsonObject);

    /**
     * @brief Deserialize QString to JSON.
     * @details This function deserializes a QString to a JSON object.
     * @param str The QString to deserialize.
     * @return Deserialized JSON object.
     */
    static json deserializeJson(const QString &str);

private:
    /**
     * @brief Constructor.
     * @details This is a private constructor for QStaticDataSedes to prevent
     *          instantiation. Making the constructor private ensures that no
     *          objects of this class can be created from outside the class,
     *          enforcing a static-only usage pattern.
     */
    QStaticDataSedes() {}
};

#endif // QSTATICDATASEDES_H
