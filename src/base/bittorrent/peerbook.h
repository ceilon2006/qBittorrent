/*
 * Bittorrent Client using Qt and libtorrent.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 */

#pragma once

#include <map>

#include <libtorrent/fwd.hpp>
#include <libtorrent/peer_info.hpp>
#include <libtorrent/socket.hpp>
#include <libtorrent/torrent_handle.hpp>

#include <QDateTime>
#include <QHash>
#include <QList>
#include <QMutex>
#include <QString>

#include "seenpeer.h"

namespace BitTorrent
{
    // Every peer libtorrent was told about, per torrent: from trackers, DHT, PeX, LSD,
    // including the ones the IP filter refused. libtorrent itself only exposes the
    // peers it is connected to, so they are collected here as they are reported.
    //
    // Written from libtorrent's network thread (NativeTorrentExtension::on_add_peer)
    // and read from the GUI thread, so every access takes the mutex.
    class PeerBook
    {
    public:
        explicit PeerBook(const QString &ipFilterReason);

        void addPeer(const lt::torrent_handle &torrent, const lt::tcp::endpoint &endpoint
            , lt::peer_source_flags_t sources, bool isFiltered);
        void markBlocked(const lt::torrent_handle &torrent, const lt::tcp::endpoint &endpoint, const QString &reason);
        void removeTorrent(const lt::torrent_handle &torrent);

        // Re-checks every peer against a new IP filter: refused peers it now allows
        // become plain known peers again, and the other way round
        void applyIPFilter(const lt::ip_filter &filter);
        // Drops peers nobody reported since `cutoff`, and torrents that are gone
        void prune(const QDateTime &cutoff);

        QList<SeenPeer> peers(const lt::torrent_handle &torrent) const;
        int blockedCount(const lt::torrent_handle &torrent) const;

    private:
        struct TorrentPeers
        {
            QHash<QString, SeenPeer> peers;  // "ip|port" -> peer
            int blockedCount = 0;
        };

        SeenPeer *findOrInsert(TorrentPeers &torrentPeers, const QString &ip, quint16 port, const QDateTime &now);
        static void setBlocked(TorrentPeers &torrentPeers, SeenPeer &peer, bool blocked);

        const QString m_ipFilterReason;
        mutable QMutex m_mutex;
        // torrent_handle orders by owner, so entries stay findable after the torrent is gone
        std::map<lt::torrent_handle, TorrentPeers> m_torrents;
    };
}
