#include "qstaticdatasedes.h"

#include <sstream>

QString QStaticDataSedes::serializeYaml(const YAML::Node &node)
{
    std::stringstream stream;
    stream << node;
    return QString::fromStdString(stream.str());
}

YAML::Node QStaticDataSedes::deserializeYaml(const QString &str)
{
    const std::string stdString = str.toStdString();
    const YAML::Node  node      = YAML::Load(stdString);
    return node;
}

QString QStaticDataSedes::serializeJson(const json &jsonObject)
{
    const std::string serialized = jsonObject.dump();
    return QString::fromStdString(serialized);
}

json QStaticDataSedes::deserializeJson(const QString &str)
{
    const std::string stdString = str.toStdString();
    return json::parse(stdString);
}
