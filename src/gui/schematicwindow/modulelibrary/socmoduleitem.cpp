// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2023-2025 Huang Rui <vowstar@gmail.com>

#include "socmoduleitem.h"
#include "socmoduleconnector.h"

#include <qschematic/items/connector.hpp>
#include <qschematic/items/label.hpp>

#include <gpds/container.hpp>
#include <yaml-cpp/yaml.h>

#include <QBrush>
#include <QDebug>
#include <QFont>
#include <QFontMetrics>
#include <QPainter>
#include <QPen>
#include <QSet>

using namespace ModuleLibrary;

SocModuleItem::SocModuleItem(
    const QString &moduleName, const YAML::Node &moduleYaml, int type, QGraphicsItem *parent)
    : QSchematic::Items::Node(type, parent)
    , m_moduleName(moduleName)
    , m_instanceName(moduleName) // Default to module name, will be set properly when added to scene
    , m_moduleYaml(moduleYaml)
{
    // Create instance name label
    m_label = std::make_shared<QSchematic::Items::Label>();
    m_label->setParentItem(this);
    m_label->setVisible(true);
    m_label->setMovable(true);        // Make label draggable
    m_label->setText(m_instanceName); // Display instance name, not module name
    m_label->setHasConnectionPoint(false);

    // Set initial properties
    setAllowMouseResize(true);
    setAllowMouseRotate(true);
    setConnectorsMovable(true);
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
    // Note: Don't update label here, label shows instance name
    update();
}

QString SocModuleItem::instanceName() const
{
    return m_instanceName;
}

void SocModuleItem::setInstanceName(const QString &name)
{
    m_instanceName = name;
    if (m_label) {
        m_label->setText(name);
        m_label->setVisible(true);
        m_label->update();
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

    // Copy instance name (important for maintaining unique names during copy/paste)
    copy->setInstanceName(m_instanceName);

    copy->setPos(pos());
    copy->setRotation(rotation());
    copy->setSize(size());

    return copy;
}

gpds::container SocModuleItem::to_container() const
{
    // Root container
    gpds::container root;
    addItemTypeIdToContainer(root);

    // Save base Node data
    root.add_value("node", QSchematic::Items::Node::to_container());

    // Save module-specific data
    root.add_value("module_name", m_moduleName.toStdString());
    root.add_value("instance_name", m_instanceName.toStdString());

    // Save YAML data as string
    YAML::Emitter emitter;
    emitter << m_moduleYaml;
    root.add_value("module_yaml", std::string(emitter.c_str()));

    // Save label data
    if (m_label) {
        root.add_value("label", m_label->to_container());
    }

    return root;
}

void SocModuleItem::from_container(const gpds::container &container)
{
    // Load module name first (needed before Node::from_container)
    if (auto nameOpt = container.get_value<std::string>("module_name")) {
        m_moduleName = QString::fromStdString(*nameOpt);
    }

    // Load instance name (if not present, use module name for backward compatibility)
    if (auto instNameOpt = container.get_value<std::string>("instance_name")) {
        m_instanceName = QString::fromStdString(*instNameOpt);
    } else {
        m_instanceName = m_moduleName; // Backward compatibility
    }

    // Load YAML data (needed for ports if they don't exist in container)
    if (auto yamlOpt = container.get_value<std::string>("module_yaml")) {
        try {
            m_moduleYaml = YAML::Load(*yamlOpt);
        } catch (const YAML::Exception &e) {
            qWarning() << "Failed to parse YAML:" << e.what();
        }
    }

    // Load base Node data - this will restore connectors from container
    if (auto nodeContainer = container.get_value<gpds::container *>("node")) {
        QSchematic::Items::Node::from_container(**nodeContainer);
    }

    // Store restored connectors
    const auto restoredConnectors = connectors();
    for (const auto &connector : restoredConnectors) {
        if (auto socConnector = std::dynamic_pointer_cast<ModuleLibrary::SocModuleConnector>(
                connector)) {
            m_ports.append(socConnector);
        }
    }

    // Only create ports if none were restored (backward compatibility)
    if (m_ports.isEmpty() && m_moduleYaml) {
        createPortsFromYaml();
    }

    // Load label data
    if (m_label) {
        if (auto labelContainer = container.get_value<gpds::container *>("label")) {
            m_label->from_container(**labelContainer);
        }
        // Update label text with instance name
        m_label->setText(m_instanceName);
    }
}

void SocModuleItem::paint(QPainter *painter, const QStyleOptionGraphicsItem *option, QWidget *widget)
{
    Q_UNUSED(option)
    Q_UNUSED(widget)

    // Draw the bounding rect if debug mode is enabled
    if (_settings.debug) {
        painter->setPen(Qt::NoPen);
        painter->setBrush(QBrush(Qt::red));
        painter->drawRect(boundingRect());
    }

    QRectF rect = sizeRect();

    // Body pen (Color4DBodyEx: #840000)
    QPen bodyPen;
    bodyPen.setWidthF(1.5);
    bodyPen.setStyle(Qt::SolidLine);
    bodyPen.setColor(QColor(132, 0, 0));

    // Body brush (Color4DBodyBgEx: #FFFFC2)
    QBrush bodyBrush;
    bodyBrush.setStyle(Qt::SolidPattern);
    bodyBrush.setColor(QColor(255, 255, 194));

    // Draw the component body (sharp rectangle, no rounded corners)
    painter->setPen(bodyPen);
    painter->setBrush(bodyBrush);
    painter->drawRect(rect);

    // Draw module name (Color4DReferenceEx: #008484)
    painter->setPen(QPen(QColor(0, 132, 132)));
    QFont font = painter->font();
    font.setBold(true);
    font.setPointSize(10);
    painter->setFont(font);

    QRectF textRect(0, 5, rect.width(), LABEL_HEIGHT);
    painter->drawText(textRect, Qt::AlignCenter, m_moduleName);

    // Draw separator line (Color4DGridEx: #848484)
    QPen separatorPen(QColor(132, 132, 132), 1.0);
    painter->setPen(separatorPen);
    painter->drawLine(10, LABEL_HEIGHT + 5, rect.width() - 10, LABEL_HEIGHT + 5);

    // Resize handles
    if (isSelected() && allowMouseResize()) {
        paintResizeHandles(*painter);
    }

    // Rotate handle
    if (isSelected() && allowMouseRotate()) {
        paintRotateHandle(*painter);
    }
}

void SocModuleItem::createPortsFromYaml()
{
    if (!m_moduleYaml) {
        return;
    }

    // First, collect all ports that are mapped by buses
    QSet<QString> mappedPorts;
    if (m_moduleYaml["bus"]) {
        const YAML::Node &buses = m_moduleYaml["bus"];
        for (const auto &busPair : buses) {
            const YAML::Node &busData = busPair.second;
            if (busData["mapping"]) {
                const YAML::Node &mapping = busData["mapping"];
                for (const auto &mapPair : mapping) {
                    const std::string mappedPortName = mapPair.second.as<std::string>();
                    if (!mappedPortName.empty()) {
                        mappedPorts.insert(QString::fromStdString(mappedPortName));
                    }
                }
            }
        }
    }

    QStringList inputPorts;
    QStringList outputPorts;
    QStringList inoutPorts;
    QStringList busPorts;

    // Process regular ports
    if (m_moduleYaml["port"]) {
        const YAML::Node &ports = m_moduleYaml["port"];

        for (const auto &portPair : ports) {
            const std::string portName = portPair.first.as<std::string>();
            const YAML::Node &portData = portPair.second;

            if (portData["direction"]) {
                const std::string direction  = portData["direction"].as<std::string>();
                const QString     portNameQt = QString::fromStdString(portName);

                // Check if port should be visible
                bool isVisible = true;
                if (mappedPorts.contains(portNameQt)) {
                    // Port is mapped by a bus, check if it has explicit visible: true
                    if (portData["visible"]) {
                        isVisible = portData["visible"].as<bool>();
                    } else {
                        // No visible attribute, default to hidden for mapped ports
                        isVisible = false;
                    }
                }

                // Only add port if visible
                if (isVisible) {
                    if (direction == "in" || direction == "input") {
                        inputPorts.append(portNameQt);
                    } else if (direction == "out" || direction == "output") {
                        outputPorts.append(portNameQt);
                    } else if (direction == "inout") {
                        inoutPorts.append(portNameQt);
                    }
                }
            }
        }
    }

    // Process bus ports
    if (m_moduleYaml["bus"]) {
        const YAML::Node &buses = m_moduleYaml["bus"];

        for (const auto &busPair : buses) {
            const std::string busName   = busPair.first.as<std::string>();
            const QString     busNameQt = QString::fromStdString(busName);
            busPorts.append(busNameQt);
        }
    }

    // Calculate required width based on port label lengths and module name
    QFont        labelFont;
    QFontMetrics fm(labelFont);

    // Calculate module name width (with bold font used in paint())
    QFont boldFont;
    boldFont.setBold(true);
    boldFont.setPointSize(10);
    QFontMetrics fmBold(boldFont);
    const qreal  moduleNameWidth = fmBold.horizontalAdvance(m_moduleName);

    // Find longest text on left side
    qreal maxLeftWidth = 0;
    for (const QString &portName : inputPorts) {
        maxLeftWidth = qMax(maxLeftWidth, static_cast<qreal>(fm.horizontalAdvance(portName)));
    }
    for (const QString &portName : busPorts) {
        maxLeftWidth = qMax(maxLeftWidth, static_cast<qreal>(fm.horizontalAdvance(portName)));
    }

    // Find longest text on right side
    qreal maxRightWidth = 0;
    for (const QString &portName : outputPorts) {
        maxRightWidth = qMax(maxRightWidth, static_cast<qreal>(fm.horizontalAdvance(portName)));
    }
    for (const QString &portName : inoutPorts) {
        maxRightWidth = qMax(maxRightWidth, static_cast<qreal>(fm.horizontalAdvance(portName)));
    }

    // Calculate required width based on ports
    // Left padding (15) + connector size (25) + left text + center gap (20) + right text + connector size (25) + right padding (15)
    const qreal connectorSpace = 25; // Space for connector visual
    const qreal centerGap      = 20; // Minimum gap between left and right text
    const qreal sidePadding    = 15; // Padding on each side
    const qreal portBasedWidth = sidePadding + connectorSpace + maxLeftWidth + centerGap
                                 + maxRightWidth + connectorSpace + sidePadding;

    // Calculate required width based on module name (with padding)
    const qreal nameBasedWidth = moduleNameWidth + 40; // Add 40px padding for module name

    // Use the maximum of all width requirements
    const qreal calculatedWidth = qMax(portBasedWidth, nameBasedWidth);

    // Calculate required size
    const int   leftSidePorts  = inputPorts.size() + busPorts.size();
    const int   rightSidePorts = outputPorts.size() + inoutPorts.size();
    const int   maxPorts       = qMax(leftSidePorts, rightSidePorts);
    const qreal requiredHeight = qMax(MIN_HEIGHT, LABEL_HEIGHT + 30 + maxPorts * PORT_SPACING);
    const qreal requiredWidth  = qMax(MIN_WIDTH, calculatedWidth);

    setSize(requiredWidth, requiredHeight);

    // Get grid size for proper positioning
    const int gridSize = _settings.gridSize > 0 ? _settings.gridSize : 20;

    // Create input ports (left side)
    int leftPortIndex = 0;
    for (int i = 0; i < inputPorts.size(); ++i) {
        const qreal yPos = LABEL_HEIGHT + 20 + leftPortIndex * PORT_SPACING;
        QPoint      gridPos(
            0,                                  // Left edge
            static_cast<int>(yPos / gridSize)); // Convert to grid coordinates
        auto connector = std::make_shared<SocModuleConnector>(
            gridPos, inputPorts[i], SocModuleConnector::Input, SocModuleConnector::Left, this);
        addConnector(connector);
        m_ports.append(connector);
        leftPortIndex++;
    }

    // Create bus ports (left side, after input ports)
    for (int i = 0; i < busPorts.size(); ++i) {
        const qreal yPos = LABEL_HEIGHT + 20 + leftPortIndex * PORT_SPACING;
        QPoint      gridPos(
            0,                                  // Left edge
            static_cast<int>(yPos / gridSize)); // Convert to grid coordinates
        auto connector = std::make_shared<SocModuleConnector>(
            gridPos, busPorts[i], SocModuleConnector::Bus, SocModuleConnector::Left, this);
        addConnector(connector);
        m_ports.append(connector);
        leftPortIndex++;
    }

    // Create output ports (right side)
    int rightPortIndex = 0;
    for (int i = 0; i < outputPorts.size(); ++i) {
        const qreal yPos = LABEL_HEIGHT + 20 + rightPortIndex * PORT_SPACING;
        QPoint      gridPos(
            static_cast<int>(requiredWidth / gridSize), // Right edge
            static_cast<int>(yPos / gridSize));         // Convert to grid coordinates
        auto connector = std::make_shared<SocModuleConnector>(
            gridPos, outputPorts[i], SocModuleConnector::Output, SocModuleConnector::Right, this);
        addConnector(connector);
        m_ports.append(connector);
        rightPortIndex++;
    }

    // Create inout ports (right side, after output ports)
    for (int i = 0; i < inoutPorts.size(); ++i) {
        const qreal yPos = LABEL_HEIGHT + 20 + rightPortIndex * PORT_SPACING;
        QPoint      gridPos(
            static_cast<int>(requiredWidth / gridSize), // Right edge
            static_cast<int>(yPos / gridSize));         // Convert to grid coordinates
        auto connector = std::make_shared<SocModuleConnector>(
            gridPos, inoutPorts[i], SocModuleConnector::InOut, SocModuleConnector::Right, this);
        addConnector(connector);
        m_ports.append(connector);
        rightPortIndex++;
    }

    updateLabelPosition();
}

QSizeF SocModuleItem::calculateRequiredSize() const
{
    if (!m_moduleYaml) {
        return QSizeF(MIN_WIDTH, MIN_HEIGHT);
    }

    int inputCount  = 0;
    int outputCount = 0;
    int inoutCount  = 0;
    int busCount    = 0;

    // Count regular ports
    if (m_moduleYaml["port"]) {
        const YAML::Node &ports = m_moduleYaml["port"];

        for (const auto &portPair : ports) {
            const YAML::Node &portData = portPair.second;

            if (portData["direction"]) {
                const std::string direction = portData["direction"].as<std::string>();

                if (direction == "in" || direction == "input") {
                    inputCount++;
                } else if (direction == "out" || direction == "output") {
                    outputCount++;
                } else if (direction == "inout") {
                    inoutCount++;
                }
            }
        }
    }

    // Count bus ports
    if (m_moduleYaml["bus"]) {
        const YAML::Node &buses = m_moduleYaml["bus"];
        busCount                = buses.size();
    }

    const int   leftSidePorts  = inputCount + busCount;
    const int   rightSidePorts = outputCount + inoutCount;
    const int   maxPorts       = qMax(leftSidePorts, rightSidePorts);
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
        // Position label above the module box to avoid overlap with the border
        // Using negative Y to place it above the module
        m_label->setPos(rect.center().x() - m_label->boundingRect().width() / 2, -15);
    }
}
