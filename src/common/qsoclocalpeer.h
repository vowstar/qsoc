// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCLOCALPEER_H
#define QSOCLOCALPEER_H

#include <QtGlobal>

class QLocalSocket;

/**
 * @brief Identity of the process on the other end of an agent socket.
 * @details Checks the peer's kernel credentials, including its user SID on Windows.
 */
namespace QSocLocalPeer {

/** True when the peer runs as this user. False when it cannot be read. */
bool sameUser(const QLocalSocket &socket);

/** Process id of the peer, or -1 when the platform cannot tell. */
qint64 processId(const QLocalSocket &socket);

} // namespace QSocLocalPeer

#endif // QSOCLOCALPEER_H
