/*
 * Bittorrent Client using Qt and libtorrent.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 */

#include "peerlistdialog.h"

#include <algorithm>

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QFuture>
#include <QHash>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QHostAddress>
#include <QItemSelectionModel>
#include <QLabel>
#include <QPushButton>
#include <QStringList>
#include <QTableWidget>
#include <QVBoxLayout>

#include "base/bittorrent/infohash.h"
#include "base/bittorrent/peeraddress.h"
#include "base/bittorrent/peerinfo.h"
#include "base/bittorrent/seenpeer.h"
#include "base/bittorrent/session.h"
#include "base/bittorrent/torrent.h"
#include "base/global.h"

#define SETTINGS_KEY(name) u"PeerListDialog/" name

namespace
{
    // Row data kept on the items, for filtering without asking libtorrent again
    const int ROLE_FILTER = Qt::UserRole + 1;

    enum class AddressFamily
    {
        IPv4,
        IPv6,
        Other  // I2P
    };

    enum class PeerState
    {
        Connected,
        NotConnected,
        Blocked,
        Unreachable,
        Self
    };

    const int DEFAULT_SEEN_WITHIN_HOURS = 7 * 24;

    AddressFamily addressFamily(const QString &ip)
    {
        switch (QHostAddress(ip).protocol())
        {
        case QAbstractSocket::IPv4Protocol:
            return AddressFamily::IPv4;
        case QAbstractSocket::IPv6Protocol:
            return AddressFamily::IPv6;
        default:
            return AddressFamily::Other;
        }
    }
    enum Column
    {
        COL_IP,
        COL_PORT,
        COL_STATUS,
        COL_SOURCE,
        COL_CLIENT,
        COL_PROGRESS,
        COL_DOWN_SPEED,
        COL_UP_SPEED,
        COL_DETAILS,
        COL_FIRST_SEEN,
        COL_LAST_SEEN,
        COL_HITS,

        COL_COUNT
    };

    QTableWidgetItem *textItem(const QString &text)
    {
        auto *item = new QTableWidgetItem(text);
        item->setFlags(item->flags() & ~Qt::ItemIsEditable);
        return item;
    }

    // Stores the number itself so that sorting is numeric
    QTableWidgetItem *numberItem(const qlonglong value, const QString &text = {})
    {
        auto *item = new QTableWidgetItem;
        item->setData(Qt::DisplayRole, value);
        if (!text.isEmpty())
            item->setData(Qt::ToolTipRole, text);
        item->setFlags(item->flags() & ~Qt::ItemIsEditable);
        return item;
    }

    QString timeString(const QDateTime &time)
    {
        return time.toLocalTime().toString(u"yyyy-MM-dd HH:mm:ss"_s);
    }

    QString peerKey(const QString &ip, const quint16 port)
    {
        return ip + u'|' + QString::number(port);
    }

    QString sourcesString(const quint8 sources)
    {
        QStringList names;
        if (sources & BitTorrent::PeerSourceTracker)
            names << PeerListDialog::tr("Tracker");
        if (sources & BitTorrent::PeerSourceDHT)
            names << u"DHT"_s;
        if (sources & BitTorrent::PeerSourcePeX)
            names << u"PeX"_s;
        if (sources & BitTorrent::PeerSourceLSD)
            names << u"LSD"_s;
        if (sources & BitTorrent::PeerSourceResumeData)
            names << PeerListDialog::tr("Resume data");
        if (sources & BitTorrent::PeerSourceIncoming)
            names << PeerListDialog::tr("Incoming");
        return names.join(u", "_s);
    }

    QString sourcesString(const BitTorrent::PeerInfo &peer)
    {
        QStringList names;
        if (peer.fromDHT())
            names << u"DHT"_s;
        if (peer.fromPeX())
            names << u"PeX"_s;
        if (peer.fromLSD())
            names << u"LSD"_s;
        return names.join(u", "_s);
    }

    QString connectedStatus(const BitTorrent::PeerInfo &peer)
    {
        if (peer.isConnecting())
            return PeerListDialog::tr("Connecting (%1)").arg(peer.connectionType());
        if (peer.isHandshake())
            return PeerListDialog::tr("Handshake (%1)").arg(peer.connectionType());
        return PeerListDialog::tr("Connected (%1)").arg(peer.connectionType());
    }
}

PeerListDialog::PeerListDialog(const BitTorrent::Torrent *torrent, QWidget *parent)
    : QDialog(parent)
    , m_torrent {torrent}
    , m_table {new QTableWidget(0, COL_COUNT, this)}
    , m_summary {new QLabel(this)}
    , m_showIPv4 {new QCheckBox(tr("IPv4"), this)}
    , m_showIPv6 {new QCheckBox(tr("IPv6"), this)}
    , m_seenWithin {new QComboBox(this)}
    , m_storeShowIPv4 {SETTINGS_KEY(u"ShowIPv4"_s)}
    , m_storeShowIPv6 {SETTINGS_KEY(u"ShowIPv6"_s)}
    , m_storeSeenWithinHours {SETTINGS_KEY(u"SeenWithinHours"_s)}
{
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowTitle(tr("Peer list - %1").arg(torrent->name()));
    resize(1000, 550);

    m_table->setHorizontalHeaderLabels({
        tr("IP"), tr("Port"), tr("Status"), tr("Source"), tr("Client"), tr("Progress %")
        , tr("Down (KiB/s)"), tr("Up (KiB/s)"), tr("Flags / Reason")
        , tr("First seen"), tr("Last seen"), tr("Times reported")});
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->verticalHeader()->setVisible(false);
    m_table->horizontalHeader()->setStretchLastSection(true);
    // Sorting moves the rows but not their hidden state, so filter again afterwards
    connect(m_table->model(), &QAbstractItemModel::layoutChanged, this, &PeerListDialog::applyFilter);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    QPushButton *refreshButton = buttons->addButton(tr("&Refresh"), QDialogButtonBox::ActionRole);
    QPushButton *copyButton = buttons->addButton(tr("&Copy all"), QDialogButtonBox::ActionRole);
    QPushButton *copyAddressesButton = buttons->addButton(tr("Copy &IP:port"), QDialogButtonBox::ActionRole);
    copyButton->setToolTip(tr("Copy the shown rows with all columns, tab separated"));
    copyAddressesButton->setToolTip(tr("Copy one IP:port per line (selected rows, or all shown rows if none are selected)"));
    connect(refreshButton, &QPushButton::clicked, this, &PeerListDialog::refresh);
    connect(copyButton, &QPushButton::clicked, this, &PeerListDialog::copyAll);
    connect(copyAddressesButton, &QPushButton::clicked, this, &PeerListDialog::copyAddresses);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    m_showIPv4->setChecked(m_storeShowIPv4.get(true));
    m_showIPv6->setChecked(m_storeShowIPv6.get(true));
    if (!m_showIPv4->isChecked() && !m_showIPv6->isChecked())
        m_showIPv4->setChecked(true);

    m_seenWithin->addItem(tr("1 hour"), 1);
    m_seenWithin->addItem(tr("6 hours"), 6);
    m_seenWithin->addItem(tr("24 hours"), 24);
    m_seenWithin->addItem(tr("7 days"), DEFAULT_SEEN_WITHIN_HOURS);
    const int seenWithinIndex = m_seenWithin->findData(m_storeSeenWithinHours.get(DEFAULT_SEEN_WITHIN_HOURS));
    m_seenWithin->setCurrentIndex((seenWithinIndex >= 0) ? seenWithinIndex : (m_seenWithin->count() - 1));
    m_seenWithin->setToolTip(tr("Show only peers a tracker, DHT, PeX, ... reported within this time. Connected peers are always shown."));

    // At least one address family stays shown, an empty table helps nobody
    const auto onFamilyToggled = [this](QCheckBox *box, QCheckBox *other, SettingValue<bool> &store)
    {
        if (!box->isChecked() && !other->isChecked())
        {
            box->setChecked(true);
            return;
        }
        store = box->isChecked();
        applyFilter();
    };
    connect(m_showIPv4, &QCheckBox::toggled, this, [this, onFamilyToggled] { onFamilyToggled(m_showIPv4, m_showIPv6, m_storeShowIPv4); });
    connect(m_showIPv6, &QCheckBox::toggled, this, [this, onFamilyToggled] { onFamilyToggled(m_showIPv6, m_showIPv4, m_storeShowIPv6); });
    connect(m_seenWithin, &QComboBox::currentIndexChanged, this, [this]
    {
        m_storeSeenWithinHours = m_seenWithin->currentData().toInt();
        applyFilter();
    });

    auto *bottomLayout = new QHBoxLayout;
    bottomLayout->addWidget(m_showIPv4);
    bottomLayout->addWidget(m_showIPv6);
    bottomLayout->addSpacing(12);
    bottomLayout->addWidget(new QLabel(tr("Seen in the last:"), this));
    bottomLayout->addWidget(m_seenWithin);
    bottomLayout->addStretch();
    bottomLayout->addWidget(buttons);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(m_summary);
    layout->addWidget(m_table);
    layout->addLayout(bottomLayout);

    refresh();
}

void PeerListDialog::refresh()
{
    if (!m_torrent)
    {
        m_summary->setText(tr("The torrent was removed."));
        return;
    }

    // Known peers are read now; connected peers arrive asynchronously
    const QList<BitTorrent::SeenPeer> known = BitTorrent::Session::instance()->seenPeers(m_torrent->id());

    m_torrent->fetchPeerInfo().then(this, [this, known](const QList<BitTorrent::PeerInfo> &connected)
    {
        if (!m_torrent)
            return;

        QHash<QString, const BitTorrent::PeerInfo *> connectedByKey;
        for (const BitTorrent::PeerInfo &peer : connected)
        {
            if (!peer.useI2PSocket())
                connectedByKey.insert(peerKey(peer.address().ip.toString(), peer.address().port), &peer);
        }

        m_table->setSortingEnabled(false);
        m_table->setRowCount(0);
        m_table->setRowCount(static_cast<int>(known.size() + connected.size()));

        int row = 0;
        const auto setFilterData = [this, &row](const QString &ip, const PeerState state, const QDateTime &lastSeen)
        {
            m_table->item(row, COL_IP)->setData(ROLE_FILTER, static_cast<int>(addressFamily(ip)));
            m_table->item(row, COL_STATUS)->setData(ROLE_FILTER, static_cast<int>(state));
            if (QTableWidgetItem *lastSeenItem = m_table->item(row, COL_LAST_SEEN))
                lastSeenItem->setData(ROLE_FILTER, lastSeen);
        };

        const auto fillConnected = [this, &row](const BitTorrent::PeerInfo &peer)
        {
            m_table->setItem(row, COL_STATUS, textItem(connectedStatus(peer)));
            m_table->setItem(row, COL_CLIENT, textItem(peer.client()));
            m_table->setItem(row, COL_PROGRESS, numberItem(qRound(peer.progress() * 100)));
            m_table->setItem(row, COL_DOWN_SPEED, numberItem(peer.payloadDownSpeed() / 1024));
            m_table->setItem(row, COL_UP_SPEED, numberItem(peer.payloadUpSpeed() / 1024));
            QTableWidgetItem *flagsItem = textItem(peer.flags());
            flagsItem->setToolTip(peer.flagsDescription());
            m_table->setItem(row, COL_DETAILS, flagsItem);
        };

        const QColor blockedColor = palette().color(QPalette::Disabled, QPalette::Text);

        // Every peer libtorrent was told about, with live data for the connected ones
        for (const BitTorrent::SeenPeer &peer : known)
        {
            m_table->setItem(row, COL_IP, textItem(peer.ip));
            m_table->setItem(row, COL_PORT, numberItem(peer.port));
            m_table->setItem(row, COL_SOURCE, textItem(sourcesString(peer.sources)));
            m_table->setItem(row, COL_FIRST_SEEN, textItem(timeString(peer.firstSeen)));
            m_table->setItem(row, COL_LAST_SEEN, textItem(timeString(peer.lastSeen)));
            m_table->setItem(row, COL_HITS, numberItem(peer.hits));

            if (peer.isSelf)
            {
                // libtorrent heard about this machine itself (LSD, a tracker echoing it back)
                m_table->setItem(row, COL_STATUS, textItem(tr("This machine")));
                m_table->setItem(row, COL_DETAILS, textItem(tr("own address")));
                for (int col = 0; col < COL_COUNT; ++col)
                {
                    if (QTableWidgetItem *item = m_table->item(row, col))
                        item->setForeground(blockedColor);
                }
                setFilterData(peer.ip, PeerState::Self, peer.lastSeen);
            }
            else if (peer.isBlocked)
            {
                m_table->setItem(row, COL_STATUS, textItem(tr("Blocked")));
                m_table->setItem(row, COL_DETAILS, textItem(peer.blockReason));
                for (int col = 0; col < COL_COUNT; ++col)
                {
                    if (QTableWidgetItem *item = m_table->item(row, col))
                        item->setForeground(blockedColor);
                }
                setFilterData(peer.ip, PeerState::Blocked, peer.lastSeen);
            }
            else if (const BitTorrent::PeerInfo *connectedPeer = connectedByKey.take(peerKey(peer.ip, peer.port)))
            {
                fillConnected(*connectedPeer);
                setFilterData(peer.ip, PeerState::Connected, peer.lastSeen);
            }
            else if (peer.isUnreachable)
            {
                // Not refused: this machine has no way to reach the address
                m_table->setItem(row, COL_STATUS, textItem(tr("Unreachable")));
                m_table->setItem(row, COL_DETAILS, textItem(peer.blockReason));
                setFilterData(peer.ip, PeerState::Unreachable, peer.lastSeen);
            }
            else
            {
                m_table->setItem(row, COL_STATUS, textItem(tr("Not connected")));
                setFilterData(peer.ip, PeerState::NotConnected, peer.lastSeen);
            }
            ++row;
        }

        // Connected peers nobody reported, mostly incoming connections
        for (const BitTorrent::PeerInfo &peer : connected)
        {
            const bool isI2P = peer.useI2PSocket();
            if (!isI2P && !connectedByKey.contains(peerKey(peer.address().ip.toString(), peer.address().port)))
                continue;  // already shown above

            const QString ip = isI2P ? peer.I2PAddress() : peer.address().ip.toString();
            m_table->setItem(row, COL_IP, textItem(ip));
            m_table->setItem(row, COL_PORT, numberItem(isI2P ? 0 : peer.address().port));
            const QString sources = sourcesString(peer);
            m_table->setItem(row, COL_SOURCE, textItem(sources.isEmpty() ? tr("Incoming") : sources));
            fillConnected(peer);
            setFilterData(ip, PeerState::Connected, {});
            ++row;
        }
        m_table->setRowCount(row);

        m_table->setSortingEnabled(true);
        m_table->resizeColumnsToContents();
        applyFilter();
    });
}

void PeerListDialog::applyFilter()
{
    const bool showIPv4 = m_showIPv4->isChecked();
    const bool showIPv6 = m_showIPv6->isChecked();
    const QDateTime cutoff = QDateTime::currentDateTimeUtc().addSecs(-3600LL * m_seenWithin->currentData().toInt());

    int connectedCount = 0;
    int notConnectedCount = 0;
    int blockedCount = 0;
    int unreachableCount = 0;
    int selfCount = 0;
    for (int row = 0; row < m_table->rowCount(); ++row)
    {
        const QTableWidgetItem *ipItem = m_table->item(row, COL_IP);
        const QTableWidgetItem *statusItem = m_table->item(row, COL_STATUS);
        if (!ipItem || !statusItem)
            continue;

        const auto family = static_cast<AddressFamily>(ipItem->data(ROLE_FILTER).toInt());
        const auto state = static_cast<PeerState>(statusItem->data(ROLE_FILTER).toInt());

        bool visible = ((family != AddressFamily::IPv4) || showIPv4)
            && ((family != AddressFamily::IPv6) || showIPv6);
        // Connected peers are seen right now, whenever they were last reported
        if (visible && (state != PeerState::Connected))
        {
            const QTableWidgetItem *lastSeenItem = m_table->item(row, COL_LAST_SEEN);
            const QDateTime lastSeen = lastSeenItem ? lastSeenItem->data(ROLE_FILTER).toDateTime() : QDateTime();
            visible = (lastSeen.isValid() && (lastSeen >= cutoff));
        }

        m_table->setRowHidden(row, !visible);
        if (!visible)
            continue;

        switch (state)
        {
        case PeerState::Connected:
            ++connectedCount;
            break;
        case PeerState::NotConnected:
            ++notConnectedCount;
            break;
        case PeerState::Blocked:
            ++blockedCount;
            break;
        case PeerState::Unreachable:
            ++unreachableCount;
            break;
        case PeerState::Self:
            ++selfCount;
            break;
        }
    }

    const int shownCount = connectedCount + notConnectedCount + blockedCount + unreachableCount + selfCount;
    QString summary = tr("%1 of %2 peers shown: %3 connected, %4 not connected (not tried yet), %5 blocked, %6 unreachable (connecting failed)")
        .arg(shownCount).arg(m_table->rowCount()).arg(connectedCount).arg(notConnectedCount).arg(blockedCount).arg(unreachableCount);
    if (selfCount > 0)
        summary += tr(", %1 this machine").arg(selfCount);
    summary += u". "_s + tr("These are all peers libtorrent was told about (tracker, DHT, PeX, LSD); each is kept for 7 days after it was last reported.");
    m_summary->setWordWrap(true);
    m_summary->setText(summary);
}

QList<int> PeerListDialog::visibleRows() const
{
    QList<int> rows;
    for (int row = 0; row < m_table->rowCount(); ++row)
    {
        if (!m_table->isRowHidden(row))
            rows << row;
    }
    return rows;
}

void PeerListDialog::copyAll() const
{
    QStringList lines;

    QStringList header;
    for (int col = 0; col < m_table->columnCount(); ++col)
        header << m_table->horizontalHeaderItem(col)->text();
    lines << header.join(u'\t');

    for (const int row : asConst(visibleRows()))
    {
        QStringList cells;
        for (int col = 0; col < m_table->columnCount(); ++col)
        {
            const QTableWidgetItem *item = m_table->item(row, col);
            cells << (item ? item->text() : QString());
        }
        lines << cells.join(u'\t');
    }

    QApplication::clipboard()->setText(lines.join(u'\n'));
}

void PeerListDialog::copyAddresses() const
{
    QList<int> rows;
    for (const QModelIndex &index : asConst(m_table->selectionModel()->selectedRows()))
    {
        if (!m_table->isRowHidden(index.row()))
            rows << index.row();
    }
    std::sort(rows.begin(), rows.end());

    // Nothing selected: take every shown row
    if (rows.isEmpty())
        rows = visibleRows();

    QStringList lines;
    for (const int row : asConst(rows))
    {
        const QTableWidgetItem *ipItem = m_table->item(row, COL_IP);
        if (!ipItem || ipItem->text().isEmpty())
            continue;

        const QString ip = ipItem->text();
        const QTableWidgetItem *portItem = m_table->item(row, COL_PORT);
        const int port = portItem ? portItem->data(Qt::DisplayRole).toInt() : 0;

        // Port 0 means unknown (or an I2P peer): the address alone is the best we have
        if (port == 0)
            lines << ip;
        else if (ip.contains(u':'))  // IPv6
            lines << u"[%1]:%2"_s.arg(ip, QString::number(port));
        else
            lines << u"%1:%2"_s.arg(ip, QString::number(port));
    }

    QApplication::clipboard()->setText(lines.join(u'\n'));
}
