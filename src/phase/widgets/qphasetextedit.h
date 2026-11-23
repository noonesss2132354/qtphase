// Copyright (C) 2026 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only

#ifndef QPHASETEXTEDIT_H
#define QPHASETEXTEDIT_H

#include <QtPhase/qtphaseglobal.h>
#include <QtPhase/qphaseview.h>
#include <QtCore/qstring.h>
#include <QtGui/qcolor.h>
#include <QtGui/qfont.h>

#include <functional>
#include <memory>

QT_BEGIN_NAMESPACE

class QPhaseTextEditPrivate;

class Q_PHASE_EXPORT QPhaseTextEdit : public QPhaseView
{
public:
    explicit QPhaseTextEdit(QPhaseView *parent = nullptr);
    ~QPhaseTextEdit() override;

    void setText(const QString &text);
    QString text() const;

    void setPlaceholderText(const QString &placeholder);
    QString placeholderText() const;

    void setFont(const QFont &font);
    QFont font() const;

    void setOnTextChanged(std::function<void(const QString &)> callback);

    QSizeF sizeHint(Qt::SizeHint which, const QSizeF &constraint = {}) const override;

protected:
    void describeVisuals(QPhaseVisuals &visuals) override;
    void layoutChildren() override;
    void onFocusChanged(bool focused) override;

    bool onMousePress(QMouseEvent *event) override;
    bool onMouseMove(QMouseEvent *event) override;
    bool onMouseRelease(QMouseEvent *event) override;
    bool onKeyPress(QKeyEvent *event) override;
    void onInputMethod(QInputMethodEvent *event) override;
    QVariant inputMethodQuery(Qt::InputMethodQuery query) const override;

private:
    void rebuildTextLayout();
    void updateSelectionAndCaretNodes();
    void commitEdit(const QString &newText, int newCursorPos);

    std::unique_ptr<QPhaseTextEditPrivate> d_edit;
};

QT_END_NAMESPACE

#endif // QPHASETEXTEDIT_H
