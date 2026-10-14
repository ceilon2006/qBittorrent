/*
 * Bittorrent Client using Qt and libtorrent.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 */

#include "peerlistdialog.h"

#include <QApplication>
#include <QClipboard>
#include <QDialogButtonBox>
#include <QFuture>
#include <QHeaderView>
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

namespace
{
    enum Column
    {
        COL_IP,
        COL_PORT,
        COL_STATUS,
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
}

PeerListDialog::PeerListDialog(const BitTorrent::Torrent *torrent, QWidget *parent)
    : QDialog(parent)
    , m_torrent {torrent}
    , m_table {new QTableWidget(0, COL_COUNT, this)}
    , m_summary {new QLabel(this)}
{
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowTitle(tr("Peer list - %1").arg(torrent->name()));
    resize(1000, 550);

    m_table->setHorizontalHeaderLabels({
        tr("IP"), tr("Port"), tr("Status"), tr("Client"), tr("Progress %")
        , tr("Down (KiB/s)"), tr("Up (KiB/s)"), tr("Flags / Reason")
        , tr("First blocked"), tr("Last blocked"), tr("Times blocked")});
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->verticalHeader()->setVisible(false);
    m_table->horizontalHeader()->setStretchLastSection(true);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    QPushButton *refreshButton = buttons->addButton(tr("&Refresh"), QDialogButtonBox::ActionRole);
    QPushButton *copyButton = buttons->addButton(tr("&Copy all"), QDialogButtonBox::ActionRole);
    connect(refreshButton, &QPushButton::clicked, this, &PeerListDialog::refresh);
    connect(copyButton, &QPushButton::clicked, this, &PeerListDialog::copyAll);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(m_summary);
    layout->addWidget(m_table);
    layout->addWidget(buttons);

    refresh();
}

void PeerListDialog::refresh()
{
    if (!m_torrent)
    {
        m_summary->setText(tr("The torrent was removed."));
        return;
    }

    // Blocked peers are read now; connected peers arrive asynchronously
    const QList<BitTorrent::SeenPeer> blocked = BitTorrent::Session::instance()->seenPeers(m_torrent->id());

    m_torrent->fetchPeerInfo().then(this, [this, blocked](const QList<BitTorrent::PeerInfo> &connected)
    {
        if (!m_torrent)
            return;

        m_table->setSortingEnabled(false);
        m_table->setRowCount(0);
        m_table->setRowCount(static_cast<int>(connected.size() + blocked.size()));

        int row = 0;
        for (const BitTorrent::PeerInfo &peer : connected)
        {
            const bool isI2P = peer.useI2PSocket();
            m_table->setItem(row, COL_IP, textItem(isI2P ? peer.I2PAddress() : peer.address().ip.toString()));
            m_table->setItem(row, COL_PORT, numberItem(isI2P ? 0 : peer.address().port));
            m_table->setItem(row, COL_STATUS, textItem(tr("Connected (%1)").arg(peer.connectionType())));
            m_table->setItem(row, COL_CLIENT, textItem(peer.client()));
            m_table->setItem(row, COL_PROGRESS, numberItem(qRound(peer.progress() * 100)));
            m_table->setItem(row, COL_DOWN_SPEED, numberItem(peer.payloadDownSpeed() / 1024));
            m_table->setItem(row, COL_UP_SPEED, numberItem(peer.payloadUpSpeed() / 1024));
            QTableWidgetItem *flagsItem = textItem(peer.flags());
            flagsItem->setToolTip(peer.flagsDescription());
            m_table->setItem(row, COL_DETAILS, flagsItem);
            ++row;
        }

        const QColor blockedColor = palette().color(QPalette::Disabled, QPalette::Text);
        for (const BitTorrent::SeenPeer &peer : blocked)
        {
            m_table->setItem(row, COL_IP, textItem(peer.ip));
            m_table->setItem(row, COL_PORT, numberItem(peer.port));
            m_table->setItem(row, COL_STATUS, textItem(tr("Blocked")));
            m_table->setItem(row, COL_DETAILS, textItem(peer.reason));
            m_table->setItem(row, COL_FIRST_SEEN, textItem(timeString(peer.firstSeen)));
            m_table->setItem(row, COL_LAST_SEEN, textItem(timeString(peer.lastSeen)));
            m_table->setItem(row, COL_HITS, numberItem(peer.hits));

            for (int col = 0; col < COL_COUNT; ++col)
            {
                if (QTableWidgetItem *item = m_table->item(row, col))
                    item->setForeground(blockedColor);
            }
            ++row;
        }

        m_table->setSortingEnabled(true);
        m_table->resizeColumnsToContents();

        m_summary->setText(tr("%1 peers: %2 connected, %3 blocked (blocked peers are kept for 7 days after they were last refused)")
            .arg(connected.size() + blocked.size()).arg(connected.size()).arg(blocked.size()));
    });
}

void PeerListDialog::copyAll() const
{
    QStringList lines;

    QStringList header;
    for (int col = 0; col < m_table->columnCount(); ++col)
        header << m_table->horizontalHeaderItem(col)->text();
    lines << header.join(u'\t');

    for (int row = 0; row < m_table->rowCount(); ++row)
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
