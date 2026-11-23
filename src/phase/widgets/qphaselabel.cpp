// Copyright (C) 2026 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only

#include "qphaselabel.h"

#include <QtGui/qfontmetrics.h>
#include <cfloat>

QT_BEGIN_NAMESPACE

QPhaseLabel::QPhaseLabel(QPhaseView *parent)
    : QPhaseView(parent)
{
    m_font.setPixelSize(14);
}

QPhaseLabel::QPhaseLabel(const QString &text, QPhaseView *parent)
    : QPhaseView(parent)
    , m_text(text)
{
    m_font.setPixelSize(14);
}

void QPhaseLabel::setText(const QString &text)
{
    if (m_text == text)
        return;
    m_text = text;
    markVisualsDirty();
    if (parentView())
        parentView()->markLayoutDirty();
}

void QPhaseLabel::setFont(const QFont &font)
{
    m_font = font;
    markVisualsDirty();
    if (parentView())
        parentView()->markLayoutDirty();
}

void QPhaseLabel::setTextColor(const QColor &color)
{
    if (m_color != color) {
        m_color = color;
        markVisualsDirty();
    }
}

void QPhaseLabel::setAlignment(Qt::Alignment alignment)
{
    if (m_alignment != alignment) {
        m_alignment = alignment;
        markVisualsDirty();
    }
}

QSizeF QPhaseLabel::sizeHint(Qt::SizeHint which, const QSizeF &constraint) const
{
    const QSizeF base = QPhaseView::sizeHint(which, constraint);
    if (which == Qt::MaximumSize)
        return base;
    if (which == Qt::PreferredSize && (base.width() > 0.0 || base.height() > 0.0))
        return base;

    QFont unhinted = m_font;
    unhinted.setHintingPreference(QFont::PreferNoHinting);
    QFontMetricsF fm(unhinted);
    const QSizeF textSz = fm.size(Qt::TextSingleLine, m_text.isEmpty() ? QStringLiteral(" ") : m_text);
    if (which == Qt::MinimumSize)
        return QSizeF(std::max(base.width(), 0.0), std::max(base.height(), textSz.height() + 4.0));
    return QSizeF(textSz.width() + 4.0, textSz.height() + 4.0);
}

void QPhaseLabel::describeVisuals(QPhaseVisuals &visuals)
{
    if (!m_text.isEmpty())
        visuals.text(m_text, m_font, localRect(), m_alignment, m_color);
}

QT_END_NAMESPACE
