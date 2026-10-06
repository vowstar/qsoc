// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "common/qsoclicense.h"

#include <QFile>

const QList<QSocLicense::Component> &QSocLicense::components()
{
    static const QList<Component> list = {
        {"antlr4", "BSD-3-Clause", "https://github.com/antlr/antlr4", {"antlr4.txt"}, {}},
        {"aws-lc",
         "Apache-2.0 AND ISC",
         "https://github.com/aws/aws-lc",
         {"aws-lc.txt", "aws-lc-notice.txt"},
         "New AWS-LC files are Apache-2.0 OR ISC. AWS-LC bundles fiat-crypto "
         "under MIT and the jitterentropy library under BSD-3-Clause."},
        {"bigint", "Public domain", "http://mattmccutchen.net/bigint/", {"bigint.txt"}, {}},
        {"cmark-gfm", "BSD-2-Clause", "https://github.com/github/cmark-gfm", {"cmark-gfm.txt"}, {}},
        {"csv", "BSD-3-Clause", "https://github.com/d99kris/rapidcsv", {"csv.txt"}, {}},
        {"fmt", "MIT", "https://github.com/fmtlib/fmt", {"fmt.txt"}, {}},
        {"gpds", "MIT", "https://github.com/simulton/gpds", {"gpds.txt"}, {}},
        {"icu", "ICU", "https://icu.unicode.org", {"icu.txt"}, {}},
        {"inja", "MIT", "https://github.com/pantor/inja", {"inja.txt"}, {}},
        {"json", "MIT", "https://github.com/nlohmann/json", {"json.txt"}, {}},
        {"lexbor",
         "Apache-2.0",
         "https://github.com/lexbor/lexbor",
         {"lexbor.txt", "lexbor-notice.txt"},
         {}},
        {"libssh2", "BSD-3-Clause", "https://github.com/libssh2/libssh2", {"libssh2.txt"}, {}},
        {"linux-runtime",
         "Various, see text",
         "https://almalinux.org",
         {"linux-runtime.txt",
          "lgpl-2.1.txt",
          "gpl-2.0.txt",
          "gcc-exception-3.1.txt",
          "openssl-1.1.1.txt"},
         {}},
        {"material-icons",
         "Apache-2.0",
         "https://github.com/google/material-design-icons",
         {"material-icons.txt", "apache-2.0.txt"},
         {}},
        {"mbedtls",
         "Apache-2.0",
         "https://github.com/Mbed-TLS/mbedtls",
         {"mbedtls.txt"},
         "Mbed TLS is dual licensed under Apache-2.0 OR GPL-2.0-or-later. "
         "QSoC uses it under Apache-2.0."},
        {"mimalloc", "MIT", "https://github.com/microsoft/mimalloc", {"mimalloc.txt"}, {}},
        {"msvc-runtime",
         "Microsoft Software License Terms",
         "https://learn.microsoft.com/cpp/windows/latest-supported-vc-redist",
         {"msvc-runtime.txt"},
         {}},
        {"qschematic", "MIT", "https://github.com/simulton/QSchematic", {"qschematic.txt"}, {}},
        {"qt", "LGPL-3.0-only", "https://www.qt.io", {"qt.txt", "lgpl-3.0.txt", "gpl-3.0.txt"}, {}},
        {"replxx", "BSD-3-Clause", "https://github.com/AmokHuginnsson/replxx", {"replxx.txt"}, {}},
        {"slang",
         "MIT AND BSL-1.0",
         "https://github.com/MikePopoloski/slang",
         {"slang.txt", "slang-bsl-1.0.txt"},
         "slang bundles boost_unordered and expected-lite under BSL-1.0, and "
         "BS_thread_pool under MIT, Copyright (c) 2021-2026 Barak Shoshany."},
        {"systemrdl", "MIT", "https://github.com/vowstar/systemrdl-toolkit", {"systemrdl.txt"}, {}},
        {"tiktoken",
         "MIT",
         "https://github.com/openai/tiktoken",
         {"tiktoken.txt"},
         "The o200k_base table. The same table ships under Apache-2.0 in the "
         "gpt-oss tokenizer."},
        {"uvm-core",
         "Apache-2.0",
         "https://github.com/accellera-official/uvm-core",
         {"uvm-core.txt", "uvm-core-notice.txt"},
         {}},
        {"yaml", "MIT", "https://github.com/jbeder/yaml-cpp", {"yaml.txt"}, {}},
        {"z3", "MIT", "https://github.com/Z3Prover/z3", {"z3.txt"}, {}},
    };
    return list;
}

const QSocLicense::Component *QSocLicense::find(const QString &name)
{
    for (const Component &component : components()) {
        if (component.name == name) {
            return &component;
        }
    }
    return nullptr;
}

QString QSocLicense::text(const Component &component)
{
    QStringList parts;
    if (!component.note.isEmpty()) {
        parts.append(component.note);
    }
    for (const QString &name : component.files) {
        QFile file(QStringLiteral(":/license/") + name);
        if (file.open(QIODevice::ReadOnly)) {
            parts.append(QString::fromUtf8(file.readAll()).trimmed());
        }
    }
    return parts.join(QStringLiteral("\n\n")) + QLatin1Char('\n');
}
