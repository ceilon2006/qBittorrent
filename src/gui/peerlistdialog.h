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

#include "base/settingvalue.h"

class QCheckBox;
class QComboBox;
class QLabel;
class QTableWidget;

namespace BitTorrent
{
    class Torrent;
}

// "Get peer list": every peer of one torrent in one table - all peers libtorrent
// was told about (tracker, DHT, PeX, LSD), connected, not connected or refused.
class PeerListDialog final : public QDialog
{
    Q_OBJECT
    Q_DISABLE_COPY_MOVE(PeerListDialog)

public:
    explicit PeerListDialog(const BitTorrent::Torrent *torrent, QWidget *parent = nullptr);

private:
    void refresh();
    // Hides the rows the IPv4/IPv6 boxes and the "seen in the last" choice leave out
    void applyFilter();
    QList<int> visibleRows() const;
    void copyAll() const;
    void copyAddresses() const;

    QPointer<const BitTorrent::Torrent> m_torrent;
    QTableWidget *m_table = nullptr;
    QLabel *m_summary = nullptr;
    QCheckBox *m_showIPv4 = nullptr;
    QCheckBox *m_showIPv6 = nullptr;
    QComboBox *m_seenWithin = nullptr;

    SettingValue<bool> m_storeShowIPv4;
    SettingValue<bool> m_storeShowIPv6;
    SettingValue<int> m_storeSeenWithinHours;
};
