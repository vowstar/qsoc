// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2023-2025 Huang Rui <vowstar@gmail.com>

#include "socmoduleitem.h"

#include <qschematic/items/connector.hpp>
#include <qschematic/items/label.hpp>

#include <QBrush>
#include <QDebug>
#include <QFont>
#include <QFontMetrics>
#include <QPainter>
#include <QPen>

using namespace ModuleLibrary;

SocModuleItem::SocModuleItem(
    const QString &moduleName, const YAML::Node &moduleYaml, int type, QGraphicsItem *parent)
    : QSchematic::Items::Node(type, parent)
    , m_moduleName(moduleName)
    , m_moduleYaml(moduleYaml)
{
    // Create module name label
    m_label = std::make_shared<QSchematic::Items::Label>();
    m_label->setParentItem(this);
    m_label->setVisible(true);
    m_label->setMovable(false);
    m_label->setText(m_moduleName);
    m_label->setHasConnectionPoint(false);

    // Set initial properties
    setAllowMouseResize(false);
    setAllowMouseRotate(false);
    setConnectorsMovable(false);
    setConnectorsSnapPolicy(QSchematic::Items::Connector::NodeSizerectOutline);
    setConnectorsSnapToGrid(true);

    // Create ports from YAML data
    createPortsFromYaml();

    // Connect signals
    connect(this, &QSchematic::Items::Node::sizeChanged, this, &SocModuleItem::updateLabelPosition);
    connect(this, &QSchematic::Items::Item::settingsChanged, [this] {
        m_label->setSettings(_settings);
    });
}

QString SocModuleItem::moduleName() const
{
    return m_moduleName;
}

void SocModuleItem::setModuleName(const QString &name)
{
    m_moduleName = name;
    if (m_label) {
        m_label->setText(name);
    }
    update();
}

YAML::Node SocModuleItem::moduleYaml() const
{
    return m_moduleYaml;
}

void SocModuleItem::setModuleYaml(const YAML::Node &yaml)
{
    m_moduleYaml = yaml;

    // Clear existing ports
    for (auto &port : m_ports) {
        removeConnector(port);
    }
    m_ports.clear();

    // Recreate ports
    createPortsFromYaml();
}

std::shared_ptr<QSchematic::Items::Item> SocModuleItem::deepCopy() const
{
    auto copy = std::make_shared<SocModuleItem>(m_moduleName, m_moduleYaml, type());
    copy->setPos(pos());
    copy->setRotation(rotation());
    copy->setSize(size());

    return copy;
}

void SocModuleItem::paint(QPainter *painter, const QStyleOptionGraphicsItem *option, QWidget *widget)
{
    Q_UNUSED(option)
    Q_UNUSED(widget)

    // Set up drawing
    QPen   pen(Qt::black, 2.0);
    QBrush brush(QColor(240, 240, 255));

    painter->setPen(pen);
    painter->setBrush(brush);

    // Draw module rectangle
    QRectF rect = sizeRect();
    painter->drawRect(rect);

    // Draw module name
    painter->setPen(QPen(Qt::black));
    QFont font = painter->font();
    font.setBold(true);
    font.setPointSize(10);
    painter->setFont(font);

    QRectF textRect(0, 5, rect.width(), LABEL_HEIGHT);
    painter->drawText(textRect, Qt::AlignCenter, m_moduleName);

    // Draw separator line
    painter->drawLine(0, LABEL_HEIGHT + 5, rect.width(), LABEL_HEIGHT + 5);
}

void SocModuleItem::createPortsFromYaml()
{
    if (!m_moduleYaml || !m_moduleYaml["port"]) {
        qDebug() << "No port data found in module YAML for" << m_moduleName;
        return;
    }

    const YAML::Node &ports = m_moduleYaml["port"];

    QStringList inputPorts;
    QStringList outputPorts;

    // Categorize ports by direction
    for (const auto &portPair : ports) {
        const std::string portName = portPair.first.as<std::string>();
        const YAML::Node &portData = portPair.second;

        if (portData["direction"]) {
            const std::string direction  = portData["direction"].as<std::string>();
            const QString     portNameQt = QString::fromStdString(portName);

            if (direction == "in" || direction == "input") {
                inputPorts.append(portNameQt);
            } else if (direction == "out" || direction == "output") {
                outputPorts.append(portNameQt);
            }
        }
    }

    // Calculate required size
    const int   maxPorts       = qMax(inputPorts.size(), outputPorts.size());
    const qreal requiredHeight = qMax(MIN_HEIGHT, LABEL_HEIGHT + 20 + maxPorts * PORT_SPACING);
    const qreal requiredWidth  = MIN_WIDTH;

    setSize(requiredWidth, requiredHeight);

    // Create input ports (left side)
    for (int i = 0; i < inputPorts.size(); ++i) {
        QPoint gridPos(
            0,
            static_cast<int>(
                (LABEL_HEIGHT + 15 + i * PORT_SPACING) / 10)); // Convert to grid coordinates
        auto connector = std::make_shared<QSchematic::Items::Connector>(
            QSchematic::Items::Item::ConnectorType, gridPos, inputPorts[i], this);
        connector->setText(inputPorts[i]);
        addConnector(connector);
        m_ports.append(connector);
    }

    // Create output ports (right side)
    for (int i = 0; i < outputPorts.size(); ++i) {
        QPoint gridPos(
            static_cast<int>(requiredWidth / 10),
            static_cast<int>(
                (LABEL_HEIGHT + 15 + i * PORT_SPACING) / 10)); // Convert to grid coordinates
        auto connector = std::make_shared<QSchematic::Items::Connector>(
            QSchematic::Items::Item::ConnectorType, gridPos, outputPorts[i], this);
        connector->setText(outputPorts[i]);
        addConnector(connector);
        m_ports.append(connector);
    }

    updateLabelPosition();
}

QSizeF SocModuleItem::calculateRequiredSize() const
{
    if (!m_moduleYaml || !m_moduleYaml["port"]) {
        return QSizeF(MIN_WIDTH, MIN_HEIGHT);
    }

    const YAML::Node &ports       = m_moduleYaml["port"];
    int               inputCount  = 0;
    int               outputCount = 0;

    for (const auto &portPair : ports) {
        const YAML::Node &portData = portPair.second;

        if (portData["direction"]) {
            const std::string direction = portData["direction"].as<std::string>();

            if (direction == "in" || direction == "input") {
                inputCount++;
            } else if (direction == "out" || direction == "output") {
                outputCount++;
            }
        }
    }

    const int   maxPorts       = qMax(inputCount, outputCount);
    const qreal requiredHeight = qMax(MIN_HEIGHT, LABEL_HEIGHT + 20 + maxPorts * PORT_SPACING);

    return QSizeF(MIN_WIDTH, requiredHeight);
}

void SocModuleItem::arrangePorts()
{
    // This function is called after ports are created to arrange them properly
    // Implementation is already handled in createPortsFromYaml()
}

void SocModuleItem::updateLabelPosition()
{
    if (m_label) {
        QRectF rect = sizeRect();
        m_label->setPos(rect.center().x() - m_label->boundingRect().width() / 2, 8);
    }
}
