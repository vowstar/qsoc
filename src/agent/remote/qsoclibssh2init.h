// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCLIBSSH2INIT_H
#define QSOCLIBSSH2INIT_H

/**
 * @brief Process-lifetime libssh2_init / libssh2_exit guard.
 * @details `libssh2_init(0)` must be called exactly once per process. The
 *          first `ensure()` call initializes the library; subsequent calls
 *          increment a reference count. The process-level singleton survives
 *          until program exit.
 */
class QSocLibSsh2Init
{
public:
    /** @brief Initialize libssh2 if not already initialized. Safe to call repeatedly. */
    static void ensure();

    /** @brief Returns the number of times `ensure()` has been called. */
    static int useCount();

private:
    QSocLibSsh2Init()  = delete;
    ~QSocLibSsh2Init() = delete;
};

#endif // QSOCLIBSSH2INIT_H
