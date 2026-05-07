// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCWINCONSOLE_H
#define QSOCWINCONSOLE_H

#include <QtGlobal>

/* Windows-only console bootstrap: forces UTF-8 code page and enables VT
   processing for the lifetime of this process, then restores prior state
   on shutdown. On non-Windows platforms every entry point is a no-op. */
class QSocWinConsole
{
public:
    static void bootstrap();
    static void restore();
};

#endif /* QSOCWINCONSOLE_H */
