// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2023-2025 Huang Rui <vowstar@gmail.com>

#include "prcprimitiveitem.h"

#include <qschematic/items/connector.hpp>

#include <QPainter>
#include <QStyleOptionGraphicsItem>

using namespace PrcLibrary;

PrcPrimitiveItem::PrcPrimitiveItem(
    PrimitiveType primitiveType, const QString &name, QGraphicsItem *parent)
    : QSchematic::Items::Node(Type, parent)
    , m_primitiveType(primitiveType)
    , m_primitiveName(name.isEmpty() ? primitiveTypeName() : name)
{
    /* Initialize type-specific default parameters */
    switch (primitiveType) {
    case ClockSource:
        m_params = ClockSourceParams{};
        break;
    case ClockTarget:
        m_params = ClockTargetParams{};
        break;
    case ResetSource:
        m_params = ResetSourceParams{};
        break;
    case ResetTarget:
        m_params = ResetTargetParams{};
        break;
    case PowerDomain:
        m_params = PowerDomainParams{};
        break;
    }

    setSize(WIDTH, HEIGHT);
    setAllowMouseResize(true);
    setAllowMouseRotate(true);
    setConnectorsMovable(true);
    setConnectorsSnapPolicy(QSchematic::Items::Connector::NodeSizerectOutline);
    setConnectorsSnapToGrid(true);

    m_label = std::make_shared<QSchematic::Items::Label>(QSchematic::Items::Item::LabelType, this);
    m_label->setText(m_primitiveName);
    updateLabelPosition();

    createConnectors();
}

PrimitiveType PrcPrimitiveItem::primitiveType() const
{
    return m_primitiveType;
}

QString PrcPrimitiveItem::primitiveTypeName() const
{
    switch (m_primitiveType) {
    case ClockSource:
        return "Clock Source";
    case ClockTarget:
        return "Clock Target";
    case ResetSource:
        return "Reset Source";
    case ResetTarget:
        return "Reset Target";
    case PowerDomain:
        return "Power Domain";
    default:
        return "Unknown";
    }
}

QString PrcPrimitiveItem::primitiveName() const
{
    return m_primitiveName;
}

void PrcPrimitiveItem::setPrimitiveName(const QString &name)
{
    if (m_primitiveName != name) {
        m_primitiveName = name;
        if (m_label) {
            m_label->setText(name);
        }
        update();
    }
}

const PrcParams &PrcPrimitiveItem::params() const
{
    return m_params;
}

void PrcPrimitiveItem::setParams(const PrcParams &params)
{
    m_params = params;
    update();
}

std::shared_ptr<QSchematic::Items::Item> PrcPrimitiveItem::deepCopy() const
{
    auto copy = std::make_shared<PrcPrimitiveItem>(m_primitiveType, m_primitiveName);
    copy->setParams(m_params);
    copy->setPos(pos());
    copy->setRotation(rotation());
    return copy;
}

gpds::container PrcPrimitiveItem::to_container() const
{
    gpds::container c = QSchematic::Items::Node::to_container();

    c.add_value("primitive_type", static_cast<int>(m_primitiveType));
    c.add_value("primitive_name", m_primitiveName.toStdString());

    /* Serialize type-specific parameters */
    std::visit(
        [&c](auto &&params) {
            using T = std::decay_t<decltype(params)>;
            if constexpr (std::is_same_v<T, ClockSourceParams>) {
                c.add_value("param_frequency_mhz", params.frequency_mhz);
                c.add_value("param_phase_deg", params.phase_deg);
            } else if constexpr (std::is_same_v<T, ClockTargetParams>) {
                c.add_value("param_divider", params.divider);
                c.add_value("param_enable_gate", params.enable_gate);
            } else if constexpr (std::is_same_v<T, ResetSourceParams>) {
                c.add_value("param_active_low", params.active_low);
                c.add_value("param_duration_us", params.duration_us);
            } else if constexpr (std::is_same_v<T, ResetTargetParams>) {
                c.add_value("param_synchronous", params.synchronous);
                c.add_value("param_stages", params.stages);
            } else if constexpr (std::is_same_v<T, PowerDomainParams>) {
                c.add_value("param_voltage", params.voltage);
                c.add_value("param_isolation", params.isolation);
                c.add_value("param_retention", params.retention);
            }
        },
        m_params);

    return c;
}

void PrcPrimitiveItem::from_container(const gpds::container &container)
{
    m_primitiveType = static_cast<PrimitiveType>(
        container.get_value<int>("primitive_type").value_or(0));
    m_primitiveName = QString::fromStdString(
        container.get_value<std::string>("primitive_name").value_or(""));

    /* Deserialize type-specific parameters */
    switch (m_primitiveType) {
    case ClockSource: {
        ClockSourceParams params;
        params.frequency_mhz = container.get_value<double>("param_frequency_mhz").value_or(100.0);
        params.phase_deg     = container.get_value<double>("param_phase_deg").value_or(0.0);
        m_params             = params;
        break;
    }
    case ClockTarget: {
        ClockTargetParams params;
        params.divider     = container.get_value<int>("param_divider").value_or(1);
        params.enable_gate = container.get_value<bool>("param_enable_gate").value_or(false);
        m_params           = params;
        break;
    }
    case ResetSource: {
        ResetSourceParams params;
        params.active_low  = container.get_value<bool>("param_active_low").value_or(true);
        params.duration_us = container.get_value<double>("param_duration_us").value_or(10.0);
        m_params           = params;
        break;
    }
    case ResetTarget: {
        ResetTargetParams params;
        params.synchronous = container.get_value<bool>("param_synchronous").value_or(true);
        params.stages      = container.get_value<int>("param_stages").value_or(2);
        m_params           = params;
        break;
    }
    case PowerDomain: {
        PowerDomainParams params;
        params.voltage   = container.get_value<double>("param_voltage").value_or(1.0);
        params.isolation = container.get_value<bool>("param_isolation").value_or(true);
        params.retention = container.get_value<bool>("param_retention").value_or(false);
        m_params         = params;
        break;
    }
    }

    /* Load base Node data - this will restore connectors from container */
    QSchematic::Items::Node::from_container(container);

    /* Store restored connectors (matching schematicwindow pattern) */
    const auto restoredConnectors = connectors();
    for (const auto &connector : restoredConnectors) {
        if (auto prcConnector = std::dynamic_pointer_cast<PrcConnector>(connector)) {
            m_connectors.append(prcConnector);
        }
    }

    /* Only create connectors if none were restored (backward compatibility) */
    if (m_connectors.isEmpty()) {
        createConnectors();
    }

    if (m_label) {
        m_label->setText(m_primitiveName);
    }
}

void PrcPrimitiveItem::paint(
    QPainter *painter, const QStyleOptionGraphicsItem *option, QWidget *widget)
{
    Q_UNUSED(option)
    Q_UNUSED(widget)

    /* Draw the bounding rect if debug mode is enabled */
    if (_settings.debug) {
        painter->setPen(Qt::NoPen);
        painter->setBrush(QBrush(Qt::red));
        painter->drawRect(boundingRect());
    }

    QRectF rect = QRectF(0, 0, WIDTH, HEIGHT);

    /* Unified color scheme matching schematicwindow */
    QPen   bodyPen(QColor(132, 0, 0), 1.5);  // Deep red border
    QBrush bodyBrush(QColor(255, 255, 194)); // Light yellow background

    painter->setPen(bodyPen);
    painter->setBrush(bodyBrush);
    painter->drawRect(rect);

    /* Draw type name with cyan text */
    QFont font = painter->font();
    font.setPointSize(8);
    painter->setFont(font);
    painter->setPen(QColor(0, 132, 132)); // Cyan text
    painter->drawText(QRectF(0, 5, WIDTH, 15), Qt::AlignCenter, primitiveTypeName());

    /* Resize handles (matching schematicwindow) */
    if (isSelected() && allowMouseResize()) {
        paintResizeHandles(*painter);
    }

    /* Rotate handle (matching schematicwindow) */
    if (isSelected() && allowMouseRotate()) {
        paintRotateHandle(*painter);
    }
}

void PrcPrimitiveItem::createConnectors()
{
    for (auto &connector : m_connectors) {
        removeConnector(connector);
    }
    m_connectors.clear();

    /* Get grid size for proper positioning (matching schematicwindow) */
    const int gridSize = _settings.gridSize > 0 ? _settings.gridSize : 20;

    /* Calculate right edge grid position (slightly inside to avoid overflow) */
    const int rightEdgeGrid = static_cast<int>((WIDTH - gridSize * 0.5) / gridSize);

    switch (m_primitiveType) {
    case ClockSource: {
        /* Output on right edge, middle */
        QPoint gridPos(
            rightEdgeGrid,                              // Right edge
            static_cast<int>((HEIGHT / 2) / gridSize)); // Middle
        auto output = std::make_shared<PrcConnector>(
            gridPos, "out", PrcConnector::Clock, PrcConnector::Right, this);
        addConnector(output);
        m_connectors.append(output);
        break;
    }

    case ClockTarget: {
        /* Input on left edge, middle */
        QPoint gridPosIn(
            0,                                          // Left edge
            static_cast<int>((HEIGHT / 2) / gridSize)); // Middle
        auto input = std::make_shared<PrcConnector>(
            gridPosIn, "in", PrcConnector::Clock, PrcConnector::Left, this);
        addConnector(input);
        m_connectors.append(input);

        /* Output on right edge, middle */
        QPoint gridPosOut(
            rightEdgeGrid,                              // Right edge
            static_cast<int>((HEIGHT / 2) / gridSize)); // Middle
        auto output = std::make_shared<PrcConnector>(
            gridPosOut, "out", PrcConnector::Clock, PrcConnector::Right, this);
        addConnector(output);
        m_connectors.append(output);
        break;
    }

    case ResetSource: {
        /* Output on right edge, middle */
        QPoint gridPos(
            rightEdgeGrid,                              // Right edge
            static_cast<int>((HEIGHT / 2) / gridSize)); // Middle
        auto output = std::make_shared<PrcConnector>(
            gridPos, "rst", PrcConnector::Reset, PrcConnector::Right, this);
        addConnector(output);
        m_connectors.append(output);
        break;
    }

    case ResetTarget: {
        /* Input on left edge, middle */
        QPoint gridPos(
            0,                                          // Left edge
            static_cast<int>((HEIGHT / 2) / gridSize)); // Middle
        auto input = std::make_shared<PrcConnector>(
            gridPos, "rst", PrcConnector::Reset, PrcConnector::Left, this);
        addConnector(input);
        m_connectors.append(input);
        break;
    }

    case PowerDomain: {
        /* Power domain: inputs (enable, clear) + outputs (ready, fault) */
        /* Enable input: left edge, upper quarter */
        QPoint gridPosEn(
            0,                                          // Left edge
            static_cast<int>((HEIGHT / 4) / gridSize)); // Upper quarter
        auto enable = std::make_shared<PrcConnector>(
            gridPosEn, "en", PrcConnector::Power, PrcConnector::Left, this);
        addConnector(enable);
        m_connectors.append(enable);

        /* Clear input: left edge, lower quarter */
        QPoint gridPosClr(
            0,                                              // Left edge
            static_cast<int>((HEIGHT * 3 / 4) / gridSize)); // Lower quarter
        auto clear = std::make_shared<PrcConnector>(
            gridPosClr, "clr", PrcConnector::Power, PrcConnector::Left, this);
        addConnector(clear);
        m_connectors.append(clear);

        /* Ready output: right edge, upper quarter */
        QPoint gridPosRdy(
            rightEdgeGrid,                              // Right edge
            static_cast<int>((HEIGHT / 4) / gridSize)); // Upper quarter
        auto ready = std::make_shared<PrcConnector>(
            gridPosRdy, "rdy", PrcConnector::Power, PrcConnector::Right, this);
        addConnector(ready);
        m_connectors.append(ready);

        /* Fault output: right edge, lower quarter */
        QPoint gridPosFlt(
            rightEdgeGrid,                                  // Right edge
            static_cast<int>((HEIGHT * 3 / 4) / gridSize)); // Lower quarter
        auto fault = std::make_shared<PrcConnector>(
            gridPosFlt, "flt", PrcConnector::Power, PrcConnector::Right, this);
        addConnector(fault);
        m_connectors.append(fault);
        break;
    }
    }
}

void PrcPrimitiveItem::updateLabelPosition()
{
    if (m_label) {
        qreal labelWidth = m_label->boundingRect().width();
        m_label->setPos((WIDTH - labelWidth) / 2, HEIGHT - LABEL_HEIGHT);
    }
}
