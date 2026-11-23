// Copyright (C) 2026 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only

#include "qphasetextedit.h"

#include <QtCore/qvariant.h>
#include <QtGui/qclipboard.h>
#include <QtGui/qevent.h>
#include <QtGui/qfontmetrics.h>
#include <QtGui/qguiapplication.h>
#include <QtGui/qtextlayout.h>
#include <QtGui/qundostack.h>

#include <algorithm>

QT_BEGIN_NAMESPACE

namespace {

class TextEditUndoCmd final : public QUndoCommand
{
public:
    TextEditUndoCmd(std::function<void(const QString &, int)> applyFn,
                    QString beforeText, int beforeCursor,
                    QString afterText, int afterCursor)
        : m_apply(std::move(applyFn))
        , m_beforeText(std::move(beforeText))
        , m_beforeCursor(beforeCursor)
        , m_afterText(std::move(afterText))
        , m_afterCursor(afterCursor)
    {
    }

    void undo() override { m_apply(m_beforeText, m_beforeCursor); }
    void redo() override
    {
        if (m_firstRedo) {
            m_firstRedo = false;
            return;
        }
        m_apply(m_afterText, m_afterCursor);
    }

private:
    std::function<void(const QString &, int)> m_apply;
    QString m_beforeText;
    int m_beforeCursor = 0;
    QString m_afterText;
    int m_afterCursor = 0;
    bool m_firstRedo = true;
};

// Dedicated text layer child so selection/caret changes NEVER re-shape or invalidate glyph curves!
class ParagraphGlyphView final : public QPhaseView
{
public:
    QList<QGlyphRun> runs;
    QPointF origin{12.0, 0.0};
    QColor color{Qt::white};

protected:
    void describeVisuals(QPhaseVisuals &visuals) override
    {
        for (const QGlyphRun &r : runs)
            visuals.glyphRun(r, origin, color);
    }
};

} // namespace

class QPhaseTextEditPrivate
{
public:
    QString text;
    QString preeditText;
    QString placeholder;
    QFont font;
    QTextLayout layout;
    QUndoStack undoStack;
    std::function<void(const QString &)> onChanged;

    int cursorPos = 0;
    int anchorPos = 0;
    bool dragging = false;

    QPhaseBox *selectionBox = nullptr;
    ParagraphGlyphView *glyphLayer = nullptr;
    QPhaseBox *caretBox = nullptr;

    static constexpr qreal kPadX = 12.0;
};

QPhaseTextEdit::QPhaseTextEdit(QPhaseView *parent)
    : QPhaseView(parent)
    , d_edit(std::make_unique<QPhaseTextEditPrivate>())
{
    setFocusable(true);
    setClipsToBounds(true, QPhaseCorners::all(8.0f));
    d_edit->font.setPixelSize(14);
    d_edit->font.setHintingPreference(QFont::PreferNoHinting);

    // 1. Selection quad layer (mutates bounds in O(1) without touching text!)
    d_edit->selectionBox = add<QPhaseBox>();
    d_edit->selectionBox->setBoxStyle({
        .radius = QPhaseCorners::all(3.0f),
        .fill = QPhaseFill::solid(QColor(66, 153, 225, 110))
    });
    d_edit->selectionBox->setVisible(false);

    // 2. Shaped hb-gpu glyph layer
    d_edit->glyphLayer = add<ParagraphGlyphView>();

    // 3. GPU-pulsed blinking caret (CosinePulseInfinite runs 100% in Vulkan shader with 0% CPU!)
    d_edit->caretBox = add<QPhaseBox>();
    d_edit->caretBox->setColor(QColor(99, 179, 237));
    d_edit->caretBox->setAnimation({
        .mode = QPhaseAnimation::Mode::CosinePulseInfinite,
        .fromOpacity = 1.0f,
        .toOpacity = 0.12f,
        .durationMs = 1050
    });
    d_edit->caretBox->setVisible(false);
}

QPhaseTextEdit::~QPhaseTextEdit() = default;

void QPhaseTextEdit::setText(const QString &text)
{
    if (d_edit->text == text)
        return;
    d_edit->text = text;
    d_edit->cursorPos = int(text.size());
    d_edit->anchorPos = d_edit->cursorPos;
    d_edit->undoStack.clear();
    rebuildTextLayout();
    markVisualsDirty();
}

QString QPhaseTextEdit::text() const
{
    return d_edit->text;
}

void QPhaseTextEdit::setPlaceholderText(const QString &placeholder)
{
    d_edit->placeholder = placeholder;
    if (d_edit->text.isEmpty())
        rebuildTextLayout();
}

QString QPhaseTextEdit::placeholderText() const
{
    return d_edit->placeholder;
}

void QPhaseTextEdit::setFont(const QFont &font)
{
    d_edit->font = font;
    d_edit->font.setHintingPreference(QFont::PreferNoHinting);
    rebuildTextLayout();
}

QFont QPhaseTextEdit::font() const
{
    return d_edit->font;
}

void QPhaseTextEdit::setOnTextChanged(std::function<void(const QString &)> callback)
{
    d_edit->onChanged = std::move(callback);
}

QSizeF QPhaseTextEdit::sizeHint(Qt::SizeHint which, const QSizeF &constraint) const
{
    const QSizeF base = QPhaseView::sizeHint(which, constraint);
    if (which == Qt::MaximumSize)
        return QSizeF(base.width(), 40.0);
    return QSizeF(std::max(base.width(), 160.0), 38.0);
}

void QPhaseTextEdit::describeVisuals(QPhaseVisuals &visuals)
{
    const QColor borderCol = hasFocus() ? QColor(99, 179, 237, 220) : QColor(255, 255, 255, 36);
    visuals.roundedBox(localRect(), {
        .radius = QPhaseCorners::all(8.0f),
        .fill = QPhaseFill::solid(QColor(24, 28, 38)),
        .border = { .width = hasFocus() ? 1.5f : 1.0f, .color = borderCol }
    });
}

void QPhaseTextEdit::layoutChildren()
{
    d_edit->glyphLayer->setGeometry(localRect());
    rebuildTextLayout();
}

void QPhaseTextEdit::onFocusChanged(bool focused)
{
    d_edit->caretBox->setVisible(focused);
    updateSelectionAndCaretNodes();
}

void QPhaseTextEdit::rebuildTextLayout()
{
    const bool showingPlaceholder = d_edit->text.isEmpty() && d_edit->preeditText.isEmpty();
    QString displayStr = showingPlaceholder ? d_edit->placeholder : d_edit->text;
    if (!d_edit->preeditText.isEmpty())
        displayStr.insert(d_edit->cursorPos, d_edit->preeditText);

    d_edit->layout.setFont(d_edit->font);
    d_edit->layout.setText(displayStr);
    QTextOption opt(Qt::AlignLeft | Qt::AlignVCenter);
    opt.setWrapMode(QTextOption::NoWrap);
    d_edit->layout.setTextOption(opt);

    d_edit->layout.beginLayout();
    QTextLine line = d_edit->layout.createLine();
    if (line.isValid()) {
        line.setLineWidth(std::max(100.0, geometry().width() - QPhaseTextEditPrivate::kPadX * 2.0));
        line.setPosition(QPointF(0.0, 0.0));
    }
    d_edit->layout.endLayout();

    const qreal lineH = (d_edit->layout.lineCount() > 0) ? d_edit->layout.lineAt(0).height() : 18.0;
    const qreal topY = std::max(0.0, (geometry().height() - lineH) * 0.5);

    d_edit->glyphLayer->origin = QPointF(QPhaseTextEditPrivate::kPadX, topY);
    d_edit->glyphLayer->color = showingPlaceholder ? QColor(130, 142, 165) : QColor(237, 242, 247);
    d_edit->glyphLayer->runs = displayStr.isEmpty()
            ? QList<QGlyphRun>{}
            : d_edit->layout.glyphRuns();
    d_edit->glyphLayer->markVisualsDirty();

    updateSelectionAndCaretNodes();
}

void QPhaseTextEdit::updateSelectionAndCaretNodes()
{
    if (d_edit->layout.lineCount() == 0)
        return;

    const QTextLine line = d_edit->layout.lineAt(0);
    const qreal lineH = line.height();
    const qreal topY = std::max(4.0, (geometry().height() - lineH) * 0.5);
    const bool emptyDoc = d_edit->text.isEmpty() && d_edit->preeditText.isEmpty();

    const int effCursor = emptyDoc ? 0 : std::clamp(d_edit->cursorPos + int(d_edit->preeditText.size()), 0, int(d_edit->layout.text().size()));
    const qreal cursorX = QPhaseTextEditPrivate::kPadX + (emptyDoc ? 0.0 : line.cursorToX(effCursor));

    d_edit->caretBox->setGeometry(QRectF(cursorX, topY, 2.0, lineH));
    d_edit->caretBox->setVisible(hasFocus());

    if (!emptyDoc && d_edit->cursorPos != d_edit->anchorPos) {
        const int s0 = std::min(d_edit->cursorPos, d_edit->anchorPos);
        const int s1 = std::max(d_edit->cursorPos, d_edit->anchorPos);
        const qreal x0 = QPhaseTextEditPrivate::kPadX + line.cursorToX(s0);
        const qreal x1 = QPhaseTextEditPrivate::kPadX + line.cursorToX(s1);
        d_edit->selectionBox->setGeometry(QRectF(std::min(x0, x1), topY, std::max(2.0, std::abs(x1 - x0)), lineH));
        d_edit->selectionBox->setVisible(true);
    } else {
        d_edit->selectionBox->setVisible(false);
    }
}

void QPhaseTextEdit::commitEdit(const QString &newText, int newCursorPos)
{
    const QString prevText = d_edit->text;
    const int prevCur = d_edit->cursorPos;
    if (prevText == newText) {
        d_edit->cursorPos = d_edit->anchorPos = newCursorPos;
        updateSelectionAndCaretNodes();
        return;
    }

    auto applyState = [this](const QString &txt, int cur) {
        d_edit->text = txt;
        d_edit->cursorPos = std::clamp(cur, 0, int(txt.size()));
        d_edit->anchorPos = d_edit->cursorPos;
        rebuildTextLayout();
        if (d_edit->onChanged)
            d_edit->onChanged(d_edit->text);
    };

    applyState(newText, newCursorPos);
    d_edit->undoStack.push(new TextEditUndoCmd(applyState, prevText, prevCur, d_edit->text, d_edit->cursorPos));
}

bool QPhaseTextEdit::onMousePress(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton)
        return false;

    if (d_edit->text.isEmpty() || d_edit->layout.lineCount() == 0) {
        d_edit->cursorPos = d_edit->anchorPos = 0;
    } else {
        const QTextLine line = d_edit->layout.lineAt(0);
        const int pos = line.xToCursor(event->position().x() - QPhaseTextEditPrivate::kPadX);
        d_edit->cursorPos = pos;
        if (!(event->modifiers() & Qt::ShiftModifier))
            d_edit->anchorPos = pos;
    }
    d_edit->dragging = true;
    updateSelectionAndCaretNodes();
    return true;
}

bool QPhaseTextEdit::onMouseMove(QMouseEvent *event)
{
    if (!d_edit->dragging || d_edit->text.isEmpty() || d_edit->layout.lineCount() == 0)
        return false;

    const QTextLine line = d_edit->layout.lineAt(0);
    d_edit->cursorPos = line.xToCursor(event->position().x() - QPhaseTextEditPrivate::kPadX);
    updateSelectionAndCaretNodes();
    return true;
}

bool QPhaseTextEdit::onMouseRelease(QMouseEvent *)
{
    d_edit->dragging = false;
    return true;
}

bool QPhaseTextEdit::onKeyPress(QKeyEvent *event)
{
    const bool shift = (event->modifiers() & Qt::ShiftModifier);
    const bool ctrl = (event->modifiers() & Qt::ControlModifier);
    const bool hasSel = (d_edit->cursorPos != d_edit->anchorPos);
    const int selStart = std::min(d_edit->cursorPos, d_edit->anchorPos);
    const int selLen = std::abs(d_edit->cursorPos - d_edit->anchorPos);

    if (ctrl && event->key() == Qt::Key_Z) {
        if (shift)
            d_edit->undoStack.redo();
        else
            d_edit->undoStack.undo();
        return true;
    }
    if (ctrl && event->key() == Qt::Key_Y) {
        d_edit->undoStack.redo();
        return true;
    }
    if (ctrl && event->key() == Qt::Key_A) {
        d_edit->anchorPos = 0;
        d_edit->cursorPos = int(d_edit->text.size());
        updateSelectionAndCaretNodes();
        return true;
    }
    if (ctrl && event->key() == Qt::Key_C) {
        if (hasSel)
            QGuiApplication::clipboard()->setText(d_edit->text.mid(selStart, selLen));
        return true;
    }
    if (ctrl && event->key() == Qt::Key_V) {
        const QString clip = QGuiApplication::clipboard()->text();
        if (!clip.isEmpty()) {
            QString next = d_edit->text;
            if (hasSel)
                next.remove(selStart, selLen);
            const int insAt = hasSel ? selStart : d_edit->cursorPos;
            next.insert(insAt, clip);
            commitEdit(next, insAt + int(clip.size()));
        }
        return true;
    }

    switch (event->key()) {
    case Qt::Key_Left: {
        int nextPos = hasSel && !shift ? selStart : d_edit->layout.previousCursorPosition(d_edit->cursorPos);
        d_edit->cursorPos = std::clamp(nextPos, 0, int(d_edit->text.size()));
        if (!shift)
            d_edit->anchorPos = d_edit->cursorPos;
        updateSelectionAndCaretNodes();
        return true;
    }
    case Qt::Key_Right: {
        int nextPos = hasSel && !shift ? (selStart + selLen) : d_edit->layout.nextCursorPosition(d_edit->cursorPos);
        d_edit->cursorPos = std::clamp(nextPos, 0, int(d_edit->text.size()));
        if (!shift)
            d_edit->anchorPos = d_edit->cursorPos;
        updateSelectionAndCaretNodes();
        return true;
    }
    case Qt::Key_Home:
        d_edit->cursorPos = 0;
        if (!shift)
            d_edit->anchorPos = 0;
        updateSelectionAndCaretNodes();
        return true;
    case Qt::Key_End:
        d_edit->cursorPos = int(d_edit->text.size());
        if (!shift)
            d_edit->anchorPos = d_edit->cursorPos;
        updateSelectionAndCaretNodes();
        return true;
    case Qt::Key_Backspace: {
        QString next = d_edit->text;
        if (hasSel) {
            next.remove(selStart, selLen);
            commitEdit(next, selStart);
        } else if (d_edit->cursorPos > 0) {
            const int prev = d_edit->layout.previousCursorPosition(d_edit->cursorPos);
            next.remove(prev, d_edit->cursorPos - prev);
            commitEdit(next, prev);
        }
        return true;
    }
    case Qt::Key_Delete: {
        QString next = d_edit->text;
        if (hasSel) {
            next.remove(selStart, selLen);
            commitEdit(next, selStart);
        } else if (d_edit->cursorPos < d_edit->text.size()) {
            const int nxt = d_edit->layout.nextCursorPosition(d_edit->cursorPos);
            next.remove(d_edit->cursorPos, nxt - d_edit->cursorPos);
            commitEdit(next, d_edit->cursorPos);
        }
        return true;
    }
    default:
        break;
    }

    const QString typed = event->text();
    if (!typed.isEmpty() && typed.at(0).isPrint()) {
        QString next = d_edit->text;
        const int insAt = hasSel ? selStart : d_edit->cursorPos;
        if (hasSel)
            next.remove(selStart, selLen);
        next.insert(insAt, typed);
        commitEdit(next, insAt + int(typed.size()));
        return true;
    }

    return false;
}

void QPhaseTextEdit::onInputMethod(QInputMethodEvent *event)
{
    QString next = d_edit->text;
    if (d_edit->cursorPos != d_edit->anchorPos) {
        const int s0 = std::min(d_edit->cursorPos, d_edit->anchorPos);
        next.remove(s0, std::abs(d_edit->cursorPos - d_edit->anchorPos));
        d_edit->cursorPos = d_edit->anchorPos = s0;
    }

    if (event->replacementLength() > 0) {
        const int repPos = std::clamp(d_edit->cursorPos + event->replacementStart(), 0, int(next.size()));
        next.remove(repPos, event->replacementLength());
        d_edit->cursorPos = repPos;
    }

    d_edit->preeditText = event->preeditString();
    if (!event->commitString().isEmpty()) {
        const int insAt = d_edit->cursorPos;
        next.insert(insAt, event->commitString());
        commitEdit(next, insAt + int(event->commitString().size()));
    } else {
        rebuildTextLayout();
    }
    event->accept();
}

QVariant QPhaseTextEdit::inputMethodQuery(Qt::InputMethodQuery query) const
{
    switch (query) {
    case Qt::ImEnabled:
        return true;
    case Qt::ImCursorRectangle:
        return d_edit->caretBox->geometry();
    case Qt::ImFont:
        return d_edit->font;
    case Qt::ImCursorPosition:
        return d_edit->cursorPos;
    case Qt::ImSurroundingText:
        return d_edit->text;
    case Qt::ImCurrentSelection: {
        const int s0 = std::min(d_edit->cursorPos, d_edit->anchorPos);
        return d_edit->text.mid(s0, std::abs(d_edit->cursorPos - d_edit->anchorPos));
    }
    default:
        return {};
    }
}

QT_END_NAMESPACE
