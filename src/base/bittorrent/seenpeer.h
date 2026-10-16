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
    // Where libtorrent learned about a peer. Same bits as lt::peer_info::peer_source_flags.
    enum PeerSource : quint8
    {
        PeerSourceTracker = 1,
        PeerSourceDHT = 2,
        PeerSourcePeX = 4,
        PeerSourceLSD = 8,
        PeerSourceResumeData = 16,
        PeerSourceIncoming = 32
    };

    // A peer libtorrent was told about (by a tracker, DHT, PeX, ...), whether it
    // was allowed into the peer list or refused (IP filter, port filter, ...).
    struct SeenPeer
    {
        QString ip;
        quint16 port = 0;
        quint8 sources = 0;  // PeerSource bits, all sources that ever reported it
        bool isBlocked = false;
        bool isBlockedByIPFilter = false;
        QString blockReason;
        QDateTime firstSeen;
        QDateTime lastSeen;
        int hits = 0;  // how many times libtorrent was told about it
    };
}
