/*
 * Bittorrent Client using Qt and libtorrent.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 */

#pragma once

#include <QDateTime>
#include <QString>

namespace BitTorrent
{
    // A peer that libtorrent refused (IP filter, port filter, ...).
    // These never reach the connected-peers list, so they are kept here.
    struct SeenPeer
    {
        QString ip;
        quint16 port = 0;
        QString reason;
        QDateTime firstSeen;
        QDateTime lastSeen;
        int hits = 0;  // how many times it was blocked
    };
}
