// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCLOCALPEER_H
#define QSOCLOCALPEER_H

#include <QtGlobal>

class QLocalSocket;

/**
 * @brief Identity of the process on the other end of an agent socket.
 * @details Checks kernel credentials and matches the Windows user SID and integrity level.
 */
namespace QSocLocalPeer {

/** True for this user and, on Windows, integrity level. Unreadable credentials fail. */
bool sameUser(const QLocalSocket &socket);

/** Process id of the peer, or -1 when the platform cannot tell. */
qint64 processId(const QLocalSocket &socket);

} // namespace QSocLocalPeer

#endif // QSOCLOCALPEER_H
