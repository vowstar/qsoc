// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QLSPCONFIGLOADER_H
#define QLSPCONFIGLOADER_H

class QLspService;
class QSocConfig;

/**
 * @brief Loads external LSP server configuration from QSocConfig and
 *        registers them with the LSP service.
 * @details Reads the lsp.servers map from qsoc.yml. Each entry becomes a
 *          QLspProcessBackend registered with overrideExisting=true so
 *          external servers replace the built-in slang fallback for
 *          matching file extensions.
 */
class QLspConfigLoader
{
public:
    /**
     * @brief Load LSP server entries from config and register backends.
     * @param service Target service (must be non-null).
     * @param config Config provider (no-op if null).
     */
    static void loadAndRegister(QLspService *service, QSocConfig *config);
};

#endif // QLSPCONFIGLOADER_H
