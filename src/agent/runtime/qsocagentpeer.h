// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCAGENTPEER_H
#define QSOCAGENTPEER_H

#include <QtGlobal>

class QLocalSocket;

/**
 * @brief Identity of the process on the other end of an agent socket.
 * @details Unix sockets report the peer through the kernel. On Windows the
 *          pipe's access list already limits clients to the same user.
 */
namespace QSocAgentPeer {

/** True when the peer runs as this user. False when it cannot be read. */
bool sameUser(const QLocalSocket &socket);

/** Process id of the peer, or -1 when the platform cannot tell. */
qint64 processId(const QLocalSocket &socket);

} // namespace QSocAgentPeer

#endif // QSOCAGENTPEER_H
