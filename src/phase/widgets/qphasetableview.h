// Copyright (C) 2026 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only

#ifndef QPHASETABLEVIEW_H
#define QPHASETABLEVIEW_H

#include <QtPhase/qtphaseglobal.h>
#include <QtPhase/qphaseview.h>
#include <QtCore/qabstractitemmodel.h>

#include <functional>
#include <memory>
#include <vector>

QT_BEGIN_NAMESPACE

class QPhaseTableViewPrivate;

class Q_PHASE_EXPORT QPhaseTableView : public QPhaseView
{
public:
    explicit QPhaseTableView(QPhaseView *parent = nullptr);
    ~QPhaseTableView() override;

    void setModel(QAbstractItemModel *model);
    QAbstractItemModel *model() const noexcept;

    void setColumnWidths(const std::vector<qreal> &widths);
    void setRowHeight(qreal heightPx);
    qreal rowHeight() const noexcept;

    void scrollToY(qreal contentY);
    qreal scrollY() const noexcept;

    int selectedRow() const noexcept;
    void setSelectedRow(int row);

protected:
    void describeVisuals(QPhaseVisuals &visuals) override;
    void layoutChildren() override;
    bool onWheel(QWheelEvent *event) override;
    bool onMousePress(QMouseEvent *event) override;
    bool onKeyPress(QKeyEvent *event) override;

private:
    void ensureRowPoolSize();
    void syncVisibleRows(bool forceRefresh);
    void rebuildHeader();

    std::unique_ptr<QPhaseTableViewPrivate> d_table;
};

QT_END_NAMESPACE

#endif // QPHASETABLEVIEW_H
