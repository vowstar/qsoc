// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCCELLBINDING_H
#define QSOCCELLBINDING_H

#include "common/qsoccellsynth.h"
#include "common/qsoccelltable.h"

#include <QList>
#include <QMap>
#include <QString>
#include <QStringList>

#include <yaml-cpp/yaml.h>

class QSocModuleManager;
class QSocProjectManager;

/**
 * @brief Technology cells declared on module library entries, and the cell
 *        roles they implement.
 * @details A library entry declares a clock cell with a `function` truth
 *          table or a `sequential` template, plus optional `tie` constants.
 *          The project key `cell.target` selects generic behavioral roles or
 *          asic roles that instantiate the declared cells. In asic mode a
 *          combinational role no cell implements is composed from the
 *          declared combinational cells by exact synthesis.
 */
class QSocCellBinding
{
public:
    enum class Target { Generic, Asic };

    /** A declared technology cell. */
    struct Cell
    {
        QString                name;
        QSocCellPorts          ports;
        QMap<QString, bool>    tie;   /**< Input pin to its constant */
        QSocCellTable          table; /**< Function of a combinational cell */
        QString                type;  /**< icg_pos, icg_neg or sync; empty when combinational */
        QMap<QString, QString> pin;   /**< Template key such as clock to its cell pin */
        int                    stages = 0;
    };

    /** How a role is implemented in asic mode. */
    struct Binding
    {
        int                    cell = -1; /**< Index in cells(), -1 when composed */
        QMap<QString, QString> pin;       /**< Cell pin to role port */
        QStringList            via;       /**< Role instances of a composed role, in signal order */
        QSocCellSynthNetlist   network;   /**< Declared cells of a synthesized role */
    };

    /**
     * @brief Read the target and every declaration, then bind the roles.
     * @param project The project node, for `cell.target`.
     * @param modules Module name to library entry.
     */
    static QSocCellBinding resolve(const YAML::Node &project, const YAML::Node &modules);

    /** resolve() on the loaded project and module libraries; either may be null. */
    static QSocCellBinding fromProject(QSocProjectManager *project, QSocModuleManager *modules);

    /** Role module names in file list order. */
    static QStringList roleNames();

    /** Port names of a role module. */
    static QStringList rolePorts(const QString &role);

    Target                        target() const { return mode; }
    bool                          isAsic() const { return mode == Target::Asic; }
    bool                          isValid() const { return problems.isEmpty(); }
    const QStringList            &errors() const { return problems; }
    const QStringList            &warnings() const { return notices; }
    const QList<Cell>            &cells() const { return declared; }
    const QMap<QString, Binding> &roles() const { return bound; }

    /** Roles no declared cell implements, in file list order. */
    QStringList unresolved() const;

    /** Asic body of a role module, the text after its port list. */
    QString body(const QString &role) const;

    /** One line saying what implements the role. */
    QString detail(const QString &role) const;

    /** Behavioral model of a declared cell. */
    static QString model(const Cell &cell);

    /**
     * @brief The declared cells synthesis may use, as the solver sees them.
     * @details Combinational cells with one output and 1 to 3 inputs left
     *          after the ties, in cells() order.
     */
    QList<QSocCellSynthCell> basis() const;

    /** Role binding report in YAML. */
    QString report() const;

private:
    Target                 mode   = Target::Generic;
    unsigned               budget = QSocCellSynthRequest::defaultResourceLimit;
    QList<Cell>            declared;
    QMap<QString, Binding> bound;
    QMap<QString, QString> failed; /**< Role to why synthesis left it unresolved */
    QStringList            problems;
    QStringList            notices;

    void    readTarget(const YAML::Node &project);
    void    declare(const QString &name, const YAML::Node &entry);
    bool    readTemplate(Cell *cell, const YAML::Node &node, const QString &path);
    bool    readTies(Cell *cell, const YAML::Node &node, const QString &path);
    void    bind();
    void    synthesize();
    void    compose();
    QString network(const QString &role, const QSocCellSynthNetlist &netlist) const;
    void    claim(const QString &role, const QList<int> &cells, const QList<Binding> &bindings);
};

#endif // QSOCCELLBINDING_H
