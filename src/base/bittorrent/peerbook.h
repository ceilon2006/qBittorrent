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
#include <QSet>
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
        // libtorrent refused the peer (peer_blocked_alert). `isUnreachable` is for refusals that
        // only mean this machine cannot reach the address, not counted as blocked.
        void markRefused(const lt::torrent_handle &torrent, const lt::tcp::endpoint &endpoint
            , const QString &reason, bool isUnreachable);
        // Outgoing connection results: a failure before the connection was set up
        // marks the peer unreachable with the error, a success clears that again
        void markConnectFailed(const lt::torrent_handle &torrent, const lt::tcp::endpoint &endpoint, const QString &reason);
        void markConnected(const lt::torrent_handle &torrent, const lt::tcp::endpoint &endpoint);
        void removeTorrent(const lt::torrent_handle &torrent);

        // This machine's own addresses (interfaces and external IPs); peers with one of
        // them are shown as this machine instead of blocked or unreachable
        void setLocalAddresses(const QSet<QString> &addresses);

        // Re-checks every peer against a new IP filter: peers it refuses become blocked,
        // all others lose any earlier refusal (filter, settings or unreachable)
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
        SeenPeer *find(TorrentPeers &torrentPeers, const QString &ip, quint16 port);
        bool isLocalAddress(const QString &ip) const;
        static void clearRefusal(TorrentPeers &torrentPeers, SeenPeer &peer);
        static void setBlocked(TorrentPeers &torrentPeers, SeenPeer &peer, bool blocked);

        const QString m_ipFilterReason;
        mutable QMutex m_mutex;
        QSet<QString> m_localAddresses;  // normalized, without IPv6 scope
        // torrent_handle orders by owner, so entries stay findable after the torrent is gone
        std::map<lt::torrent_handle, TorrentPeers> m_torrents;
    };
}
