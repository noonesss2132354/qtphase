#include <QtCore/QAbstractTableModel>
#include <QtCore/QCommandLineParser>
#include <QtCore/QCoreApplication>
#include <QtCore/QTimer>
#include <QtGui/QFont>
#include <QtGui/QGuiApplication>
#include <QtGui/QImage>
#include <QtGui/QScreen>

#include <QtPhase/qphasewindow.h>
#include <QtPhase/qphaseview.h>
#include <QtPhase/qphaselabel.h>
#include <QtPhase/qphasetextedit.h>
#include <QtPhase/qphasetableview.h>

#include <vulkan/vulkan.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace {

QImage makeGpuEmblemTexture(int sz)
{
    QImage img(sz, sz, QImage::Format_RGBA8888);
    for (int y = 0; y < sz; ++y) {
        uchar *row = img.scanLine(y);
        const float fy = float(y) / float(std::max(1, sz - 1));
        for (int x = 0; x < sz; ++x) {
            const float fx = float(x) / float(std::max(1, sz - 1));
            row[x * 4 + 0] = uchar(45 + 180 * fx);
            row[x * 4 + 1] = uchar(130 + 95 * (1.0f - fy));
            row[x * 4 + 2] = 245;
            row[x * 4 + 3] = 255;
        }
    }
    return img;
}

// 100,000-row virtualized QAbstractTableModel with instant substring filtering
class HundredKSceneModel final : public QAbstractTableModel
{
public:
    static constexpr int kTotalBaseRows = 100'000;

    void setFilterString(const QString &filter)
    {
        const QString trimmed = filter.trimmed();
        if (m_filter == trimmed)
            return;

        beginResetModel();
        m_filter = trimmed;
        m_filteredRows.clear();
        if (!m_filter.isEmpty()) {
            m_filteredRows.reserve(4096);
            for (int r = 0; r < kTotalBaseRows; ++r) {
                if (QString::number(r).contains(m_filter, Qt::CaseInsensitive)
                    || subsystemForRow(r).contains(m_filter, Qt::CaseInsensitive)) {
                    m_filteredRows.push_back(r);
                }
            }
        }
        endResetModel();
    }

    int rowCount(const QModelIndex &parent = {}) const override
    {
        if (parent.isValid())
            return 0;
        return m_filter.isEmpty() ? kTotalBaseRows : int(m_filteredRows.size());
    }

    int columnCount(const QModelIndex &parent = {}) const override
    {
        return parent.isValid() ? 0 : 5;
    }

    QVariant headerData(int section, Qt::Orientation orientation, int role) const override
    {
        if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
            return {};
        static const char *const kCols[5] = {
            "Row / Slot", "Subsystem", "Primitive Kind", "Memory Residency", "GPU Time"
        };
        return QString::fromLatin1(kCols[std::clamp(section, 0, 4)]);
    }

    QVariant data(const QModelIndex &idx, int role) const override
    {
        if (!idx.isValid() || role != Qt::DisplayRole)
            return {};

        const int r = m_filter.isEmpty() ? idx.row() : m_filteredRows[size_t(idx.row())];
        switch (idx.column()) {
        case 0:
            return QStringLiteral("#%1").arg(r, 6, 10, QLatin1Char('0'));
        case 1:
            return subsystemForRow(r);
        case 2:
            switch (r % 4) {
            case 0: return QStringLiteral("HbGpuCurve (Slug Bezier)");
            case 1: return QStringLiteral("RoundedBox + erf Shadow");
            case 2: return QStringLiteral("Bindless Sampler2D");
            default: return QStringLiteral("TransformSlot Scroll");
            }
        case 3:
            return QStringLiteral("DEVICE_LOCAL | BDA");
        case 4:
            return QStringLiteral("0.%1 ms").arg(11 + (r * 13) % 48, 2, 10, QLatin1Char('0'));
        default:
            return {};
        }
    }

private:
    static QString subsystemForRow(int r)
    {
        static const QString kNames[5] = {
            QStringLiteral("Wayland Phased Surface"),
            QStringLiteral("HarfBuzz 15 Curve Encoder"),
            QStringLiteral("VMA Unified Memory Arena"),
            QStringLiteral("Bindless Texture Table"),
            QStringLiteral("QGridLayoutEngine Solver")
        };
        return kNames[r % 5];
    }

    QString m_filter;
    std::vector<int> m_filteredRows;
};

class QtPhaseShowcaseWindow final : public QPhaseWindow
{
public:
    explicit QtPhaseShowcaseWindow(int exitAfterFrames,
                                   bool enableReflex,
                                   QPhaseRendererAffinity widgetAffinity,
                                   bool widgetBlocking,
                                   bool includeGpu3dWidget)
        : m_exitAfterFrames(exitAfterFrames)
        , m_widgetAffinity(widgetAffinity)
        , m_widgetBlocking(widgetBlocking)
        , m_includeGpu3dWidget(includeGpu3dWidget)
    {
        setLowLatencyReflexMode(enableReflex);
        setTitle(QStringLiteral("QtPhase — Compute Tile-Binned Vulkan 1.3 + UI Reflex Deadline Pacer"));
        resize(980, 640);
        setBackgroundColor(QColor(15, 18, 26));

        QObject::connect(this, &QPhaseWindow::cutoverCompleted, this, [this]() {
            const auto t = telemetry();
            const bool gpuWon = (t.lockedWinner == Phase::GpuSteadyState);
            const qreal dpr = devicePixelRatio();
            const QSize px = size() * dpr;
            const double hz = (screen() && screen()->refreshRate() > 1.0)
                    ? double(screen()->refreshRate())
                    : 60.0;
            const char *affStr = (t.activeAffinityConstraint == QPhaseRendererAffinity::GpuOnly)
                    ? "FORCED GpuOnly (zero race)"
                    : ((t.activeAffinityConstraint == QPhaseRendererAffinity::CpuOnly)
                       ? "FORCED CpuOnly (zero race)"
                       : "Any (5-frame head-to-head race)");
            std::printf("[QtPhase Worker Coordinator] Locked %s — %s shut down!\n"
                        "  Widget affinity constraint    : %s [blocking=%s]\n"
                        "  Surface physical size / DPR   : %dx%d px (DPR %.2fx @ %.1f Hz)\n"
                        "  UI Reflex deadline pacer      : %s\n"
                        "  Async Vulkan 1.3 hw warmup    : %.2f ms (started ASAP on window construction)\n",
                        gpuWon ? "GPU (Vulkan 1.3)" : "CPU (Multi-Threaded Tile Pool)",
                        gpuWon ? "CPU worker + wl_shm" : "Vulkan worker + VRAM",
                        affStr,
                        t.activeConstraintWasBlocking ? "true" : "false",
                        px.width(), px.height(), double(dpr), hz,
                        t.lowLatencyReflexEnabled ? "ENABLED (just-in-time VSync deadline sampling)" : "DISABLED (legacy QTimer 16ms)",
                        double(t.gpuAsyncWarmupUs) / 1000.0);
            std::printf("  Window create -> Winner lock  : %.2f ms\n"
                        "  First frame after expose      : %.2f ms\n"
                        "  Frames presented (CPU / GPU)  : %llu CPU (%llu aborted) / %llu GPU (%llu aborted)\n"
                        "  Locked steady-state worker    : %s\n",
                        double(t.showToGpuCutoverUs) / 1000.0,
                        double(t.showToFirstFrameCommitUs) / 1000.0,
                        static_cast<unsigned long long>(t.cpuFramesPresented),
                        static_cast<unsigned long long>(t.abortedCpuFrames),
                        static_cast<unsigned long long>(t.gpuFramesPresented),
                        static_cast<unsigned long long>(t.abortedGpuFrames),
                        gpuWon ? "GPU Vulkan 1.3 (16x16 Compute Tile-Binned)"
                               : "CPU Multi-Threaded wl_shm Pool");

            if (m_phaseBanner) {
                if (gpuWon) {
                    m_phaseBanner->setBoxStyle({
                        .radius = QPhaseCorners::all(10.0f),
                        .fill = QPhaseFill::linearGradient({0, 0}, {360, 0}, QColor(47, 133, 90), QColor(56, 161, 105)),
                        .border = { .width = 1.0f, .color = QColor(154, 230, 180, 140) },
                        .shadow = { .blurRadius = 14.0f, .offset = {0, 4}, .color = QColor(47, 133, 90, 95) }
                    });
                } else {
                    m_phaseBanner->setBoxStyle({
                        .radius = QPhaseCorners::all(10.0f),
                        .fill = QPhaseFill::linearGradient({0, 0}, {360, 0}, QColor(43, 108, 176), QColor(49, 130, 206)),
                        .border = { .width = 1.0f, .color = QColor(144, 205, 244, 140) },
                        .shadow = { .blurRadius = 14.0f, .offset = {0, 4}, .color = QColor(43, 108, 176, 95) }
                    });
                }
            }
            if (m_phaseLabel) {
                if (gpuWon) {
                    m_phaseLabel->setText(QStringLiteral("LOCKED 5-0: Vulkan 1.3 Tile-Binned (%1 ms warmup)")
                                          .arg(double(t.gpuAsyncWarmupUs) / 1000.0, 0, 'f', 1));
                } else {
                    m_phaseLabel->setText(QStringLiteral("LOCKED 5-0: %1-Thread CPU Pool (Vulkan Freed)")
                                          .arg(t.cpuBootTileThreads));
                }
            }
        });

        if (!enableReflex) {
            // Legacy blind 16ms QTimer path (used when --no-reflex is passed for A/B latency benchmarking)
            m_legacyTimer.setInterval(16);
            QObject::connect(&m_legacyTimer, &QTimer::timeout, this, [this]() {
                advanceBenchmarkStep();
                requestUpdate();
            });
            m_legacyTimer.start();
        }
    }

protected:
    void beforeFrameSample() override
    {
        QPhaseWindow::beforeFrameSample();
        if (isLowLatencyReflexMode() && m_exitAfterFrames > 0) {
            advanceBenchmarkStep();
            if (!m_quitScheduled)
                requestFrameSample();
        }
    }

    void setupUi(QPhaseView *root) override
    {
        root->setBoxLayout(Qt::Vertical, QMarginsF(22, 20, 22, 20), 14.0);

        auto *headerRow = root->add<QPhaseView>();
        headerRow->setPreferredSize(QSizeF(936, 46));
        headerRow->setMaximumSize(QSizeF(4000, 46));
        headerRow->setBoxLayout(Qt::Horizontal, {}, 12.0);

        auto *emblem = headerRow->add<QPhaseImage>(makeGpuEmblemTexture(64));
        emblem->setCornerRadius(QPhaseCorners::all(10.0f));
        emblem->setMinimumSize(QSizeF(38, 38));
        emblem->setPreferredSize(QSizeF(38, 38));
        emblem->setMaximumSize(QSizeF(38, 38));

        auto *boltIcon = headerRow->add<QPhaseIcon>(
                QStringLiteral("M13 2 L4 14 H11 L10 22 L20 10 H13 Z"),
                QRectF(0, 0, 24, 24), QColor(246, 224, 94));
        boltIcon->setMinimumSize(QSizeF(28, 28));
        boltIcon->setPreferredSize(QSizeF(28, 28));
        boltIcon->setMaximumSize(QSizeF(28, 28));
        if (m_widgetAffinity != QPhaseRendererAffinity::Any)
            boltIcon->setRendererAffinity(m_widgetAffinity, m_widgetBlocking);

        if (m_includeGpu3dWidget) {
            auto *gpuBadge = headerRow->add<QPhaseGpuView>();
            gpuBadge->setMinimumSize(QSizeF(56, 38));
            gpuBadge->setPreferredSize(QSizeF(56, 38));
            gpuBadge->setMaximumSize(QSizeF(56, 38));
            gpuBadge->setCornerRadius(QPhaseCorners::all(10.0f));
            gpuBadge->setDepthBufferEnabled(true);
            gpuBadge->setBackendBlocking(m_widgetBlocking);
            gpuBadge->setRenderCallback([](const QPhaseGpuSurfaceContext &ctx) {
                auto cmd = static_cast<VkCommandBuffer>(ctx.vkCommandBuffer);
                const uint32_t w = uint32_t(ctx.pixelSize.width());
                const uint32_t h = uint32_t(ctx.pixelSize.height());

                VkRenderingAttachmentInfo colorAtt{};
                colorAtt.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
                colorAtt.imageView = VkImageView(ctx.colorImageView);
                colorAtt.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                colorAtt.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
                colorAtt.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                colorAtt.clearValue.color = {{ 0.16f, 0.52f, 0.92f, 1.0f }};

                VkRenderingInfo ri{};
                ri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
                ri.renderArea = { {0, 0}, { w, h } };
                ri.layerCount = 1;
                ri.colorAttachmentCount = 1;
                ri.pColorAttachments = &colorAtt;
                vkCmdBeginRendering(cmd, &ri);

                VkClearAttachment ca{};
                ca.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                ca.colorAttachment = 0;
                ca.clearValue.color = {{ 0.96f, 0.62f, 0.18f, 1.0f }};
                VkClearRect cr{ { { int(w / 4), int(h / 4) }, { w / 2, h / 2 } }, 0, 1 };
                vkCmdClearAttachments(cmd, 1, &ca, 1, &cr);
                vkCmdEndRendering(cmd);
            });
        }

        auto *title = headerRow->add<QPhaseLabel>(QStringLiteral("QtPhase 100k-Row GPU Explorer"));
        QFont fTitle;
        fTitle.setPixelSize(21);
        fTitle.setWeight(QFont::Bold);
        title->setFont(fTitle);
        title->setStretch(1);

        m_phaseBanner = headerRow->add<QPhaseBox>();
        m_phaseBanner->setMinimumSize(QSizeF(380, 38));
        m_phaseBanner->setPreferredSize(QSizeF(380, 38));
        m_phaseBanner->setMaximumSize(QSizeF(420, 38));
        m_phaseBanner->setBoxLayout(Qt::Horizontal, QMarginsF(14, 0, 14, 0), 0.0);
        m_phaseBanner->setBoxStyle({
            .radius = QPhaseCorners::all(10.0f),
            .fill = QPhaseFill::linearGradient({0, 0}, {380, 0}, QColor(192, 86, 33), QColor(214, 158, 46)),
            .border = { .width = 1.0f, .color = QColor(254, 235, 200, 140) },
            .shadow = { .blurRadius = 14.0f, .offset = {0, 4}, .color = QColor(192, 86, 33, 95) }
        });

        m_phaseLabel = m_phaseBanner->add<QPhaseLabel>(QStringLiteral("RACING WORKERS: CPU Pool vs Vulkan 1.3 (First to 5-0)"));
        QFont fBadge;
        fBadge.setPixelSize(13);
        fBadge.setWeight(QFont::DemiBold);
        m_phaseLabel->setFont(fBadge);
        m_phaseLabel->setAlignment(Qt::AlignCenter);
        m_phaseLabel->setStretch(1);

        auto *filterBar = root->add<QPhaseView>();
        filterBar->setPreferredSize(QSizeF(936, 40));
        filterBar->setMaximumSize(QSizeF(4000, 40));
        filterBar->setBoxLayout(Qt::Horizontal, {}, 12.0);

        auto *filterLbl = filterBar->add<QPhaseLabel>(QStringLiteral("Live Filter (100,000 rows):"));
        filterLbl->setTextColor(QColor(160, 174, 192));
        filterLbl->setPreferredSize(QSizeF(185, 38));

        m_searchEdit = filterBar->add<QPhaseTextEdit>();
        m_searchEdit->setPlaceholderText(QStringLiteral("Type row # or subsystem (e.g. HarfBuzz, Wayland, 4200)..."));
        m_searchEdit->setStretch(1);
        m_searchEdit->setOnTextChanged([this](const QString &txt) {
            m_model.setFilterString(txt);
            if (m_countLabel)
                m_countLabel->setText(QStringLiteral("%1 rows").arg(m_model.rowCount()));
        });

        m_countLabel = filterBar->add<QPhaseLabel>(QStringLiteral("100000 rows"));
        m_countLabel->setTextColor(QColor(144, 205, 244));
        m_countLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        m_countLabel->setMinimumSize(QSizeF(105, 38));
        m_countLabel->setPreferredSize(QSizeF(105, 38));

        m_table = root->add<QPhaseTableView>();
        m_table->setStretch(1);
        m_table->setColumnWidths({ 125.0, 245.0, 235.0, 195.0, 120.0 });
        m_table->setModel(&m_model);
        m_table->setSelectedRow(0);

        m_searchEdit->requestFocus();
    }

private:
    void advanceBenchmarkStep()
    {
        if (m_quitScheduled)
            return;

        const auto frames = frameTimings();
        if (m_exitAfterFrames > 0
            && int(frames.size()) >= m_exitAfterFrames
            && currentPhase() != Phase::WorkerRace) {
            m_quitScheduled = true;
            printReportAndQuit(frames);
            return;
        }

        ++m_frameCounter;
        if (m_exitAfterFrames > 0 && m_table) {
            // First two-thirds: smooth sub-row + multi-row table scroll (tests UMA transform-only sync + GPU glyph cache!)
            // Final third: localized label change + idle blinking caret (<0.1% damage!)
            const int scrollCutoff = std::max(10, m_exitAfterFrames - 8);
            if (int(frames.size()) < scrollCutoff) {
                // Alternate small sub-row smooth scroll (4.25 px) & larger jumps so we observe BOTH
                // pure TransformSlot scroll (~320 B primitive-free sync!) and row recycling!
                const double delta = (m_frameCounter % 3 == 0) ? 18.5 : 4.25;
                m_scrollPos += delta;
                m_table->scrollToY(m_scrollPos);
            } else if (!m_updatedCountLabel && m_countLabel) {
                m_updatedCountLabel = true;
                m_countLabel->setText(QStringLiteral("100,000 rows"));
            }
        }
    }

    void printReportAndQuit(const std::vector<FrameTiming> &frames)
    {
        const auto t = telemetry();
        std::printf("\n[QtPhase Per-Frame Timeline (%zu frames recorded)]\n", frames.size());
        std::printf("  Frame  Winner          Race State      dt_between   cpu_submit   gpu_hw_ts    sample->pres   uma_sync    damage\n");
        std::printf("  -----  -------------   -------------   ----------   ----------   ----------   ------------   ---------   -------\n");

        qint64 steadyGpuHwSum = 0;
        qint64 steadyLatencySum = 0;
        int steadyCount = 0;

        for (const auto &f : frames) {
            const char *workerStr = (f.presentedBy == Worker::CpuShm) ? "CPU wl_shm" : "GPU Vulkan1.3";
            char stateBuf[24];
            if (f.phase == Phase::GpuSteadyState || f.phase == Phase::CpuSteadyState) {
                std::snprintf(stateBuf, sizeof(stateBuf), "LOCKED (%s)",
                              f.phase == Phase::GpuSteadyState ? "GPU" : "CPU");
                if (f.interFrameDeltaUs > 0) {
                    steadyGpuHwSum += f.gpuHardwareUs;
                    steadyLatencySum += f.sampleToPresentUs;
                    ++steadyCount;
                }
            } else if (f.winStreak == 0) {
                std::snprintf(stateBuf, sizeof(stateBuf), "GPU booting");
            } else {
                std::snprintf(stateBuf, sizeof(stateBuf), "%s %u/%u",
                              f.presentedBy == Worker::CpuShm ? "CPU" : "GPU",
                              unsigned(f.winStreak),
                              unsigned(kWorkerLockInStreak));
            }
            const double dmgPct = (f.totalViewportPixels > 0)
                    ? (100.0 * double(f.damagedPixels) / double(f.totalViewportPixels))
                    : 100.0;
            std::printf("  #%02u    %-13s   %-13s  %7.2f ms   %7.2f ms   %7.2f ms     %7.2f ms   %7.2f KB   %6.2f%%\n",
                        unsigned(f.frameIndex),
                        workerStr,
                        stateBuf,
                        double(f.interFrameDeltaUs) / 1000.0,
                        double(f.backendWorkUs) / 1000.0,
                        double(f.gpuHardwareUs) / 1000.0,
                        double(f.sampleToPresentUs) / 1000.0,
                        double(f.gpuSyncedBytes) / 1024.0,
                        dmgPct);
        }

        const quint64 totalGlyphLookups = t.gpuGlyphCacheHits + t.gpuGlyphCacheMisses;
        const double hitRatePct = (totalGlyphLookups > 0)
                ? (100.0 * double(t.gpuGlyphCacheHits) / double(totalGlyphLookups))
                : 0.0;

        if (steadyCount > 0) {
            std::printf("\n[QtPhase Steady-State Summary (%d frames, UI Reflex: %s)]\n"
                        "  Avg GPU Hardware Execution (VK_QUERY_TYPE_TIMESTAMP) : %.2f ms\n"
                        "  Avg Input Sample -> Present Latency                  : %.2f ms\n"
                        "  On-GPU Scratch Glyph Cache Hit Rate                  : %.1f%% (%llu hits / %llu bakes)\n",
                        steadyCount,
                        t.lowLatencyReflexEnabled ? "ON" : "OFF",
                        double(steadyGpuHwSum) / double(steadyCount) / 1000.0,
                        double(steadyLatencySum) / double(steadyCount) / 1000.0,
                        hitRatePct,
                        static_cast<unsigned long long>(t.gpuGlyphCacheHits),
                        static_cast<unsigned long long>(t.gpuGlyphCacheMisses));
        }

        std::printf("[QtPhase Exit] Total presented -> CPU: %llu, GPU: %llu (aborted losers -> CPU: %llu, GPU: %llu)\n",
                    static_cast<unsigned long long>(t.cpuFramesPresented),
                    static_cast<unsigned long long>(t.gpuFramesPresented),
                    static_cast<unsigned long long>(t.abortedCpuFrames),
                    static_cast<unsigned long long>(t.abortedGpuFrames));
        QTimer::singleShot(0, qApp, &QCoreApplication::quit);
    }

    HundredKSceneModel m_model;
    QPhaseBox *m_phaseBanner = nullptr;
    QPhaseLabel *m_phaseLabel = nullptr;
    QPhaseLabel *m_countLabel = nullptr;
    QPhaseTextEdit *m_searchEdit = nullptr;
    QPhaseTableView *m_table = nullptr;
    QTimer m_legacyTimer;
    double m_scrollPos = 0.0;
    int m_frameCounter = 0;
    int m_exitAfterFrames = 0;
    QPhaseRendererAffinity m_widgetAffinity = QPhaseRendererAffinity::Any;
    bool m_widgetBlocking = false;
    bool m_includeGpu3dWidget = false;
    bool m_updatedCountLabel = false;
    bool m_quitScheduled = false;
};

} // namespace

int main(int argc, char **argv)
{
    qunsetenv("QT_PLUGIN_PATH");
    QCoreApplication::setLibraryPaths({ QStringLiteral(QT_PHASE_PLUGINS_DIR) });

    QGuiApplication app(argc, argv);

    QCommandLineParser parser;
    parser.addHelpOption();
    QCommandLineOption delayOpt(QStringLiteral("delay-gpu-ms"),
                                QStringLiteral("Simulated background GPU init delay (ms)"),
                                QStringLiteral("ms"),
                                QStringLiteral("0"));
    QCommandLineOption gpuFrameDelayOpt(QStringLiteral("gpu-frame-delay-ms"),
                                        QStringLiteral("Simulated per-frame GPU delay (ms) to test slow/overloaded GPU"),
                                        QStringLiteral("ms"),
                                        QStringLiteral("0"));
    QCommandLineOption exitOpt(QStringLiteral("exit-after-frames"),
                               QStringLiteral("Exit automatically after N frames once winner locks in"),
                               QStringLiteral("frames"),
                               QStringLiteral("0"));
    QCommandLineOption noReflexOpt(QStringLiteral("no-reflex"),
                                   QStringLiteral("Disable VSync-deadline UI Reflex pacing (use legacy 16ms QTimer)"));
    QCommandLineOption affinityOpt(QStringLiteral("affinity"),
                                   QStringLiteral("Widget renderer affinity: any, gpu, or cpu"),
                                   QStringLiteral("mode"),
                                   QStringLiteral("any"));
    QCommandLineOption blockingOpt(QStringLiteral("blocking-affinity"),
                                   QStringLiteral("Wait for forced backend before presenting frame #1"));
    QCommandLineOption gpuViewOpt(QStringLiteral("with-gpu-view"),
                                  QStringLiteral("Mount a custom Vulkan 3D/depth QPhaseGpuView in the header"));
    parser.addOption(delayOpt);
    parser.addOption(gpuFrameDelayOpt);
    parser.addOption(exitOpt);
    parser.addOption(noReflexOpt);
    parser.addOption(affinityOpt);
    parser.addOption(blockingOpt);
    parser.addOption(gpuViewOpt);
    parser.process(app);

    QPhaseRendererAffinity aff = QPhaseRendererAffinity::Any;
    const QString affVal = parser.value(affinityOpt).trimmed().toLower();
    if (affVal == QLatin1String("gpu"))
        aff = QPhaseRendererAffinity::GpuOnly;
    else if (affVal == QLatin1String("cpu"))
        aff = QPhaseRendererAffinity::CpuOnly;

    QtPhaseShowcaseWindow win(parser.value(exitOpt).toInt(),
                              !parser.isSet(noReflexOpt),
                              aff,
                              parser.isSet(blockingOpt),
                              parser.isSet(gpuViewOpt));
    win.setSimulatedGpuInitDelayMs(parser.value(delayOpt).toInt());
    win.setSimulatedGpuFrameDelayMs(parser.value(gpuFrameDelayOpt).toInt());
    win.show();

    return app.exec();
}
