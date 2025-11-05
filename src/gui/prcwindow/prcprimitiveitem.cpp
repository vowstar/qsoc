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
    setSize(WIDTH, HEIGHT);
    setConnectorsMovable(false);
    setConnectorsSnapPolicy(QSchematic::Items::Connector::NodeSizerect);

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

QVariant PrcPrimitiveItem::config(const QString &key, const QVariant &defaultValue) const
{
    return m_config.value(key, defaultValue);
}

void PrcPrimitiveItem::setConfig(const QString &key, const QVariant &value)
{
    m_config[key] = value;
}

QMap<QString, QVariant> PrcPrimitiveItem::configuration() const
{
    return m_config;
}

void PrcPrimitiveItem::setConfiguration(const QMap<QString, QVariant> &config)
{
    m_config = config;
}

std::shared_ptr<QSchematic::Items::Item> PrcPrimitiveItem::deepCopy() const
{
    auto copy = std::make_shared<PrcPrimitiveItem>(m_primitiveType, m_primitiveName);
    copy->setConfiguration(m_config);
    copy->setPos(pos());
    copy->setRotation(rotation());
    return copy;
}

gpds::container PrcPrimitiveItem::to_container() const
{
    gpds::container c = QSchematic::Items::Node::to_container();

    c.add_value("primitive_type", static_cast<int>(m_primitiveType));
    c.add_value("primitive_name", m_primitiveName.toStdString());

    /* Serialize configuration map with "config_" prefix */
    for (auto it = m_config.constBegin(); it != m_config.constEnd(); ++it) {
        std::string key = "config_" + it.key().toStdString();
        c.add_value(key, it.value().toString().toStdString());
    }

    return c;
}

void PrcPrimitiveItem::from_container(const gpds::container &container)
{
    QSchematic::Items::Node::from_container(container);

    m_primitiveType = static_cast<PrimitiveType>(
        container.get_value<int>("primitive_type").value_or(0));
    m_primitiveName = QString::fromStdString(
        container.get_value<std::string>("primitive_name").value_or(""));

    /* Deserialize type-specific configuration */
    m_config.clear();

    QStringList configKeys;
    switch (m_primitiveType) {
    case ClockSource:
        configKeys << "frequency" << "phase";
        break;
    case ClockTarget:
        configKeys << "divider" << "enable_gate";
        break;
    case ResetSource:
        configKeys << "active_level" << "duration";
        break;
    case ResetTarget:
        configKeys << "synchronous" << "stages";
        break;
    case PowerDomain:
        configKeys << "voltage" << "isolation" << "retention";
        break;
    }

    for (const QString &key : configKeys) {
        std::string gpdsKey = "config_" + key.toStdString();
        if (auto value = container.get_value<std::string>(gpdsKey)) {
            m_config[key] = QString::fromStdString(*value);
        }
    }

    if (m_label) {
        m_label->setText(m_primitiveName);
    }

    createConnectors();
}

void PrcPrimitiveItem::paint(
    QPainter *painter, const QStyleOptionGraphicsItem *option, QWidget *widget)
{
    Q_UNUSED(option)
    Q_UNUSED(widget)

    QColor color = getTypeColor();

    QPen pen(Qt::black, 2.0);
    painter->setPen(pen);
    painter->setBrush(QBrush(color));
    painter->drawRect(0, 0, WIDTH, HEIGHT);

    QFont font = painter->font();
    font.setPointSize(8);
    painter->setFont(font);
    painter->setPen(Qt::black);
    painter->drawText(QRectF(0, 5, WIDTH, 15), Qt::AlignCenter, primitiveTypeName());

    if (isSelected()) {
        QPen selectionPen(Qt::blue, 2.0, Qt::DashLine);
        painter->setPen(selectionPen);
        painter->setBrush(Qt::NoBrush);
        painter->drawRect(0, 0, WIDTH, HEIGHT);
    }
}

void PrcPrimitiveItem::createConnectors()
{
    for (auto &connector : m_connectors) {
        removeConnector(connector);
    }
    m_connectors.clear();

    switch (m_primitiveType) {
    case ClockSource: {
        auto output = std::make_shared<QSchematic::Items::Connector>(
            QSchematic::Items::Item::ConnectorType, QPoint(WIDTH, HEIGHT / 2), "out", this);
        addConnector(output);
        m_connectors.append(output);
        break;
    }

    case ClockTarget: {
        auto input = std::make_shared<QSchematic::Items::Connector>(
            QSchematic::Items::Item::ConnectorType, QPoint(0, HEIGHT / 2), "in", this);
        addConnector(input);
        m_connectors.append(input);

        auto output = std::make_shared<QSchematic::Items::Connector>(
            QSchematic::Items::Item::ConnectorType, QPoint(WIDTH, HEIGHT / 2), "out", this);
        addConnector(output);
        m_connectors.append(output);
        break;
    }

    case ResetSource: {
        auto output = std::make_shared<QSchematic::Items::Connector>(
            QSchematic::Items::Item::ConnectorType, QPoint(WIDTH, HEIGHT / 2), "rst", this);
        addConnector(output);
        m_connectors.append(output);
        break;
    }

    case ResetTarget: {
        auto input = std::make_shared<QSchematic::Items::Connector>(
            QSchematic::Items::Item::ConnectorType, QPoint(0, HEIGHT / 2), "rst", this);
        addConnector(input);
        m_connectors.append(input);
        break;
    }

    case PowerDomain: {
        /* Power domain: inputs (enable, clear) + outputs (ready, fault) */
        int y = HEIGHT / 4;

        auto enable = std::make_shared<QSchematic::Items::Connector>(
            QSchematic::Items::Item::ConnectorType, QPoint(0, y), "en", this);
        addConnector(enable);
        m_connectors.append(enable);

        auto clear = std::make_shared<QSchematic::Items::Connector>(
            QSchematic::Items::Item::ConnectorType, QPoint(0, y * 3), "clr", this);
        addConnector(clear);
        m_connectors.append(clear);

        auto ready = std::make_shared<QSchematic::Items::Connector>(
            QSchematic::Items::Item::ConnectorType, QPoint(WIDTH, y), "rdy", this);
        addConnector(ready);
        m_connectors.append(ready);

        auto fault = std::make_shared<QSchematic::Items::Connector>(
            QSchematic::Items::Item::ConnectorType, QPoint(WIDTH, y * 3), "flt", this);
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

QColor PrcPrimitiveItem::getTypeColor() const
{
    switch (m_primitiveType) {
    case ClockSource:
        return QColor(173, 216, 230);
    case ClockTarget:
        return QColor(135, 206, 250);
    case ResetSource:
        return QColor(255, 182, 193);
    case ResetTarget:
        return QColor(255, 160, 160);
    case PowerDomain:
        return QColor(144, 238, 144);
    default:
        return QColor(220, 220, 220);
    }
}
