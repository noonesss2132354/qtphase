// Copyright (C) 2026 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only

#ifndef QPHASELABEL_H
#define QPHASELABEL_H

#include <QtPhase/qtphaseglobal.h>
#include <QtPhase/qphaseview.h>
#include <QtCore/qstring.h>
#include <QtGui/qcolor.h>
#include <QtGui/qfont.h>

QT_BEGIN_NAMESPACE

class Q_PHASE_EXPORT QPhaseLabel : public QPhaseView
{
public:
    explicit QPhaseLabel(QPhaseView *parent = nullptr);
    explicit QPhaseLabel(const QString &text, QPhaseView *parent = nullptr);

    void setText(const QString &text);
    const QString &text() const noexcept { return m_text; }

    void setFont(const QFont &font);
    const QFont &font() const noexcept { return m_font; }

    void setTextColor(const QColor &color);
    QColor textColor() const noexcept { return m_color; }

    void setAlignment(Qt::Alignment alignment);
    Qt::Alignment alignment() const noexcept { return m_alignment; }

    QSizeF sizeHint(Qt::SizeHint which, const QSizeF &constraint = {}) const override;

protected:
    void describeVisuals(QPhaseVisuals &visuals) override;

private:
    QString m_text;
    QFont m_font;
    QColor m_color{Qt::white};
    Qt::Alignment m_alignment = Qt::AlignLeft | Qt::AlignVCenter;
};

QT_END_NAMESPACE

#endif // QPHASELABEL_H
