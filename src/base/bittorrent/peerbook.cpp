/*
 * Bittorrent Client using Qt and libtorrent.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 */

#include "peerbook.h"

#include <libtorrent/address.hpp>
#include <libtorrent/error_code.hpp>
#include <libtorrent/ip_filter.hpp>

#include <QHostAddress>
#include <QMutexLocker>

#include "base/algorithm.h"
#include "base/global.h"

namespace
{
    // Safety cap per torrent so a huge swarm can't eat unlimited memory
    const qsizetype MAX_PEERS_PER_TORRENT = 50000;

    QString makeKey(const QString &ip, const quint16 port)
    {
        return ip + u'|' + QString::number(port);
    }

    // Comparable form of an address: no IPv6 scope, IPv4-mapped IPv6 as plain IPv4
    QString normalizedAddress(const QString &ip)
    {
        QHostAddress address {ip};
        if (address.isNull())
            return ip;

        address.setScopeId({});
        bool isIPv4 = false;
        const quint32 ipv4 = address.toIPv4Address(&isIPv4);
        if (isIPv4)
            return QHostAddress(ipv4).toString();
        return address.toString();
    }
}

using namespace BitTorrent;

PeerBook::PeerBook(const QString &ipFilterReason)
    : m_ipFilterReason {ipFilterReason}
{
}

SeenPeer *PeerBook::findOrInsert(TorrentPeers &torrentPeers, const QString &ip, const quint16 port, const QDateTime &now)
{
    const QString key = makeKey(ip, port);
    auto iter = torrentPeers.peers.find(key);
    if (iter == torrentPeers.peers.end())
    {
        if (torrentPeers.peers.size() >= MAX_PEERS_PER_TORRENT)
            return nullptr;
        SeenPeer peer;
        peer.ip = ip;
        peer.port = port;
        peer.firstSeen = now;
        peer.isSelf = isLocalAddress(ip);
        iter = torrentPeers.peers.insert(key, peer);
    }
    return &iter.value();
}

SeenPeer *PeerBook::find(TorrentPeers &torrentPeers, const QString &ip, const quint16 port)
{
    const auto iter = torrentPeers.peers.find(makeKey(ip, port));
    return (iter != torrentPeers.peers.end()) ? &iter.value() : nullptr;
}

bool PeerBook::isLocalAddress(const QString &ip) const
{
    return m_localAddresses.contains(normalizedAddress(ip));
}

void PeerBook::clearRefusal(TorrentPeers &torrentPeers, SeenPeer &peer)
{
    setBlocked(torrentPeers, peer, false);
    peer.isUnreachable = false;
    peer.blockReason.clear();
}

void PeerBook::setBlocked(TorrentPeers &torrentPeers, SeenPeer &peer, const bool blocked)
{
    if (peer.isBlocked == blocked)
        return;

    peer.isBlocked = blocked;
    torrentPeers.blockedCount += (blocked ? 1 : -1);
    if (!blocked)
    {
        peer.isBlockedByIPFilter = false;
        peer.blockReason.clear();
    }
}

void PeerBook::addPeer(const lt::torrent_handle &torrent, const lt::tcp::endpoint &endpoint
    , const lt::peer_source_flags_t sources, const bool isFiltered)
{
    const QString ip = QString::fromStdString(endpoint.address().to_string());
    const QDateTime now = QDateTime::currentDateTimeUtc();

    const QMutexLocker locker {&m_mutex};
    TorrentPeers &torrentPeers = m_torrents[torrent];
    SeenPeer *peer = findOrInsert(torrentPeers, ip, endpoint.port(), now);
    if (!peer)
        return;

    peer->sources |= static_cast<quint8>(sources);
    peer->lastSeen = now;
    ++peer->hits;

    // libtorrent refuses its own addresses too, that is not the filter at work
    if (peer->isSelf)
        return;

    if (isFiltered)
    {
        if (!peer->isBlockedByIPFilter)
        {
            setBlocked(torrentPeers, *peer, true);
            peer->isBlockedByIPFilter = true;
            peer->isUnreachable = false;
            peer->blockReason = m_ipFilterReason;
        }
    }
    else if (peer->isBlockedByIPFilter)
    {
        // libtorrent accepted it this time, so the filter no longer refuses it
        setBlocked(torrentPeers, *peer, false);
    }
}

void PeerBook::markRefused(const lt::torrent_handle &torrent, const lt::tcp::endpoint &endpoint
    , const QString &reason, const bool isUnreachable)
{
    const QString ip = QString::fromStdString(endpoint.address().to_string());
    const quint16 port = endpoint.port();
    const QDateTime now = QDateTime::currentDateTimeUtc();

    const QMutexLocker locker {&m_mutex};
    TorrentPeers &torrentPeers = m_torrents[torrent];

    SeenPeer *peer = nullptr;
    if (port != 0)
    {
        if (const auto iter = torrentPeers.peers.find(makeKey(ip, port)); iter != torrentPeers.peers.end())
            peer = &iter.value();
    }
    else
    {
        // No port (e.g. an incoming connection): any entry of that IP will do
        for (SeenPeer &candidate : torrentPeers.peers)
        {
            if (candidate.ip == ip)
            {
                peer = &candidate;
                break;
            }
        }
    }

    if (peer)
    {
        // Usually on_add_peer() already counted this report, the alert only adds the reason
        if (peer->lastSeen.secsTo(now) > 1)
        {
            peer->lastSeen = now;
            ++peer->hits;
        }
    }
    else
    {
        peer = findOrInsert(torrentPeers, ip, port, now);
        if (!peer)
            return;
        peer->lastSeen = now;
        peer->hits = 1;
    }

    if (peer->isSelf)
        return;

    if (isUnreachable)
    {
        // A refusal by the filter or a setting says more than "cannot reach it"
        if (peer->isBlocked)
            return;
        peer->isUnreachable = true;
        peer->blockReason = reason;
        return;
    }

    setBlocked(torrentPeers, *peer, true);
    peer->isUnreachable = false;
    peer->blockReason = reason;
    peer->isBlockedByIPFilter = (reason == m_ipFilterReason);
}

void PeerBook::markConnectFailed(const lt::torrent_handle &torrent, const lt::tcp::endpoint &endpoint, const QString &reason)
{
    const QString ip = QString::fromStdString(endpoint.address().to_string());
    const QDateTime now = QDateTime::currentDateTimeUtc();

    const QMutexLocker locker {&m_mutex};
    TorrentPeers &torrentPeers = m_torrents[torrent];
    SeenPeer *peer = find(torrentPeers, ip, endpoint.port());
    if (!peer)
    {
        // Tried, but never reported to on_add_peer (e.g. added by hand)
        peer = findOrInsert(torrentPeers, ip, endpoint.port(), now);
        if (!peer)
            return;
        peer->lastSeen = now;
    }

    if (peer->isSelf || peer->isBlocked)
        return;

    peer->isUnreachable = true;
    peer->blockReason = reason;
}

void PeerBook::markConnected(const lt::torrent_handle &torrent, const lt::tcp::endpoint &endpoint)
{
    const QString ip = QString::fromStdString(endpoint.address().to_string());

    const QMutexLocker locker {&m_mutex};
    const auto torrentIter = m_torrents.find(torrent);
    if (torrentIter == m_torrents.end())
        return;

    SeenPeer *peer = find(torrentIter->second, ip, endpoint.port());
    if (peer && peer->isUnreachable)
    {
        peer->isUnreachable = false;
        peer->blockReason.clear();
    }
}

void PeerBook::setLocalAddresses(const QSet<QString> &addresses)
{
    const QMutexLocker locker {&m_mutex};
    m_localAddresses.clear();
    for (const QString &address : addresses)
        m_localAddresses.insert(normalizedAddress(address));

    for (auto &[torrent, torrentPeers] : m_torrents)
    {
        for (SeenPeer &peer : torrentPeers.peers)
        {
            const bool isSelf = isLocalAddress(peer.ip);
            if (isSelf && !peer.isSelf)
                clearRefusal(torrentPeers, peer);
            peer.isSelf = isSelf;
        }
    }
}

void PeerBook::removeTorrent(const lt::torrent_handle &torrent)
{
    const QMutexLocker locker {&m_mutex};
    m_torrents.erase(torrent);
}

void PeerBook::applyIPFilter(const lt::ip_filter &filter)
{
    const QMutexLocker locker {&m_mutex};
    for (auto &[torrent, torrentPeers] : m_torrents)
    {
        for (SeenPeer &peer : torrentPeers.peers)
        {
            if (peer.isSelf)
                continue;

            lt::error_code ec;
            const lt::address address = lt::make_address(peer.ip.toStdString(), ec);
            if (ec)
                continue;

            const bool isFiltered = (filter.access(address) & lt::ip_filter::blocked);
            if (isFiltered)
            {
                if (!peer.isBlockedByIPFilter)
                {
                    setBlocked(torrentPeers, peer, true);
                    peer.isBlockedByIPFilter = true;
                    peer.isUnreachable = false;
                    peer.blockReason = m_ipFilterReason;
                }
            }
            else if (peer.isBlocked || peer.isUnreachable)
            {
                // The new filter lets it through, so earlier refusals for other reasons
                // (settings, no route) are forgotten as well: libtorrent tries the peer
                // afresh and reports it again if it still fails
                clearRefusal(torrentPeers, peer);
            }
        }
    }
}

void PeerBook::prune(const QDateTime &cutoff)
{
    const QMutexLocker locker {&m_mutex};
    for (auto iter = m_torrents.begin(); iter != m_torrents.end();)
    {
        if (!iter->first.is_valid())
        {
            iter = m_torrents.erase(iter);
            continue;
        }

        TorrentPeers &torrentPeers = iter->second;
        Algorithm::removeIf(torrentPeers.peers, [&cutoff, &torrentPeers](const QString &, const SeenPeer &peer)
        {
            if (peer.lastSeen >= cutoff)
                return false;
            if (peer.isBlocked)
                --torrentPeers.blockedCount;
            return true;
        });

        if (torrentPeers.peers.isEmpty())
            iter = m_torrents.erase(iter);
        else
            ++iter;
    }
}

QList<SeenPeer> PeerBook::peers(const lt::torrent_handle &torrent) const
{
    const QMutexLocker locker {&m_mutex};
    const auto iter = m_torrents.find(torrent);
    if (iter == m_torrents.end())
        return {};
    return iter->second.peers.values();
}

int PeerBook::blockedCount(const lt::torrent_handle &torrent) const
{
    const QMutexLocker locker {&m_mutex};
    const auto iter = m_torrents.find(torrent);
    if (iter == m_torrents.end())
        return 0;
    return iter->second.blockedCount;
}
