// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2023-2025 Huang Rui <vowstar@gmail.com>

#include "prcprimitiveitem.h"

#include <qschematic/items/connector.hpp>

#include <QPainter>
#include <QStyleOptionGraphicsItem>

using namespace PrcLibrary;

/* Color Definitions */
namespace {

/* Clock domain colors */
const QColor CLK_CTRL_BORDER = QColor(70, 130, 180);  /* Steel blue */
const QColor CLK_INPUT_BG    = QColor(200, 230, 255); /* Light blue */
const QColor CLK_TARGET_BG   = QColor(200, 255, 200); /* Light green */

/* Reset domain colors */
const QColor RST_CTRL_BORDER = QColor(180, 70, 70);   /* Dark red */
const QColor RST_SOURCE_BG   = QColor(255, 200, 200); /* Light red */
const QColor RST_TARGET_BG   = QColor(255, 220, 180); /* Light orange */

/* Power domain colors */
const QColor PWR_CTRL_BORDER = QColor(70, 130, 70);   /* Dark green */
const QColor PWR_DOMAIN_BG   = QColor(200, 255, 200); /* Light green */

} // namespace

/* Constructor */
PrcPrimitiveItem::PrcPrimitiveItem(
    PrimitiveType primitiveType, const QString &name, QGraphicsItem *parent)
    : QSchematic::Items::Node(Type, parent)
    , m_primitiveType(primitiveType)
    , m_primitiveName(name.isEmpty() ? primitiveTypeName() : name)
{
    /* Initialize type-specific default parameters */
    switch (primitiveType) {
    case ClockInput:
        m_params = ClockInputParams{};
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

    setSize(ITEM_WIDTH, ITEM_HEIGHT);

    setAllowMouseResize(true);
    setAllowMouseRotate(false);
    setConnectorsMovable(true);
    setConnectorsSnapPolicy(QSchematic::Items::Connector::NodeSizerectOutline);
    setConnectorsSnapToGrid(true);

    m_label = std::make_shared<QSchematic::Items::Label>(QSchematic::Items::Item::LabelType, this);
    m_label->setText(m_primitiveName);
    updateLabelPosition();

    /* Connect size change signal to update label position */
    connect(this, &QSchematic::Items::Node::sizeChanged, this, &PrcPrimitiveItem::updateLabelPosition);

    createConnectors();
}

/* Type Queries */
PrimitiveType PrcPrimitiveItem::primitiveType() const
{
    return m_primitiveType;
}

bool PrcPrimitiveItem::isController() const
{
    return false;
}

QString PrcPrimitiveItem::primitiveTypeName() const
{
    switch (m_primitiveType) {
    case ClockInput:
        return "Clock Input";
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

/* Name Accessors */
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

/* Parameter Accessors */
const PrcParams &PrcPrimitiveItem::params() const
{
    return m_params;
}

PrcParams &PrcPrimitiveItem::params()
{
    return m_params;
}

void PrcPrimitiveItem::setParams(const PrcParams &params)
{
    m_params = params;
    update();
}

bool PrcPrimitiveItem::needsConfiguration() const
{
    return m_needsConfiguration;
}

void PrcPrimitiveItem::setNeedsConfiguration(bool needs)
{
    m_needsConfiguration = needs;
}

/* Deep Copy */
std::shared_ptr<QSchematic::Items::Item> PrcPrimitiveItem::deepCopy() const
{
    auto copy = std::make_shared<PrcPrimitiveItem>(m_primitiveType, m_primitiveName);
    copy->setParams(m_params);
    copy->setPos(pos());
    copy->setRotation(rotation());
    copy->setNeedsConfiguration(m_needsConfiguration);
    return copy;
}

/* Serialization Helpers */
namespace {

void serializeSTAGuide(gpds::container &c, const STAGuideParams &p, const std::string &prefix)
{
    c.add_value(prefix + "_configured", p.configured);
    if (p.configured) {
        c.add_value(prefix + "_cell", p.cell.toStdString());
        c.add_value(prefix + "_in", p.in.toStdString());
        c.add_value(prefix + "_out", p.out.toStdString());
        c.add_value(prefix + "_instance", p.instance.toStdString());
    }
}

void deserializeSTAGuide(const gpds::container &c, STAGuideParams &p, const std::string &prefix)
{
    p.configured = c.get_value<bool>(prefix + "_configured").value_or(false);
    if (p.configured) {
        p.cell = QString::fromStdString(c.get_value<std::string>(prefix + "_cell").value_or(""));
        p.in   = QString::fromStdString(c.get_value<std::string>(prefix + "_in").value_or(""));
        p.out  = QString::fromStdString(c.get_value<std::string>(prefix + "_out").value_or(""));
        p.instance = QString::fromStdString(
            c.get_value<std::string>(prefix + "_instance").value_or(""));
    }
}

void serializeICG(gpds::container &c, const ICGParams &p, const std::string &prefix)
{
    c.add_value(prefix + "_configured", p.configured);
    if (p.configured) {
        c.add_value(prefix + "_enable", p.enable.toStdString());
        c.add_value(prefix + "_polarity", p.polarity.toStdString());
        c.add_value(prefix + "_test_enable", p.test_enable.toStdString());
        c.add_value(prefix + "_reset", p.reset.toStdString());
        c.add_value(prefix + "_clock_on_reset", p.clock_on_reset);
        serializeSTAGuide(c, p.sta_guide, prefix + "_sta");
    }
}

void deserializeICG(const gpds::container &c, ICGParams &p, const std::string &prefix)
{
    p.configured = c.get_value<bool>(prefix + "_configured").value_or(false);
    if (p.configured) {
        p.enable = QString::fromStdString(c.get_value<std::string>(prefix + "_enable").value_or(""));
        p.polarity = QString::fromStdString(
            c.get_value<std::string>(prefix + "_polarity").value_or(""));
        p.test_enable = QString::fromStdString(
            c.get_value<std::string>(prefix + "_test_enable").value_or(""));
        p.reset = QString::fromStdString(c.get_value<std::string>(prefix + "_reset").value_or(""));
        p.clock_on_reset = c.get_value<bool>(prefix + "_clock_on_reset").value_or(false);
        deserializeSTAGuide(c, p.sta_guide, prefix + "_sta");
    }
}

void serializeDIV(gpds::container &c, const DIVParams &p, const std::string &prefix)
{
    c.add_value(prefix + "_configured", p.configured);
    if (p.configured) {
        c.add_value(prefix + "_default", p.default_value);
        c.add_value(prefix + "_value", p.value.toStdString());
        c.add_value(prefix + "_width", p.width);
        c.add_value(prefix + "_reset", p.reset.toStdString());
        c.add_value(prefix + "_clock_on_reset", p.clock_on_reset);
        serializeSTAGuide(c, p.sta_guide, prefix + "_sta");
    }
}

void deserializeDIV(const gpds::container &c, DIVParams &p, const std::string &prefix)
{
    p.configured = c.get_value<bool>(prefix + "_configured").value_or(false);
    if (p.configured) {
        p.default_value = c.get_value<int>(prefix + "_default").value_or(1);
        p.value = QString::fromStdString(c.get_value<std::string>(prefix + "_value").value_or(""));
        p.width = c.get_value<int>(prefix + "_width").value_or(0);
        p.reset = QString::fromStdString(c.get_value<std::string>(prefix + "_reset").value_or(""));
        p.clock_on_reset = c.get_value<bool>(prefix + "_clock_on_reset").value_or(false);
        deserializeSTAGuide(c, p.sta_guide, prefix + "_sta");
    }
}

void serializeMUX(gpds::container &c, const MUXParams &p, const std::string &prefix)
{
    c.add_value(prefix + "_configured", p.configured);
    if (p.configured) {
        serializeSTAGuide(c, p.sta_guide, prefix + "_sta");
    }
}

void deserializeMUX(const gpds::container &c, MUXParams &p, const std::string &prefix)
{
    p.configured = c.get_value<bool>(prefix + "_configured").value_or(false);
    if (p.configured) {
        deserializeSTAGuide(c, p.sta_guide, prefix + "_sta");
    }
}

void serializeINV(gpds::container &c, const INVParams &p, const std::string &prefix)
{
    c.add_value(prefix + "_configured", p.configured);
    if (p.configured) {
        serializeSTAGuide(c, p.sta_guide, prefix + "_sta");
    }
}

void deserializeINV(const gpds::container &c, INVParams &p, const std::string &prefix)
{
    p.configured = c.get_value<bool>(prefix + "_configured").value_or(false);
    if (p.configured) {
        deserializeSTAGuide(c, p.sta_guide, prefix + "_sta");
    }
}

void serializeResetSync(gpds::container &c, const ResetSyncParams &p, const std::string &prefix)
{
    c.add_value(prefix + "_async_configured", p.async_configured);
    if (p.async_configured) {
        c.add_value(prefix + "_async_clock", p.async_clock.toStdString());
        c.add_value(prefix + "_async_stage", p.async_stage);
    }
    c.add_value(prefix + "_sync_configured", p.sync_configured);
    if (p.sync_configured) {
        c.add_value(prefix + "_sync_clock", p.sync_clock.toStdString());
        c.add_value(prefix + "_sync_stage", p.sync_stage);
    }
    c.add_value(prefix + "_count_configured", p.count_configured);
    if (p.count_configured) {
        c.add_value(prefix + "_count_clock", p.count_clock.toStdString());
        c.add_value(prefix + "_count_value", p.count_value);
    }
}

void deserializeResetSync(const gpds::container &c, ResetSyncParams &p, const std::string &prefix)
{
    p.async_configured = c.get_value<bool>(prefix + "_async_configured").value_or(false);
    if (p.async_configured) {
        p.async_clock = QString::fromStdString(
            c.get_value<std::string>(prefix + "_async_clock").value_or(""));
        p.async_stage = c.get_value<int>(prefix + "_async_stage").value_or(4);
    }
    p.sync_configured = c.get_value<bool>(prefix + "_sync_configured").value_or(false);
    if (p.sync_configured) {
        p.sync_clock = QString::fromStdString(
            c.get_value<std::string>(prefix + "_sync_clock").value_or(""));
        p.sync_stage = c.get_value<int>(prefix + "_sync_stage").value_or(2);
    }
    p.count_configured = c.get_value<bool>(prefix + "_count_configured").value_or(false);
    if (p.count_configured) {
        p.count_clock = QString::fromStdString(
            c.get_value<std::string>(prefix + "_count_clock").value_or(""));
        p.count_value = c.get_value<int>(prefix + "_count_value").value_or(16);
    }
}

} // namespace

/* Serialization */
gpds::container PrcPrimitiveItem::to_container() const
{
    /* Root container with our type id */
    gpds::container root;
    addItemTypeIdToContainer(root);

    /* Save base Node data as nested container */
    root.add_value("node", QSchematic::Items::Node::to_container());

    /* Save primitive-specific data */
    gpds::container c;
    c.add_value("primitive_type", static_cast<int>(m_primitiveType));
    c.add_value("primitive_name", m_primitiveName.toStdString());

    /* Serialize type-specific parameters */
    std::visit(
        [&c](auto &&params) {
            using T = std::decay_t<decltype(params)>;

            if constexpr (std::is_same_v<T, ClockInputParams>) {
                c.add_value("input_name", params.name.toStdString());
                c.add_value("input_freq", params.freq.toStdString());
                c.add_value("input_controller", params.controller.toStdString());
            } else if constexpr (std::is_same_v<T, ClockTargetParams>) {
                c.add_value("target_name", params.name.toStdString());
                c.add_value("target_freq", params.freq.toStdString());
                c.add_value("target_controller", params.controller.toStdString());
                serializeMUX(c, params.mux, "target_mux");
                serializeICG(c, params.icg, "target_icg");
                serializeDIV(c, params.div, "target_div");
                serializeINV(c, params.inv, "target_inv");
                c.add_value("target_select", params.select.toStdString());
                c.add_value("target_reset", params.reset.toStdString());
                c.add_value("target_test_clock", params.test_clock.toStdString());
            } else if constexpr (std::is_same_v<T, ResetSourceParams>) {
                c.add_value("rst_src_name", params.name.toStdString());
                c.add_value("rst_src_active", params.active.toStdString());
                c.add_value("rst_src_controller", params.controller.toStdString());
            } else if constexpr (std::is_same_v<T, ResetTargetParams>) {
                c.add_value("rst_tgt_name", params.name.toStdString());
                c.add_value("rst_tgt_active", params.active.toStdString());
                c.add_value("rst_tgt_controller", params.controller.toStdString());
                serializeResetSync(c, params.sync, "rst_tgt_sync");
            } else if constexpr (std::is_same_v<T, PowerDomainParams>) {
                c.add_value("pwr_dom_name", params.name.toStdString());
                c.add_value("pwr_dom_controller", params.controller.toStdString());
                c.add_value("pwr_dom_v_mv", params.v_mv);
                c.add_value("pwr_dom_pgood", params.pgood.toStdString());
                c.add_value("pwr_dom_wait_dep", params.wait_dep);
                c.add_value("pwr_dom_settle_on", params.settle_on);
                c.add_value("pwr_dom_settle_off", params.settle_off);
                /* Serialize dependencies */
                c.add_value("pwr_dom_depend_count", static_cast<int>(params.depend.size()));
                for (int i = 0; i < params.depend.size(); ++i) {
                    c.add_value(
                        "pwr_dom_dep_" + std::to_string(i) + "_name",
                        params.depend[i].name.toStdString());
                    c.add_value(
                        "pwr_dom_dep_" + std::to_string(i) + "_type",
                        params.depend[i].type.toStdString());
                }
                /* Serialize follow entries */
                c.add_value("pwr_dom_follow_count", static_cast<int>(params.follow.size()));
                for (int i = 0; i < params.follow.size(); ++i) {
                    c.add_value(
                        "pwr_dom_fol_" + std::to_string(i) + "_clock",
                        params.follow[i].clock.toStdString());
                    c.add_value(
                        "pwr_dom_fol_" + std::to_string(i) + "_reset",
                        params.follow[i].reset.toStdString());
                    c.add_value("pwr_dom_fol_" + std::to_string(i) + "_stage", params.follow[i].stage);
                }
            }
        },
        m_params);

    /* Add primitive data to root */
    root.add_value("primitive", c);

    return root;
}

void PrcPrimitiveItem::from_container(const gpds::container &container)
{
    /* Clear connectors created by constructor before loading */
    for (const auto &conn : m_connectors) {
        removeConnector(conn);
    }
    m_connectors.clear();

    /* Load base Node data from nested container */
    if (auto nodeContainer = container.get_value<gpds::container *>("node")) {
        QSchematic::Items::Node::from_container(**nodeContainer);
    }

    /* Get primitive data container */
    const gpds::container *primContainer = &container;
    if (auto pc = container.get_value<gpds::container *>("primitive")) {
        primContainer = *pc;
    }

    m_primitiveType = static_cast<PrimitiveType>(
        primContainer->get_value<int>("primitive_type").value_or(0));
    m_primitiveName = QString::fromStdString(
        primContainer->get_value<std::string>("primitive_name").value_or(""));

    /* Deserialize type-specific parameters */
    switch (m_primitiveType) {
    case ClockInput: {
        ClockInputParams params;
        params.name = QString::fromStdString(
            primContainer->get_value<std::string>("input_name").value_or(""));
        params.freq = QString::fromStdString(
            primContainer->get_value<std::string>("input_freq").value_or(""));
        params.controller = QString::fromStdString(
            primContainer->get_value<std::string>("input_controller").value_or(""));
        m_params = params;
        break;
    }
    case ClockTarget: {
        ClockTargetParams params;
        params.name = QString::fromStdString(
            primContainer->get_value<std::string>("target_name").value_or(""));
        params.freq = QString::fromStdString(
            primContainer->get_value<std::string>("target_freq").value_or(""));
        params.controller = QString::fromStdString(
            primContainer->get_value<std::string>("target_controller").value_or(""));
        deserializeMUX(*primContainer, params.mux, "target_mux");
        deserializeICG(*primContainer, params.icg, "target_icg");
        deserializeDIV(*primContainer, params.div, "target_div");
        deserializeINV(*primContainer, params.inv, "target_inv");
        params.select = QString::fromStdString(
            primContainer->get_value<std::string>("target_select").value_or(""));
        params.reset = QString::fromStdString(
            primContainer->get_value<std::string>("target_reset").value_or(""));
        params.test_clock = QString::fromStdString(
            primContainer->get_value<std::string>("target_test_clock").value_or(""));
        m_params = params;
        break;
    }
    case ResetSource: {
        ResetSourceParams params;
        params.name = QString::fromStdString(
            primContainer->get_value<std::string>("rst_src_name").value_or(""));
        params.active = QString::fromStdString(
            primContainer->get_value<std::string>("rst_src_active").value_or("low"));
        params.controller = QString::fromStdString(
            primContainer->get_value<std::string>("rst_src_controller").value_or(""));
        m_params = params;
        break;
    }
    case ResetTarget: {
        ResetTargetParams params;
        params.name = QString::fromStdString(
            primContainer->get_value<std::string>("rst_tgt_name").value_or(""));
        params.active = QString::fromStdString(
            primContainer->get_value<std::string>("rst_tgt_active").value_or("low"));
        params.controller = QString::fromStdString(
            primContainer->get_value<std::string>("rst_tgt_controller").value_or(""));
        deserializeResetSync(*primContainer, params.sync, "rst_tgt_sync");
        m_params = params;
        break;
    }
    case PowerDomain: {
        PowerDomainParams params;
        params.name = QString::fromStdString(
            primContainer->get_value<std::string>("pwr_dom_name").value_or(""));
        params.controller = QString::fromStdString(
            primContainer->get_value<std::string>("pwr_dom_controller").value_or(""));
        params.v_mv  = primContainer->get_value<int>("pwr_dom_v_mv").value_or(900);
        params.pgood = QString::fromStdString(
            primContainer->get_value<std::string>("pwr_dom_pgood").value_or(""));
        params.wait_dep   = primContainer->get_value<int>("pwr_dom_wait_dep").value_or(0);
        params.settle_on  = primContainer->get_value<int>("pwr_dom_settle_on").value_or(0);
        params.settle_off = primContainer->get_value<int>("pwr_dom_settle_off").value_or(0);
        /* Deserialize dependencies */
        int depCount = primContainer->get_value<int>("pwr_dom_depend_count").value_or(0);
        for (int i = 0; i < depCount; ++i) {
            PowerDependency dep;
            dep.name = QString::fromStdString(
                primContainer->get_value<std::string>("pwr_dom_dep_" + std::to_string(i) + "_name")
                    .value_or(""));
            dep.type = QString::fromStdString(
                primContainer->get_value<std::string>("pwr_dom_dep_" + std::to_string(i) + "_type")
                    .value_or("hard"));
            params.depend.append(dep);
        }
        /* Deserialize follow entries */
        int folCount = primContainer->get_value<int>("pwr_dom_follow_count").value_or(0);
        for (int i = 0; i < folCount; ++i) {
            PowerFollow fol;
            fol.clock = QString::fromStdString(
                primContainer->get_value<std::string>("pwr_dom_fol_" + std::to_string(i) + "_clock")
                    .value_or(""));
            fol.reset = QString::fromStdString(
                primContainer->get_value<std::string>("pwr_dom_fol_" + std::to_string(i) + "_reset")
                    .value_or(""));
            fol.stage = primContainer->get_value<int>("pwr_dom_fol_" + std::to_string(i) + "_stage")
                            .value_or(4);
            params.follow.append(fol);
        }
        m_params = params;
        break;
    }
    }

    /* Store restored connectors */
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

/* Painting */
QColor PrcPrimitiveItem::getBackgroundColor() const
{
    switch (m_primitiveType) {
    case ClockInput:
        return CLK_INPUT_BG;
    case ClockTarget:
        return CLK_TARGET_BG;
    case ResetSource:
        return RST_SOURCE_BG;
    case ResetTarget:
        return RST_TARGET_BG;
    case PowerDomain:
        return PWR_DOMAIN_BG;
    default:
        return QColor(255, 255, 194);
    }
}

QColor PrcPrimitiveItem::getBorderColor() const
{
    switch (m_primitiveType) {
    case ClockInput:
    case ClockTarget:
        return CLK_CTRL_BORDER;
    case ResetSource:
    case ResetTarget:
        return RST_CTRL_BORDER;
    case PowerDomain:
        return PWR_CTRL_BORDER;
    default:
        return QColor(132, 0, 0);
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

    QRectF rect = QRectF(0, 0, size().width(), size().height());

    /* Draw body */
    QPen   bodyPen(getBorderColor(), 1.5);
    QBrush bodyBrush(getBackgroundColor());

    painter->setPen(bodyPen);
    painter->setBrush(bodyBrush);
    painter->drawRect(rect);

    /* Draw type name */
    QFont font = painter->font();
    font.setPointSize(8);
    painter->setFont(font);
    painter->setPen(getBorderColor().darker(120));
    painter->drawText(QRectF(0, 5, size().width(), 15), Qt::AlignCenter, primitiveTypeName());

    /* Resize handles when selected */
    if (isSelected() && allowMouseResize()) {
        paintResizeHandles(*painter);
    }
}

/* Connectors */
void PrcPrimitiveItem::createConnectors()
{
    for (auto &connector : m_connectors) {
        removeConnector(connector);
    }
    m_connectors.clear();

    const int gridSize = _settings.gridSize > 0 ? _settings.gridSize : 20;

    switch (m_primitiveType) {
    case ClockInput: {
        /* Output on right edge */
        int    rightEdge = static_cast<int>((ITEM_WIDTH - gridSize * 0.5) / gridSize);
        QPoint gridPos(rightEdge, static_cast<int>((ITEM_HEIGHT / 2) / gridSize));
        auto   output = std::make_shared<PrcConnector>(
            gridPos, "out", PrcConnector::Clock, PrcConnector::Right, this);
        addConnector(output);
        m_connectors.append(output);
        break;
    }

    case ClockTarget: {
        /* Input on left edge */
        QPoint gridPosIn(0, static_cast<int>((ITEM_HEIGHT / 2) / gridSize));
        auto   input = std::make_shared<PrcConnector>(
            gridPosIn, "in", PrcConnector::Clock, PrcConnector::Left, this);
        addConnector(input);
        m_connectors.append(input);

        /* Output on right edge */
        int    rightEdge = static_cast<int>((ITEM_WIDTH - gridSize * 0.5) / gridSize);
        QPoint gridPosOut(rightEdge, static_cast<int>((ITEM_HEIGHT / 2) / gridSize));
        auto   output = std::make_shared<PrcConnector>(
            gridPosOut, "out", PrcConnector::Clock, PrcConnector::Right, this);
        addConnector(output);
        m_connectors.append(output);
        break;
    }

    case ResetSource: {
        /* Output on right edge */
        int    rightEdge = static_cast<int>((ITEM_WIDTH - gridSize * 0.5) / gridSize);
        QPoint gridPos(rightEdge, static_cast<int>((ITEM_HEIGHT / 2) / gridSize));
        auto   output = std::make_shared<PrcConnector>(
            gridPos, "out", PrcConnector::Reset, PrcConnector::Right, this);
        addConnector(output);
        m_connectors.append(output);
        break;
    }

    case ResetTarget: {
        /* Input on left edge */
        QPoint gridPos(0, static_cast<int>((ITEM_HEIGHT / 2) / gridSize));
        auto   input = std::make_shared<PrcConnector>(
            gridPos, "in", PrcConnector::Reset, PrcConnector::Left, this);
        addConnector(input);
        m_connectors.append(input);
        break;
    }

    case PowerDomain: {
        /* Dependency input (left edge, upper) */
        QPoint gridPosDepIn(0, static_cast<int>((ITEM_HEIGHT / 4) / gridSize));
        auto   depIn = std::make_shared<PrcConnector>(
            gridPosDepIn, "dep", PrcConnector::Power, PrcConnector::Left, this);
        addConnector(depIn);
        m_connectors.append(depIn);

        /* Dependency output (right edge, upper) */
        int    rightEdge = static_cast<int>((ITEM_WIDTH - gridSize * 0.5) / gridSize);
        QPoint gridPosDepOut(rightEdge, static_cast<int>((ITEM_HEIGHT / 4) / gridSize));
        auto   depOut = std::make_shared<PrcConnector>(
            gridPosDepOut, "out", PrcConnector::Power, PrcConnector::Right, this);
        addConnector(depOut);
        m_connectors.append(depOut);
        break;
    }
    }
}

void PrcPrimitiveItem::updateLabelPosition()
{
    if (m_label) {
        qreal labelWidth = m_label->boundingRect().width();
        m_label->setPos((size().width() - labelWidth) / 2, size().height() - LABEL_HEIGHT);
    }
}

/* Dynamic Port Management */

int PrcPrimitiveItem::inputPortCount() const
{
    int count = 0;
    for (const auto &conn : m_connectors) {
        QString connText = conn->text();
        if (connText == "in" || connText.startsWith("in_") || connText == "dep") {
            ++count;
        }
    }
    return count;
}

int PrcPrimitiveItem::connectedInputPortCount() const
{
    int count = 0;
    for (const auto &conn : m_connectors) {
        QString connText = conn->text();
        if (connText == "in" || connText.startsWith("in_") || connText == "dep") {
            if (conn->isConnected()) {
                ++count;
            }
        }
    }
    return count;
}

void PrcPrimitiveItem::updateDynamicPorts()
{
    /* Only ClockTarget and ResetTarget support dynamic input ports */
    if (m_primitiveType != ClockTarget && m_primitiveType != ResetTarget) {
        return;
    }

    const int gridSize = _settings.gridSize > 0 ? _settings.gridSize : 20;

    /* Count input ports and connected ports */
    int totalInputs    = inputPortCount();
    int connectedCount = connectedInputPortCount();

    /* Ensure there's always at least one available (unconnected) input port */
    if (connectedCount >= totalInputs && totalInputs > 0) {
        /* All input ports are connected - add a new one */
        int newIndex = totalInputs;

        /* Calculate required height for all ports */
        qreal requiredHeight = ITEM_HEIGHT + newIndex * gridSize;
        if (requiredHeight > size().height()) {
            setSize(size().width(), requiredHeight);
        }

        int    yOffset    = newIndex * gridSize; /* Stacked vertically */
        QPoint gridPosNew = QPoint(0, static_cast<int>((ITEM_HEIGHT / 2 + yOffset) / gridSize));

        QString connName = QString("in_%1").arg(newIndex);

        PrcConnector::PortType portType = (m_primitiveType == ClockTarget) ? PrcConnector::Clock
                                                                           : PrcConnector::Reset;

        auto newInput = std::make_shared<PrcConnector>(
            gridPosNew, connName, portType, PrcConnector::Left, this);
        addConnector(newInput);
        m_connectors.append(newInput);
    }

    /* Remove excess unconnected ports (keep at least one available) */
    if (connectedCount < totalInputs - 1 && totalInputs > 1) {
        /* Find and remove unconnected input ports from the end */
        for (int i = m_connectors.size() - 1; i >= 0; --i) {
            auto   &conn     = m_connectors[i];
            QString connText = conn->text();
            if ((connText == "in" || connText.startsWith("in_")) && !conn->isConnected()) {
                /* Check if this leaves at least one available port */
                int availableAfterRemoval = (totalInputs - connectedCount) - 1;
                if (availableAfterRemoval >= 1) {
                    removeConnector(conn);
                    m_connectors.removeAt(i);
                    --totalInputs;
                } else {
                    break; /* Keep at least one available */
                }
            }
        }
    }

    /* Shrink height if ports were removed, but keep minimum height */
    qreal minRequiredHeight = ITEM_HEIGHT + (totalInputs - 1) * gridSize;
    if (minRequiredHeight < ITEM_HEIGHT) {
        minRequiredHeight = ITEM_HEIGHT;
    }
    if (size().height() > minRequiredHeight + gridSize) {
        setSize(size().width(), minRequiredHeight);
    }

    /* Update label position after size change */
    updateLabelPosition();
    update();
}
