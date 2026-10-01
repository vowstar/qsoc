// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCCELLTABLE_H
#define QSOCCELLTABLE_H

#include <bitset>
#include <QList>
#include <QMap>
#include <QString>
#include <QStringList>
#include <QtGlobal>

#include <yaml-cpp/yaml.h>

/**
 * @brief One port of a library cell, as the module library declares it.
 */
struct QSocCellPort
{
    QString direction; /**< "in", "out" or "inout" */
    quint32 width = 1;

    QSocCellPort() = default;
    /* Implicit on purpose: a port table reads `{"PAD", "inout"}`. */
    // cppcheck-suppress noExplicitConstructor
    QSocCellPort(const char *dir)
        : direction(QString::fromUtf8(dir))
    {}
    // cppcheck-suppress noExplicitConstructor
    QSocCellPort(const QString &dir, quint32 bits = 1)
        : direction(dir)
        , width(bits)
    {}

    bool operator==(const QSocCellPort &) const = default;
};

/** Port name to its declaration, from the module library. */
using QSocCellPorts = QMap<QString, QSocCellPort>;

/**
 * @brief The truth table of a cell, read from its `function` key.
 * @details A row maps columns to values. A column that names a 1-bit port is
 *          a pin: an input pin takes 0, 1 or x (omitted means x), an output
 *          pin takes 0, 1 or an expression over input pins. Any other column
 *          is an attribute whose value is a one-word label. Boolean outputs
 *          are held as truth tables over inputs(): bit m is the value when
 *          input i is at level (m >> i) & 1.
 */
class QSocCellTable
{
public:
    static constexpr int maxInputs      = 8;
    static constexpr int maxMatchInputs = 4;

    using Truth  = std::bitset<1U << maxInputs>;
    using PinMap = QMap<QString, QString>;

    struct Row
    {
        QMap<QString, bool>    when; /**< Input pin to its level; an absent pin is x. */
        QMap<QString, QString> sets; /**< Output column to "0", "1", expression or label. */
    };

    /** One attribute row over a caller's pin order: bits holds 0, 1 or x per pin. */
    struct Pattern
    {
        QString bits;
        QString label;

        bool operator==(const Pattern &) const = default;
    };

    /** What parse() checks beyond each row on its own. */
    enum class Check {
        Function, /**< The rows must agree and define every output. */
        Rows,     /**< Rows are labelled patterns that may overlap and repeat, no truths. */
    };

    /**
     * @brief Read and check a `function` node against the cell ports.
     * @param path Prefix of every diagnostic, such as `module.NAND2.function`.
     */
    static QSocCellTable parse(
        const YAML::Node    &node,
        const QSocCellPorts &ports,
        const QString       &path  = QStringLiteral("function"),
        Check                check = Check::Function);

    /** Bits of a library port type such as `logic[7:0]` or `logic[1:0][3:0]`. */
    static quint32 portWidth(const QString &type);
    /** Ports of a module library `port` node, directions as in/out/inout. */
    static QSocCellPorts portsOf(const YAML::Node &portNode);

    bool               isValid() const { return diagnostics.isEmpty(); }
    const QStringList &errors() const { return diagnostics; }
    const QStringList &inputs() const { return inputPins; }
    const QStringList &outputs() const { return outputPins; }
    const QStringList &attributes() const { return attributeNames; }
    const QList<Row>  &rows() const { return table; }

    /** Truth table of a Boolean output over inputs(). */
    Truth truth(const QString &output) const { return truths.value(output); }

    /**
     * @brief Outputs and attributes fixed by the given input levels.
     * @details An absent input is x: an output whose value depends on it and
     *          a row that tests it are left out.
     */
    QMap<QString, QString> evaluate(const QMap<QString, bool> &levels) const;

    /** Rows setting an attribute, as positional patterns over pinOrder. */
    QList<Pattern> patterns(
        const QString &attribute, const QStringList &pinOrder, QString *error = nullptr) const;

    /** This function with some input pins held at constants, over the other inputs. */
    QSocCellTable tied(const QMap<QString, bool> &levels) const;

    /** True when some output changes with the input pin. */
    bool dependsOn(const QString &input) const;

    /** Pin renamings, this cell to other, under which both functions agree. */
    QList<PinMap> matches(const QSocCellTable &other) const;

    /** Verilog-2005 behavioral model of a combinational cell, empty on error. */
    QString verilog(const QString &module, QString *error = nullptr) const;

private:
    QSocCellPorts        ports;
    QStringList          inputPins;
    QStringList          outputPins;
    QStringList          attributeNames;
    QList<Row>           table;
    QMap<QString, Truth> functions; /**< Output value text to its truth table. */
    QMap<QString, Truth> truths;    /**< Boolean output to its truth table. */
    QStringList          diagnostics;

    int     span() const { return 1 << inputPins.size(); }
    Truth   all() const;
    Truth   cube(const QMap<QString, bool> &when) const;
    QString describe(int minterm) const;
    void    readRow(const YAML::Node &node, const QString &path);
    bool    compile(const QString &text, QString *error);
    void    check(const QString &path);
};

#endif // QSOCCELLTABLE_H
