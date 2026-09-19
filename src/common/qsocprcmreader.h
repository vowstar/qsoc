// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCPRCMREADER_H
#define QSOCPRCMREADER_H

#include "common/qsocprcminput.h"
#include "common/qsocverilogutils.h"

#include <limits>
#include <QRegularExpression>

namespace QSocPrcmDetail {

struct Context
{
    QString                       file;
    QMap<QString, QSocPrcmSource> source;
    QMap<QString, QSocPrcmSource> origin;

    QSocPrcmSource locate(const QString &path, const YAML::Mark &mark) const
    {
        QString prefix = path;
        while (!prefix.isEmpty() && !origin.contains(prefix)) {
            const auto end = qMax(prefix.lastIndexOf('.'), prefix.lastIndexOf('['));
            prefix         = end < 0 ? QString() : prefix.left(end);
        }
        const auto base = origin.value(prefix, {file, prefix, 0, 0});
        return {
            base.file,
            base.path + path.mid(prefix.size()),
            mark.is_null() ? base.line : mark.line + 1,
            mark.is_null() ? base.column : mark.column + 1};
    }
};

class Reader
{
public:
    Reader(const YAML::Node &value, QString field, Context &state, YAML::Mark fallback = {})
        : node(value)
        , path(std::move(field))
        , context(state)
    {
        const auto mark = node.IsDefined() ? node.Mark() : fallback;
        origin          = context.locate(path, mark);
        context.source.insert(path, origin);
    }

    Reader member(const QString &name) const
    {
        return {
            node[name.toStdString()],
            path.isEmpty() ? name : path + "." + name,
            context,
            node.Mark()};
    }

    Reader item(std::size_t index) const
    {
        return {node[index], path + QString("[%1]").arg(index), context, node.Mark()};
    }

    bool has(const QString &name) const { return node[name.toStdString()].IsDefined(); }

    const YAML::Node     &value() const { return node; }
    const QSocPrcmSource &position() const { return origin; }

    [[noreturn]] void fail(const QString &code, const QString &message) const
    {
        throw QSocPrcmDiagnostic{code, message, {origin}};
    }

    QStringList keys() const
    {
        if (!node.IsMap()) {
            fail("PRCM_TYPE", "Expected a mapping.");
        }
        QMap<QString, QSocPrcmSource> found;
        for (const auto &entry : node) {
            if (!entry.first.IsScalar()) {
                fail("PRCM_TYPE", "Field names must be text.");
            }
            const QString name  = QString::fromStdString(entry.first.Scalar());
            const auto    mark  = entry.first.Mark();
            const auto location = context.locate(path.isEmpty() ? name : path + "." + name, mark);
            if (found.contains(name)) {
                throw QSocPrcmDiagnostic{
                    "PRCM_DUPLICATE",
                    "Field is declared twice: " + name,
                    {found.value(name), location}};
            }
            found.insert(name, location);
        }
        return found.keys();
    }

    void fields(const QStringList &required, const QStringList &optional = {}) const
    {
        const auto actual = keys();
        for (const auto &name : actual) {
            if (!required.contains(name) && !optional.contains(name)) {
                member(name).fail("PRCM_FIELD", "Unknown field: " + name);
            }
        }
        for (const auto &name : required) {
            if (!actual.contains(name)) {
                member(name).fail("PRCM_REQUIRED", "Missing field: " + name);
            }
        }
    }

    QStringList table(bool nonempty = true) const
    {
        const auto result = keys();
        if (nonempty && result.isEmpty()) {
            fail("PRCM_REQUIRED", "Expected at least one entry.");
        }
        for (const auto &name : result) {
            if (!QSocVerilogUtils::isValidVerilogIdentifier(name)) {
                member(name).fail("PRCM_NAME", "Invalid name: " + name);
            }
        }
        return result;
    }

    QString text() const
    {
        if (!node.IsScalar() || node.Scalar().empty()) {
            fail("PRCM_TYPE", "Expected nonempty text.");
        }
        return QString::fromStdString(node.Scalar());
    }

    QString name() const
    {
        const auto result = text();
        if (!QSocVerilogUtils::isValidVerilogIdentifier(result)) {
            fail("PRCM_NAME", "Invalid name: " + result);
        }
        return result;
    }

    quint64 number(quint64 maximum = std::numeric_limits<quint64>::max()) const
    {
        const auto                      value = text();
        static const QRegularExpression integer(QStringLiteral("^(?:0[xX][0-9a-fA-F]+|[0-9]+)$"));
        bool                            ok          = false;
        const bool                      hexadecimal = value.startsWith("0x", Qt::CaseInsensitive);
        const quint64                   result = hexadecimal ? value.mid(2).toULongLong(&ok, 16)
                                                             : value.toULongLong(&ok, 10);
        if (!integer.match(value).hasMatch() || !ok || result > maximum) {
            fail(
                "PRCM_NUMBER",
                "Expected an unsigned decimal or hexadecimal integer within the field range.");
        }
        return result;
    }

    bool choice(const QString &positive, const QString &negative) const
    {
        const auto value = text();
        if (value != positive && value != negative) {
            fail("PRCM_VALUE", QString("Expected %1 or %2.").arg(positive, negative));
        }
        return value == positive;
    }

    std::size_t size() const
    {
        if (!node.IsSequence()) {
            fail("PRCM_TYPE", "Expected a list.");
        }
        return node.size();
    }

    QStringList nameList() const
    {
        QStringList result;
        const auto  count = size();
        if (count == 0) {
            fail("PRCM_REQUIRED", "Expected at least one name.");
        }
        for (std::size_t i = 0; i < count; ++i) {
            const auto value = item(i).name();
            if (result.contains(value)) {
                item(i).fail("PRCM_DUPLICATE", "Name occurs twice: " + value);
            }
            result.append(value);
        }
        return result;
    }

private:
    const YAML::Node node;
    const QString    path;
    Context         &context;
    QSocPrcmSource   origin;
};

} // namespace QSocPrcmDetail

#endif // QSOCPRCMREADER_H
