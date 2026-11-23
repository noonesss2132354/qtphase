// Copyright (C) 2026 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only

#include "qphasetableview.h"

#include <QtCore/qobject.h>
#include <QtCore/qpointer.h>
#include <QtGui/qevent.h>
#include <QtGui/qfont.h>

#include <algorithm>
#include <cmath>

QT_BEGIN_NAMESPACE

namespace {

static constexpr qreal kHeaderHeight = 38.0;

class TableHeaderView final : public QPhaseView
{
public:
    QAbstractItemModel *model = nullptr;
    std::vector<qreal> colWidths;
    QFont font;

    TableHeaderView()
    {
        font.setPixelSize(13);
        font.setWeight(QFont::DemiBold);
    }

protected:
    void describeVisuals(QPhaseVisuals &v) override
    {
        v.roundedBox(localRect(), {
            .radius = {10.0f, 10.0f, 0.0f, 0.0f},
            .fill = QPhaseFill::linearGradient({0, 0}, {0, kHeaderHeight}, QColor(36, 42, 56), QColor(29, 34, 46)),
            .border = { .width = 1.0f, .color = QColor(255, 255, 255, 24) },
            .shadow = { .blurRadius = 10.0f, .offset = {0, 4}, .color = QColor(0, 0, 0, 95) }
        });

        if (!model)
            return;
        const int cols = model->columnCount();
        qreal x = 14.0;
        for (int c = 0; c < cols; ++c) {
            const qreal w = (size_t(c) < colWidths.size()) ? colWidths[size_t(c)] : 140.0;
            const QString hdr = model->headerData(c, Qt::Horizontal, Qt::DisplayRole).toString();
            v.text(hdr, font, QRectF(x, 0.0, w - 12.0, height()),
                   Qt::AlignLeft | Qt::AlignVCenter, QColor(160, 174, 198));
            x += w;
        }
    }
};

class PooledTableRowView final : public QPhaseView
{
public:
    int boundModelRow = -1;
    bool isSelected = false;
    QStringList cellTexts;
    std::vector<qreal> colWidths;
    QFont font;

    PooledTableRowView()
    {
        font.setPixelSize(13);
    }

protected:
    void describeVisuals(QPhaseVisuals &v) override
    {
        if (boundModelRow < 0)
            return;

        const QColor bg = isSelected
                ? QColor(49, 130, 206, 95)
                : ((boundModelRow % 2 == 0) ? QColor(27, 31, 42) : QColor(23, 27, 37));
        v.solidRect(localRect(), bg);

        qreal x = 14.0;
        for (qsizetype c = 0; c < cellTexts.size(); ++c) {
            const qreal w = (size_t(c) < colWidths.size()) ? colWidths[size_t(c)] : 140.0;
            const QColor fg = isSelected ? Qt::white : ((c == 0) ? QColor(237, 242, 247) : QColor(186, 198, 218));
            v.text(cellTexts[c], font, QRectF(x, 0.0, w - 12.0, height()),
                   Qt::AlignLeft | Qt::AlignVCenter, fg);
            x += w;
        }
    }
};

} // namespace

class QPhaseTableViewPrivate
{
public:
    QPointer<QAbstractItemModel> model;
    std::vector<QMetaObject::Connection> modelConnections;
    std::vector<qreal> colWidths;
    qreal rowHeight = 32.0;
    qreal scrollY = 0.0;
    int selectedRow = -1;
    int firstWindowRow = -1;

    QPhaseView *bodyViewport = nullptr;
    QPhaseView *rowScrollContainer = nullptr;
    TableHeaderView *headerView = nullptr;
    QPhaseBox *scrollThumb = nullptr;
    std::vector<PooledTableRowView *> rowPool;
};

QPhaseTableView::QPhaseTableView(QPhaseView *parent)
    : QPhaseView(parent)
    , d_table(std::make_unique<QPhaseTableViewPrivate>())
{
    setFocusable(true);
    setClipsToBounds(true, QPhaseCorners::all(10.0f));

    // Clipped body viewport holding the scrolled row pool
    d_table->bodyViewport = add<QPhaseView>();
    d_table->bodyViewport->setClipsToBounds(true);

    d_table->rowScrollContainer = d_table->bodyViewport->add<QPhaseView>();

    // Sticky header drawn above scrolled rows with analytical GPU drop shadow
    d_table->headerView = add<TableHeaderView>();

    // Lightweight GPU scrollbar thumb
    d_table->scrollThumb = add<QPhaseBox>();
    d_table->scrollThumb->setBoxStyle({
        .radius = QPhaseCorners::all(3.0f),
        .fill = QPhaseFill::solid(QColor(255, 255, 255, 70))
    });
}

QPhaseTableView::~QPhaseTableView()
{
    for (const auto &conn : d_table->modelConnections)
        QObject::disconnect(conn);
}

void QPhaseTableView::setModel(QAbstractItemModel *model)
{
    if (d_table->model == model)
        return;

    for (const auto &conn : d_table->modelConnections)
        QObject::disconnect(conn);
    d_table->modelConnections.clear();

    d_table->model = model;
    d_table->scrollY = 0.0;
    d_table->firstWindowRow = -1;

    if (model) {
        auto refresh = [this]() {
            d_table->firstWindowRow = -1;
            rebuildHeader();
            syncVisibleRows(true);
        };
        d_table->modelConnections.push_back(QObject::connect(model, &QAbstractItemModel::modelReset, model, refresh));
        d_table->modelConnections.push_back(QObject::connect(model, &QAbstractItemModel::layoutChanged, model, refresh));
        d_table->modelConnections.push_back(QObject::connect(model, &QAbstractItemModel::rowsInserted, model, refresh));
        d_table->modelConnections.push_back(QObject::connect(model, &QAbstractItemModel::rowsRemoved, model, refresh));
        d_table->modelConnections.push_back(QObject::connect(model, &QAbstractItemModel::dataChanged, model, refresh));
    }

    rebuildHeader();
    syncVisibleRows(true);
}

QAbstractItemModel *QPhaseTableView::model() const noexcept
{
    return d_table->model.data();
}

void QPhaseTableView::setColumnWidths(const std::vector<qreal> &widths)
{
    d_table->colWidths = widths;
    rebuildHeader();
    syncVisibleRows(true);
}

void QPhaseTableView::setRowHeight(qreal heightPx)
{
    d_table->rowHeight = std::max(18.0, heightPx);
    ensureRowPoolSize();
    syncVisibleRows(true);
}

qreal QPhaseTableView::rowHeight() const noexcept
{
    return d_table->rowHeight;
}

qreal QPhaseTableView::scrollY() const noexcept
{
    return d_table->scrollY;
}

int QPhaseTableView::selectedRow() const noexcept
{
    return d_table->selectedRow;
}

void QPhaseTableView::setSelectedRow(int row)
{
    if (d_table->selectedRow == row)
        return;
    const int oldSel = d_table->selectedRow;
    d_table->selectedRow = row;
    for (PooledTableRowView *slot : d_table->rowPool) {
        if (slot->boundModelRow == oldSel || slot->boundModelRow == row) {
            slot->isSelected = (slot->boundModelRow == row);
            slot->markVisualsDirty();
        }
    }
}

void QPhaseTableView::describeVisuals(QPhaseVisuals &visuals)
{
    visuals.roundedBox(localRect(), {
        .radius = QPhaseCorners::all(10.0f),
        .fill = QPhaseFill::solid(QColor(22, 26, 36)),
        .border = { .width = 1.0f, .color = QColor(255, 255, 255, 32) },
        .shadow = { .blurRadius = 16.0f, .offset = {0, 6}, .color = QColor(0, 0, 0, 120) }
    });
}

void QPhaseTableView::layoutChildren()
{
    const qreal w = geometry().width();
    const qreal h = geometry().height();
    const qreal bodyH = std::max(0.0, h - kHeaderHeight);

    d_table->headerView->setGeometry(QRectF(0.0, 0.0, w, kHeaderHeight));
    d_table->bodyViewport->setGeometry(QRectF(0.0, kHeaderHeight, w, bodyH));
    d_table->rowScrollContainer->setGeometry(QRectF(0.0, 0.0, w, bodyH));

    rebuildHeader();
    ensureRowPoolSize();
    scrollToY(d_table->scrollY);
    syncVisibleRows(true);
}

void QPhaseTableView::rebuildHeader()
{
    d_table->headerView->model = d_table->model.data();
    d_table->headerView->colWidths = d_table->colWidths;
    d_table->headerView->markVisualsDirty();
}

void QPhaseTableView::ensureRowPoolSize()
{
    const qreal bodyH = std::max(0.0, geometry().height() - kHeaderHeight);
    const size_t needed = size_t(std::ceil(bodyH / d_table->rowHeight)) + 4;
    while (d_table->rowPool.size() < needed) {
        auto *rowView = d_table->rowScrollContainer->add<PooledTableRowView>();
        d_table->rowPool.push_back(rowView);
    }
}

void QPhaseTableView::scrollToY(qreal contentY)
{
    const int totalRows = d_table->model ? d_table->model->rowCount() : 0;
    const qreal bodyH = std::max(0.0, geometry().height() - kHeaderHeight);
    const qreal totalContentH = qreal(totalRows) * d_table->rowHeight;
    const qreal maxScroll = std::max(0.0, totalContentH - bodyH);

    d_table->scrollY = std::clamp(contentY, 0.0, maxScroll);

    // O(1) GPU TransformSlot update on rowScrollContainer:
    // Subpixel scrolling within the current row window NEVER touches cell text or re-shapes glyphs!
    d_table->rowScrollContainer->setScrollOffset(QPointF(0.0, -d_table->scrollY));

    syncVisibleRows(false);

    // Update scrollbar thumb position/height
    if (totalContentH > bodyH && bodyH > 16.0) {
        const qreal thumbH = std::max(24.0, (bodyH / totalContentH) * (bodyH - 8.0));
        const qreal frac = (maxScroll > 0.0) ? (d_table->scrollY / maxScroll) : 0.0;
        const qreal thumbY = kHeaderHeight + 4.0 + frac * (bodyH - 8.0 - thumbH);
        d_table->scrollThumb->setGeometry(QRectF(geometry().width() - 9.0, thumbY, 5.0, thumbH));
        d_table->scrollThumb->setVisible(true);
    } else {
        d_table->scrollThumb->setVisible(false);
    }
}

void QPhaseTableView::syncVisibleRows(bool forceRefresh)
{
    const int totalRows = d_table->model ? d_table->model->rowCount() : 0;
    const int cols = d_table->model ? d_table->model->columnCount() : 0;
    const int newFirstRow = std::max(0, int(std::floor(d_table->scrollY / d_table->rowHeight)));

    if (!forceRefresh && newFirstRow == d_table->firstWindowRow)
        return;

    d_table->firstWindowRow = newFirstRow;
    const int poolSize = int(d_table->rowPool.size());
    if (poolSize == 0)
        return;

    const qreal rowW = geometry().width();

    for (int i = 0; i < poolSize; ++i) {
        const int modelRow = newFirstRow + i;
        // Ring-index into rowPool so scrolling by 1 row only recycles 1 PooledTableRowView!
        PooledTableRowView *slot = d_table->rowPool[size_t(modelRow % poolSize)];

        if (modelRow >= totalRows) {
            slot->boundModelRow = -1;
            slot->setVisible(false);
            continue;
        }

        slot->setVisible(true);
        slot->setGeometry(QRectF(0.0, qreal(modelRow) * d_table->rowHeight, rowW, d_table->rowHeight));

        if (forceRefresh || slot->boundModelRow != modelRow || slot->colWidths != d_table->colWidths) {
            slot->boundModelRow = modelRow;
            slot->isSelected = (modelRow == d_table->selectedRow);
            slot->colWidths = d_table->colWidths;
            slot->cellTexts.clear();
            slot->cellTexts.reserve(cols);
            for (int c = 0; c < cols; ++c) {
                const QModelIndex idx = d_table->model->index(modelRow, c);
                slot->cellTexts.push_back(d_table->model->data(idx, Qt::DisplayRole).toString());
            }
            slot->markVisualsDirty();
        }
    }
}

bool QPhaseTableView::onWheel(QWheelEvent *event)
{
    qreal dy = 0.0;
    if (!event->pixelDelta().isNull())
        dy = -event->pixelDelta().y();
    else
        dy = -(event->angleDelta().y() / 120.0) * (d_table->rowHeight * 2.5);

    scrollToY(d_table->scrollY + dy);
    return true;
}

bool QPhaseTableView::onMousePress(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton || event->position().y() < kHeaderHeight || !d_table->model)
        return false;

    const qreal contentClickY = (event->position().y() - kHeaderHeight) + d_table->scrollY;
    const int clickedRow = int(std::floor(contentClickY / d_table->rowHeight));
    if (clickedRow >= 0 && clickedRow < d_table->model->rowCount())
        setSelectedRow(clickedRow);
    return true;
}

bool QPhaseTableView::onKeyPress(QKeyEvent *event)
{
    if (!d_table->model || d_table->model->rowCount() == 0)
        return false;

    const int count = d_table->model->rowCount();
    const qreal bodyH = std::max(d_table->rowHeight, geometry().height() - kHeaderHeight);

    if (event->key() == Qt::Key_Down) {
        const int next = std::clamp(d_table->selectedRow + 1, 0, count - 1);
        setSelectedRow(next);
        if ((next + 1) * d_table->rowHeight > d_table->scrollY + bodyH)
            scrollToY((next + 1) * d_table->rowHeight - bodyH);
        return true;
    }
    if (event->key() == Qt::Key_Up) {
        const int prev = std::clamp(d_table->selectedRow - 1, 0, count - 1);
        setSelectedRow(prev);
        if (prev * d_table->rowHeight < d_table->scrollY)
            scrollToY(prev * d_table->rowHeight);
        return true;
    }
    if (event->key() == Qt::Key_PageDown) {
        scrollToY(d_table->scrollY + bodyH * 0.9);
        return true;
    }
    if (event->key() == Qt::Key_PageUp) {
        scrollToY(d_table->scrollY - bodyH * 0.9);
        return true;
    }
    return false;
}

QT_END_NAMESPACE
