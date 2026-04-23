// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCSSHPUBDERIVE_H
#define QSOCSSHPUBDERIVE_H

#include <QString>

/**
 * @brief Derive the SSH public-key wire string from a private-key file.
 * @details Parses classical PEM RSA/ECDSA via the bundled mbedTLS and the
 *          unencrypted `openssh-key-v1` container (Ed25519, RSA, ECDSA)
 *          with a small in-house reader. libssh2 on the mbedTLS backend
 *          cannot derive a public half on its own for EC or Ed25519
 *          keys, so auth without a sibling `.pub` fails even when the
 *          private key is valid; this module fills that gap without
 *          spawning external binaries.
 *
 * Security posture: the function reads the private-key bytes only through
 * mbedTLS parsers or its own bounds-checked reader, zeroizes working
 * buffers before release, never logs key material, and refuses
 * passphrase-protected inputs instead of attempting to guess an empty
 * password. Callers are expected to check the return value; an empty
 * QString means "no usable public key, fall back to the next identity".
 */
namespace QSocSshPubDerive {

/**
 * @brief Emit the `ssh-<type> <base64>` line for a private-key file.
 * @param privateKeyPath Absolute path on disk.
 * @return The wire-format line without a trailing newline, or an empty
 *         QString when the file is missing, encrypted, malformed, or of
 *         an unsupported algorithm.
 */
QString fromPrivateKeyFile(const QString &privateKeyPath);

} // namespace QSocSshPubDerive

#endif // QSOCSSHPUBDERIVE_H
