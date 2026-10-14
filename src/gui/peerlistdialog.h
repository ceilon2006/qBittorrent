/*
 * Bittorrent Client using Qt and libtorrent.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 */

#pragma once

#include <QDialog>
#include <QPointer>

class QLabel;
class QTableWidget;

namespace BitTorrent
{
    class Torrent;
}

// "Get peer list": every peer of one torrent in one table -
// the ones we are connected to plus the ones the IP filter refused.
class PeerListDialog final : public QDialog
{
    Q_OBJECT
    Q_DISABLE_COPY_MOVE(PeerListDialog)

public:
    explicit PeerListDialog(const BitTorrent::Torrent *torrent, QWidget *parent = nullptr);

private:
    void refresh();
    void copyAll() const;

    QPointer<const BitTorrent::Torrent> m_torrent;
    QTableWidget *m_table = nullptr;
    QLabel *m_summary = nullptr;
};
