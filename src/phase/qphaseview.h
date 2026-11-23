// Copyright (C) 2026 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only

#ifndef QPHASEVIEW_H
#define QPHASEVIEW_H

#include <QtPhase/qtphaseglobal.h>
#include <QtPhase/qphasestyles.h>
#include <QtPhase/qphasevisuals.h>
#include <QtCore/qmargins.h>
#include <QtCore/qnamespace.h>
#include <QtCore/qpoint.h>
#include <QtCore/qrect.h>
#include <QtCore/qstring.h>
#include <QtGui/qimage.h>

#include <functional>
#include <memory>
#include <utility>
#include <vector>

QT_BEGIN_NAMESPACE

class QMouseEvent;
class QWheelEvent;
class QKeyEvent;
class QInputMethodEvent;
class QPhaseViewPrivate;
class QPhaseWindow;
class QPhaseSceneArena;

class Q_PHASE_EXPORT QPhaseView
{
public:
    explicit QPhaseView(QPhaseView *parent = nullptr);
    virtual ~QPhaseView();

    Q_DISABLE_COPY_MOVE(QPhaseView)

    QPhaseView *parentView() const noexcept;
    QPhaseWindow *window() const noexcept;
    const std::vector<std::unique_ptr<QPhaseView>> &children() const noexcept;

    template <typename T, typename... Args>
    T *add(Args &&...args)
    {
        auto child = std::make_unique<T>(std::forward<Args>(args)...);
        T *raw = child.get();
        addChild(std::move(child));
        return raw;
    }

    void addChild(std::unique_ptr<QPhaseView> child);

    void setGeometry(const QRectF &geometry);
    QRectF geometry() const noexcept;
    qreal width() const noexcept { return geometry().width(); }
    qreal height() const noexcept { return geometry().height(); }
    QRectF localRect() const noexcept { return QRectF(0.0, 0.0, geometry().width(), geometry().height()); }
    QPointF mapFromWindow(QPointF windowPos) const noexcept;

    void setPosition(QPointF pos);
    void setSize(QSizeF size);

    // Layout constraints consumed by QGridLayoutEngine when parent has a box layout enabled
    void setMinimumSize(QSizeF minSize);
    void setPreferredSize(QSizeF prefSize);
    void setMaximumSize(QSizeF maxSize);
    void setStretch(int stretchFactor);
    int stretch() const noexcept;

    // Enables automatic QGridLayoutEngine layout of direct child views
    void setBoxLayout(Qt::Orientation orientation, QMarginsF padding = {}, qreal spacing = 8.0);

    // O(1) GPU TransformSlot scroll/translation/rotation/scale applied to this view
    void setScrollOffset(QPointF offset);
    QPointF scrollOffset() const noexcept;

    void setRotationDegrees(float degrees);
    float rotationDegrees() const noexcept;
    void setScale(float sx, float sy);

    void setOpacity(float opacity);
    float opacity() const noexcept;

    // Isolated group compositing & blend mode (uses zero-VRAM in-register PushLayer/PopLayer stack!)
    void setIsolatedGroup(bool isolated, QPhaseBlendMode blendMode = QPhaseBlendMode::Normal);
    bool isIsolatedGroup() const noexcept;
    QPhaseBlendMode blendMode() const noexcept;

    // Generalized renderer affinity & startup blocking policy:
    // - Any (default): renders on both CPU & GPU; uses normal 5-frame competitive worker race
    // - GpuOnly: forces GPU Vulkan renderer; if blocking==false, CPU renders everything else until GPU is ready
    // - CpuOnly: forces CPU tile-pool renderer; immediately shuts down GPU worker & frees VRAM
    void setRendererAffinity(QPhaseRendererAffinity affinity, bool blocking = false);
    QPhaseRendererAffinity rendererAffinity() const noexcept;
    void setBackendBlocking(bool blocking);
    bool isBackendBlocking() const noexcept;

    void setVisible(bool visible);
    bool isVisible() const noexcept;

    void setClipsToBounds(bool clip, QPhaseCorners corners = {});
    bool clipsToBounds() const noexcept;

    void setFocusable(bool focusable);
    bool isFocusable() const noexcept;
    bool hasFocus() const noexcept;
    void requestFocus();

    virtual QSizeF sizeHint(Qt::SizeHint which, const QSizeF &constraint = {}) const;

    void markVisualsDirty();
    void markLayoutDirty();

protected:
    virtual void describeVisuals(QPhaseVisuals &visuals);
    virtual void layoutChildren();
    virtual void onFocusChanged(bool focused) { Q_UNUSED(focused); }

    virtual bool onMousePress(QMouseEvent *event) { Q_UNUSED(event); return false; }
    virtual bool onMouseMove(QMouseEvent *event) { Q_UNUSED(event); return false; }
    virtual bool onMouseRelease(QMouseEvent *event) { Q_UNUSED(event); return false; }
    virtual bool onWheel(QWheelEvent *event) { Q_UNUSED(event); return false; }
    virtual bool onKeyPress(QKeyEvent *event) { Q_UNUSED(event); return false; }
    virtual void onInputMethod(QInputMethodEvent *event) { Q_UNUSED(event); }
    virtual QVariant inputMethodQuery(Qt::InputMethodQuery query) const;

private:
    friend class QPhaseWindow;
    friend class QPhaseWindowPrivate;
    friend class QPhaseImeFocusProxy;
    friend class QPhaseSceneArena;
    friend class QPhaseGpuView;
    friend void qt_phase_set_focused_view(QPhaseWindow *window, QPhaseView *view);

    void setOwningWindow(QPhaseWindow *window);
    void setFocusedInternal(bool focused);
    void syncToArena(QPhaseSceneArena &arena, uint32_t parentTx, uint32_t parentClip,
                     std::vector<uint32_t> &outDrawOrder, bool &drawOrderChanged,
                     QPhaseRendererAffinity inheritedAffinity = QPhaseRendererAffinity::Any,
                     bool inheritedBlocking = false);
    void detachFromArena();

    QPhaseView *hitTestMousePress(QPointF parentLocalPos, QMouseEvent *origEvent);
    bool dispatchWheel(QPointF parentLocalPos, QWheelEvent *origEvent);

    std::unique_ptr<QPhaseViewPrivate> d;
};

class Q_PHASE_EXPORT QPhaseBox : public QPhaseView
{
public:
    explicit QPhaseBox(QPhaseView *parent = nullptr);

    void setColor(const QColor &color);
    void setBoxStyle(const QPhaseBoxStyle &style);
    const QPhaseBoxStyle &boxStyle() const noexcept { return m_style; }

    void setAnimation(const QPhaseAnimation &anim);
    const QPhaseAnimation &animation() const noexcept { return m_anim; }

protected:
    void describeVisuals(QPhaseVisuals &visuals) override;

private:
    QPhaseBoxStyle m_style{};
    QPhaseAnimation m_anim{};
};

class Q_PHASE_EXPORT QPhaseImage : public QPhaseView
{
public:
    explicit QPhaseImage(QPhaseView *parent = nullptr);
    explicit QPhaseImage(const QImage &image, QPhaseView *parent = nullptr);

    void setImage(const QImage &image);
    void setCornerRadius(QPhaseCorners corners);

protected:
    void describeVisuals(QPhaseVisuals &visuals) override;

private:
    QImage m_image;
    QPhaseCorners m_corners{};
};

class Q_PHASE_EXPORT QPhaseIcon : public QPhaseView
{
public:
    explicit QPhaseIcon(QPhaseView *parent = nullptr);
    QPhaseIcon(const QString &svgPathData, QRectF viewBox, const QColor &color, QPhaseView *parent = nullptr);

    void setSvgPath(const QString &svgPathData, QRectF viewBox = QRectF(0, 0, 24, 24));
    void setColor(const QColor &color);

protected:
    void describeVisuals(QPhaseVisuals &visuals) override;

private:
    QString m_svgPath;
    QRectF m_viewBox{0, 0, 24, 24};
    QColor m_color{Qt::white};
};

// Dedicated offscreen Vulkan 3D / compute / custom-pipeline view composited into QtPhase's single pass
// via bindless descriptor indexing (defaults to QPhaseRendererAffinity::GpuOnly)!
class Q_PHASE_EXPORT QPhaseGpuView : public QPhaseView
{
public:
    using RenderCallback = std::function<void(const QPhaseGpuSurfaceContext &)>;

    explicit QPhaseGpuView(QPhaseView *parent = nullptr);
    ~QPhaseGpuView() override;

    void setDepthBufferEnabled(bool enabled);
    bool isDepthBufferEnabled() const noexcept { return m_depthEnabled; }

    void setCornerRadius(QPhaseCorners corners);
    QPhaseCorners cornerRadius() const noexcept { return m_corners; }

    void setRenderCallback(RenderCallback callback);
    void markGpuDirty();

protected:
    virtual void renderGpu(const QPhaseGpuSurfaceContext &context);
    void describeVisuals(QPhaseVisuals &visuals) override;

private:
    std::shared_ptr<RenderCallback> m_callbackDispatcher;
    RenderCallback m_userCallback;
    QPhaseCorners m_corners{};
    bool m_depthEnabled = false;
    bool m_gpuContentDirty = true;
};

QT_END_NAMESPACE

#endif // QPHASEVIEW_H
