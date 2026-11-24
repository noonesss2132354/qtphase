#include <QtCore/QAbstractTableModel>
#include <QtCore/QCommandLineParser>
#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QElapsedTimer>
#include <QtGui/QFont>
#include <QtGui/QGuiApplication>
#include <QtGui/QImage>

#include <QtPhase/qphaseview.h>
#include <QtPhase/qphasevisuals.h>
#include <QtPhase/qphaselabel.h>
#include <QtPhase/qphasetextedit.h>
#include <QtPhase/qphasetableview.h>
#include <QtPhase/private/qphasescenearena_p.h>
#include <QtPhase/private/qphasecpurasterizer_p.h>
#include <QtPhase/private/qphasevulkanbackend_p.h>

#include <vulkan/vulkan.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace {

struct DiffMetrics {
    int maxChannelDiff = 0;
    double meanAbsChannelDiff = 0.0;
    int mismatchedPixels = 0;
    int significantPixelsOver8 = 0;
    QImage heatmap;
};

DiffMetrics computeAndSaveDiff(const QImage &cpuImg, const QImage &gpuImg,
                               const QString &outDir, const QString &prefix)
{
    DiffMetrics m{};
    const int w = cpuImg.width();
    const int h = cpuImg.height();
    m.heatmap = QImage(w, h, QImage::Format_ARGB32);

    uint64_t sumDiff = 0;
    for (int y = 0; y < h; ++y) {
        const auto *cRow = reinterpret_cast<const uint32_t *>(cpuImg.constScanLine(y));
        const auto *gRow = reinterpret_cast<const uint32_t *>(gpuImg.constScanLine(y));
        auto *dRow = reinterpret_cast<uint32_t *>(m.heatmap.scanLine(y));

        for (int x = 0; x < w; ++x) {
            const uint32_t c = cRow[x];
            const uint32_t g = gRow[x];

            const int db = std::abs(int(c & 0xFF) - int(g & 0xFF));
            const int dg = std::abs(int((c >> 8) & 0xFF) - int((g >> 8) & 0xFF));
            const int dr = std::abs(int((c >> 16) & 0xFF) - int((g >> 16) & 0xFF));
            const int da = std::abs(int((c >> 24) & 0xFF) - int((g >> 24) & 0xFF));

            const int pxMax = std::max({dr, dg, db, da});
            m.maxChannelDiff = std::max(m.maxChannelDiff, pxMax);
            sumDiff += uint64_t(dr + dg + db + da);
            if (pxMax > 0)
                ++m.mismatchedPixels;
            if (pxMax > 8)
                ++m.significantPixelsOver8;

            // 8x amplified difference heatmap
            const uint8_t hr = uint8_t(std::min(255, dr * 8));
            const uint8_t hg = uint8_t(std::min(255, dg * 8));
            const uint8_t hb = uint8_t(std::min(255, db * 8));
            dRow[x] = (0xFFu << 24) | (uint32_t(hr) << 16) | (uint32_t(hg) << 8) | uint32_t(hb);
        }
    }

    m.meanAbsChannelDiff = double(sumDiff) / double(std::max(1, w * h * 4));

    QDir().mkpath(outDir);
    cpuImg.save(outDir + u'/' + prefix + u"_cpu.png");
    gpuImg.save(outDir + u'/' + prefix + u"_gpu.png");
    m.heatmap.save(outDir + u'/' + prefix + u"_diff.png");
    return m;
}

QImage createProceduralGradientGridImage(int w, int h)
{
    QImage img(w, h, QImage::Format_RGBA8888);
    for (int y = 0; y < h; ++y) {
        uchar *row = img.scanLine(y);
        const float fy = float(y) / float(std::max(1, h - 1));
        for (int x = 0; x < w; ++x) {
            const float fx = float(x) / float(std::max(1, w - 1));
            const float wave = 0.5f + 0.5f * std::sin(fx * 12.566f) * std::cos(fy * 12.566f);
            row[x * 4 + 0] = uchar(std::clamp((0.18f + 0.72f * fx) * 255.0f, 0.0f, 255.0f));
            row[x * 4 + 1] = uchar(std::clamp((0.25f + 0.55f * wave) * 255.0f, 0.0f, 255.0f));
            row[x * 4 + 2] = uchar(std::clamp((0.88f - 0.55f * fy) * 255.0f, 0.0f, 255.0f));
            row[x * 4 + 3] = 255;
        }
    }
    return img;
}

QImage createTranslucentOrbImage(int sz)
{
    QImage img(sz, sz, QImage::Format_RGBA8888);
    const float c = (float(sz) - 1.0f) * 0.5f;
    for (int y = 0; y < sz; ++y) {
        uchar *row = img.scanLine(y);
        for (int x = 0; x < sz; ++x) {
            const float dx = (float(x) - c) / c;
            const float dy = (float(y) - c) / c;
            const float r = std::sqrt(dx * dx + dy * dy);
            const float alpha = std::clamp((1.0f - r) * 3.0f, 0.0f, 1.0f);
            row[x * 4 + 0] = uchar(72 + 160 * (1.0f - std::min(1.0f, r)));
            row[x * 4 + 1] = uchar(187 + 50 * (0.5f - 0.5f * dy));
            row[x * 4 + 2] = 245;
            row[x * 4 + 3] = uchar(alpha * 235.0f);
        }
    }
    return img;
}

class SparklineChartView final : public QPhaseView
{
public:
    explicit SparklineChartView(QPhaseView *parent = nullptr)
        : QPhaseView(parent)
    {
    }

protected:
    void describeVisuals(QPhaseVisuals &v) override
    {
        const qreal w = width();
        const qreal h = height();
        if (w <= 4.0 || h <= 4.0)
            return;

        // Crisp butt-cap reference grid lines
        v.strokeLine(QPointF(4.0, h - 6.0), QPointF(w - 4.0, h - 6.0), 1.25f, QColor(255, 255, 255, 70), true);
        v.strokeLine(QPointF(4.0, h * 0.5), QPointF(w - 4.0, h * 0.5), 1.0f, QColor(148, 163, 184, 55), true);

        // Analytical SDF capsule-joined polyline wave
        QList<QPointF> pts;
        constexpr int kSamples = 9;
        pts.reserve(kSamples);
        for (int i = 0; i < kSamples; ++i) {
            const qreal t = qreal(i) / qreal(kSamples - 1);
            const qreal x = 8.0 + t * (w - 16.0);
            const qreal y = h * (0.55 - 0.34 * std::sin(t * 5.2) + 0.12 * std::cos(t * 11.0));
            pts.append(QPointF(x, y));
        }
        v.polyline(pts, 2.75f, QColor(99, 230, 190, 245), false);
    }
};

class SampleParityTableModel final : public QAbstractTableModel
{
public:
    int rowCount(const QModelIndex &parent = {}) const override
    {
        return parent.isValid() ? 0 : 250;
    }

    int columnCount(const QModelIndex &parent = {}) const override
    {
        return parent.isValid() ? 0 : 4;
    }

    QVariant headerData(int section, Qt::Orientation orientation, int role) const override
    {
        if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
            return {};
        static const char *const kHeaders[4] = { "Node ID", "Subsystem", "Backend", "Frame Cost" };
        return QString::fromLatin1(kHeaders[std::clamp(section, 0, 3)]);
    }

    QVariant data(const QModelIndex &idx, int role) const override
    {
        if (!idx.isValid() || role != Qt::DisplayRole)
            return {};
        const int r = idx.row();
        switch (idx.column()) {
        case 0: return QStringLiteral("#%1").arg(1000 + r);
        case 1: return (r % 3 == 0) ? QStringLiteral("hb-gpu Curve Slug")
                     : ((r % 3 == 1) ? QStringLiteral("VMA BDA Scene Table")
                                     : QStringLiteral("SDF Rounded Shadow"));
        case 2: return QStringLiteral("Vulkan 1.3 UMA");
        case 3: return QStringLiteral("%1 us").arg(12 + (r * 7) % 35);
        default: return {};
        }
    }
};

} // namespace

int main(int argc, char **argv)
{
    qunsetenv("QT_PLUGIN_PATH");
    QCoreApplication::setLibraryPaths({ QStringLiteral(QT_PHASE_PLUGINS_DIR) });
    qputenv("QT_QPA_PLATFORM", "offscreen");

    QGuiApplication app(argc, argv);

    QCommandLineParser parser;
    parser.addHelpOption();
    QCommandLineOption outDirOpt(QStringLiteral("output-dir"),
                                 QStringLiteral("Directory for CPU/GPU/Diff PNGs"),
                                 QStringLiteral("dir"),
                                 QDir::currentPath() + QStringLiteral("/diffs"));
    parser.addOption(outDirOpt);
    parser.process(app);

    const QString outDir = parser.value(outDirOpt);
    const qreal dpr = 1.25;
    const QSize logicalSize(720, 420);
    const QSize targetPixelSize(int(std::round(logicalSize.width() * dpr)),
                                int(std::round(logicalSize.height() * dpr)));

    QPhaseCpuRasterizer cpu;
    QPhaseVulkanBackend gpu;
    if (!gpu.initializeDevice(false)) {
        std::fprintf(stderr, "FAIL: Vulkan 1.3 offscreen initialization failed\n");
        return 1;
    }

    bool allPassed = true;
    auto reportStage = [&](const char *label, const QString &prefix, const DiffMetrics &m,
                           int maxAllowedChanDiff, double maxAllowedMeanDiff) {
        const int totalPx = targetPixelSize.width() * targetPixelSize.height();
        const bool ok = (m.maxChannelDiff <= maxAllowedChanDiff) && (m.meanAbsChannelDiff <= maxAllowedMeanDiff);
        std::printf("[%s] %-4s  max_diff=%3d/255  mean_abs_diff=%.5f/255  diff_px=%d/%d (>8: %d)\n",
                    label, ok ? "PASS" : "FAIL",
                    m.maxChannelDiff, m.meanAbsChannelDiff,
                    m.mismatchedPixels, totalPx, m.significantPixelsOver8);
        std::printf("          Saved %s/%s_{cpu,gpu,diff}.png\n",
                    qUtf8Printable(outDir), qUtf8Printable(prefix));
        if (!ok)
            allPassed = false;
    };

    // =========================================================================
    // STAGE 1: Core Retained Arena (Transforms, Clips, Solid Rects @ 125% DPR)
    // =========================================================================
    {
        QPhaseSceneArena arena;
        arena.setClearColor(QColor(20, 24, 33, 255));

        const uint32_t panelTx = arena.allocateTransform(0);
        arena.updateTransform(panelTx, 28.0f, 24.0f, 1.0f, 1.0f, 1.0f);

        const uint32_t scrollTx = arena.allocateTransform(panelTx);
        arena.updateTransform(scrollTx, 0.0f, -14.5f, 1.0f, 1.0f, 1.0f);

        const uint32_t panelClip = arena.allocateClip(0, panelTx, QRectF(0.0, 0.0, 420.0, 360.0), {});

        std::vector<uint32_t> order;
        auto addRect = [&](uint32_t tx, uint32_t clip, QRectF r, QColor c) {
            PhaseGpuPrimitive p{};
            p.kind = uint32_t(PhasePrimKind::SolidRect);
            p.transformIndex = tx;
            p.clipIndex = clip;
            p.x = float(r.x());
            p.y = float(r.y());
            p.w = float(r.width());
            p.h = float(r.height());
            p.c0r = c.redF();
            p.c0g = c.greenF();
            p.c0b = c.blueF();
            p.c0a = c.alphaF();
            order.push_back(arena.allocatePrimitive(p));
        };

        addRect(panelTx, 0, QRectF(0, 0, 420, 360), QColor(34, 39, 52, 255));
        for (int i = 0; i < 11; ++i) {
            const QColor rowColor = (i % 2 == 0) ? QColor(48, 56, 74, 255) : QColor(41, 47, 62, 255);
            addRect(scrollTx, panelClip, QRectF(14.0, i * 38.0 + 10.0, 392.0, 30.0), rowColor);
        }
        addRect(panelTx, panelClip, QRectF(24.25, 52.5, 250.5, 76.0), QColor(66, 153, 225, 160));
        addRect(0, 0, QRectF(476.0, 24.0, 216.0, 360.0), QColor(45, 52, 68, 255));
        addRect(0, 0, QRectF(496.0, 44.0, 176.0, 52.0), QColor(72, 187, 120, 255));
        arena.setDrawOrder(std::move(order));

        const QImage cpuImg = cpu.renderToImage(arena, targetPixelSize, dpr);
        const QImage gpuImg = gpu.renderOffscreenToImage(arena, targetPixelSize, dpr);
        reportStage("Stage 1", QStringLiteral("stage1"),
                    computeAndSaveDiff(cpuImg, gpuImg, outDir, QStringLiteral("stage1")), 1, 0.05);
    }

    // =========================================================================
    // STAGE 2: Analytical SDF Rounded Boxes, Borders, Gradients, erf Shadows & GPU Anim
    // =========================================================================
    {
        QPhaseSceneArena arena;
        arena.setClearColor(QColor(17, 20, 28, 255));

        QPhaseView root;
        root.setGeometry(QRectF(0, 0, logicalSize.width(), logicalSize.height()));

        auto *heroCard = root.add<QPhaseBox>();
        heroCard->setGeometry(QRectF(32, 28, 410, 170));
        heroCard->setBoxStyle({
            .radius = {18.0f, 18.0f, 18.0f, 6.0f},
            .fill = QPhaseFill::linearGradient({0, 0}, {410, 170}, QColor(43, 108, 176), QColor(107, 70, 193)),
            .border = { .width = 1.5f, .color = QColor(255, 255, 255, 90) },
            .shadow = { .blurRadius = 22.0f, .spread = 2.0f, .offset = {0, 8}, .color = QColor(0, 0, 0, 165) }
        });

        auto *clippedPanel = root.add<QPhaseBox>();
        clippedPanel->setGeometry(QRectF(32, 224, 410, 164));
        clippedPanel->setClipsToBounds(true, QPhaseCorners::all(16.0f));
        clippedPanel->setBoxStyle({
            .radius = QPhaseCorners::all(16.0f),
            .fill = QPhaseFill::solid(QColor(28, 33, 46)),
            .border = { .width = 1.0f, .color = QColor(255, 255, 255, 45) },
            .shadow = { .blurRadius = 14.0f, .offset = {0, 5}, .color = QColor(0, 0, 0, 130) }
        });

        // Child pill intentionally overhanging rounded clip corners to test analytical rounded clipping
        auto *overhangBar = clippedPanel->add<QPhaseBox>();
        overhangBar->setGeometry(QRectF(-20, -10, 260, 72));
        overhangBar->setBoxStyle({
            .radius = QPhaseCorners::all(12.0f),
            .fill = QPhaseFill::linearGradient({0, 0}, {260, 72}, QColor(56, 178, 172), QColor(49, 130, 206))
        });

        // GPU-timestamp animated pulse badge evaluated at identical timestamp on CPU & GPU
        auto *pulseCard = root.add<QPhaseBox>();
        pulseCard->setGeometry(QRectF(472, 28, 216, 360));
        pulseCard->setBoxStyle({
            .radius = QPhaseCorners::all(20.0f),
            .fill = QPhaseFill::linearGradient({0, 0}, {0, 360}, QColor(237, 137, 54), QColor(229, 62, 62)),
            .border = { .width = 2.0f, .color = QColor(255, 255, 255, 110) },
            .shadow = { .blurRadius = 20.0f, .offset = {0, 10}, .color = QColor(229, 62, 62, 95) }
        });
        pulseCard->setAnimation({
            .mode = QPhaseAnimation::Mode::CosinePulseInfinite,
            .fromOpacity = 1.0f,
            .toOpacity = 0.35f,
            .durationMs = 1000
        });

        arena.syncRootView(&root);
        const uint64_t evalTimeNs = arena.monotonicTimestampNs() + 250'000'000ULL;

        const QImage cpuImg = cpu.renderToImage(arena, targetPixelSize, dpr, evalTimeNs);
        const QImage gpuImg = gpu.renderOffscreenToImage(arena, targetPixelSize, dpr, evalTimeNs);
        reportStage("Stage 2", QStringLiteral("stage2"),
                    computeAndSaveDiff(cpuImg, gpuImg, outDir, QStringLiteral("stage2")), 1, 0.05);
    }

    // =========================================================================
    // STAGE 3: Bindless Descriptor-Indexed Images + CPU RAM Drop on Cutover
    // =========================================================================
    {
        QPhaseSceneArena arena;
        arena.setClearColor(QColor(18, 22, 31, 255));

        QPhaseView root;
        root.setGeometry(QRectF(0, 0, logicalSize.width(), logicalSize.height()));

        auto *frameCard = root.add<QPhaseBox>();
        frameCard->setGeometry(QRectF(28, 26, 664, 368));
        frameCard->setBoxStyle({
            .radius = QPhaseCorners::all(18.0f),
            .fill = QPhaseFill::solid(QColor(29, 35, 48)),
            .border = { .width = 1.0f, .color = QColor(255, 255, 255, 40) },
            .shadow = { .blurRadius = 18.0f, .offset = {0, 6}, .color = QColor(0, 0, 0, 140) }
        });

        const QImage heroTex = createProceduralGradientGridImage(256, 180);
        const QImage orbTex = createTranslucentOrbImage(128);

        auto *imgView = frameCard->add<QPhaseImage>(heroTex);
        imgView->setGeometry(QRectF(24, 24, 410, 320));
        imgView->setCornerRadius(QPhaseCorners::all(14.0f));

        auto *orbView = frameCard->add<QPhaseImage>(orbTex);
        orbView->setGeometry(QRectF(468, 72, 164, 164));
        orbView->setCornerRadius(QPhaseCorners::all(82.0f));

        arena.syncRootView(&root);

        const size_t cpuBytesBefore = arena.activeCpuImageBytes();
        const QImage cpuImg = cpu.renderToImage(arena, targetPixelSize, dpr);
        const QImage gpuImg = gpu.renderOffscreenToImage(arena, targetPixelSize, dpr);

        // Simulate irreversible GPU steady-state cutover and verify CPU QImage pixel memory is freed!
        arena.markEnteredGpuSteadyState();
        const size_t cpuBytesAfter = arena.activeCpuImageBytes();
        const QImage gpuAfterDrop = gpu.renderOffscreenToImage(arena, targetPixelSize, dpr);

        std::printf("          [Stage 3 Cutover RAM Check] activeCpuImageBytes: %zu B -> %zu B after cutover\n",
                    cpuBytesBefore, cpuBytesAfter);
        if (cpuBytesBefore == 0 || cpuBytesAfter != 0 || gpuAfterDrop != gpuImg) {
            std::fprintf(stderr, "FAIL: Stage 3 CPU image memory reclamation or post-drop GPU render failed!\n");
            allPassed = false;
        }

        reportStage("Stage 3", QStringLiteral("stage3"),
                    computeAndSaveDiff(cpuImg, gpuImg, outDir, QStringLiteral("stage3")), 1, 0.05);
    }

    // =========================================================================
    // STAGE 4: HarfBuzz 15 hb-raster (CPU) <-> hb-gpu Slug Curves (Text + SVG)
    // =========================================================================
    {
        QPhaseSceneArena arena;
        arena.setClearColor(QColor(16, 19, 27, 255));

        QPhaseView root;
        root.setGeometry(QRectF(0, 0, logicalSize.width(), logicalSize.height()));

        auto *card = root.add<QPhaseBox>();
        card->setGeometry(QRectF(24, 24, 672, 372));
        card->setBoxStyle({
            .radius = QPhaseCorners::all(16.0f),
            .fill = QPhaseFill::linearGradient({0, 0}, {0, 372}, QColor(28, 33, 46), QColor(22, 26, 37)),
            .border = { .width = 1.0f, .color = QColor(255, 255, 255, 36) },
            .shadow = { .blurRadius = 16.0f, .offset = {0, 6}, .color = QColor(0, 0, 0, 140) }
        });

        // Crisp SVG vector icons evaluated directly via quadratic Bezier winding in VRAM
        const QString boltSvg = QStringLiteral("M13 2 L4 14 H11 L10 22 L20 10 H13 Z");
        const QString shieldSvg = QStringLiteral("M12 2 L20 5 V11 C20 16.5 16.5 20.7 12 22 C7.5 20.7 4 16.5 4 11 V5 Z");

        auto *icon1 = card->add<QPhaseIcon>(boltSvg, QRectF(0, 0, 24, 24), QColor(246, 224, 94));
        icon1->setGeometry(QRectF(28, 28, 44, 44));

        auto *icon2 = card->add<QPhaseIcon>(shieldSvg, QRectF(0, 0, 24, 24), QColor(104, 211, 145));
        icon2->setGeometry(QRectF(596, 28, 48, 48));

        auto *h1 = card->add<QPhaseLabel>(QStringLiteral("HarfBuzz 15 GPU Curve Typography"));
        QFont fH1;
        fH1.setPixelSize(30);
        fH1.setWeight(QFont::Bold);
        h1->setFont(fH1);
        h1->setTextColor(QColor(247, 250, 252));
        h1->setGeometry(QRectF(84, 26, 500, 44));

        auto *sub = card->add<QPhaseLabel>(
                QStringLiteral("Zero glyph atlases • Unscaled BDA Bezier outlines • Infinite HiDPI resolution"));
        QFont fSub;
        fSub.setPixelSize(15);
        sub->setFont(fSub);
        sub->setTextColor(QColor(144, 205, 244));
        sub->setGeometry(QRectF(32, 88, 610, 28));

        const int sizes[4] = { 12, 16, 22, 34 };
        qreal y = 132.0;
        for (int px : sizes) {
            auto *lbl = card->add<QPhaseLabel>(
                    QStringLiteral("[%1px] Pack my box with five dozen liquor jugs — 0123456789 & AV Wa fi").arg(px));
            QFont f;
            f.setPixelSize(px);
            lbl->setFont(f);
            lbl->setTextColor(QColor(226, 232, 240));
            lbl->setGeometry(QRectF(32, y, 616, px + 14.0));
            y += px + 20.0;
        }

        arena.syncRootView(&root);

        const QImage cpuImg = cpu.renderToImage(arena, targetPixelSize, dpr);
        const QImage gpuImg = gpu.renderOffscreenToImage(arena, targetPixelSize, dpr);

        arena.markEnteredGpuSteadyState();
        std::printf("          [Stage 4 Curve Arena] Encoded %zu unique glyph/SVG blobs (%zu B in BDA VRAM)\n",
                    arena.curves().size() - 1, arena.curveTexels().size() * sizeof(uint64_t));

        // CPU boot uses HarfBuzz 15 hb-raster scanline coverage; Vulkan steady state uses
        // 4x Rotated-Grid Supersampled hb-gpu curve evaluation!
        reportStage("Stage 4", QStringLiteral("stage4"),
                    computeAndSaveDiff(cpuImg, gpuImg, outDir, QStringLiteral("stage4")), 40, 0.12);
    }

    // =========================================================================
    // STAGE 5: High-Level Widgets (BoxLayout, Label, TextEdit, Virtualized TableView)
    // =========================================================================
    {
        QPhaseSceneArena arena;
        arena.setClearColor(QColor(15, 18, 25, 255));

        SampleParityTableModel model;

        QPhaseView root;
        root.setGeometry(QRectF(0, 0, logicalSize.width(), logicalSize.height()));
        root.setBoxLayout(Qt::Vertical, QMarginsF(20, 18, 20, 18), 12.0);

        auto *topBar = root.add<QPhaseView>();
        topBar->setPreferredSize(QSizeF(680, 40));
        topBar->setMaximumSize(QSizeF(2000, 40));
        topBar->setBoxLayout(Qt::Horizontal, {}, 12.0);

        auto *bolt = topBar->add<QPhaseIcon>(
                QStringLiteral("M13 2 L4 14 H11 L10 22 L20 10 H13 Z"),
                QRectF(0, 0, 24, 24), QColor(99, 179, 237));
        bolt->setMinimumSize(QSizeF(28, 28));
        bolt->setPreferredSize(QSizeF(28, 28));
        bolt->setMaximumSize(QSizeF(28, 28));

        auto *title = topBar->add<QPhaseLabel>(QStringLiteral("QtPhase Widget Suite"));
        QFont tf;
        tf.setPixelSize(18);
        tf.setWeight(QFont::DemiBold);
        title->setFont(tf);
        title->setPreferredSize(QSizeF(210, 36));

        auto *searchEdit = topBar->add<QPhaseTextEdit>();
        searchEdit->setText(QStringLiteral("filter: subsystem == hb-gpu"));
        searchEdit->setStretch(1);

        auto *table = root.add<QPhaseTableView>();
        table->setStretch(1);
        table->setColumnWidths({ 100.0, 240.0, 180.0, 140.0 });
        table->setModel(&model);
        table->setSelectedRow(3);

        arena.syncRootView(&root);
        // Test fractional subpixel scroll offset via O(1) GPU TransformSlot!
        table->scrollToY(46.5);
        arena.syncRootView(&root);

        const uint64_t evalTimeNs = arena.monotonicTimestampNs();
        const QImage cpuImg = cpu.renderToImage(arena, targetPixelSize, dpr, evalTimeNs);
        const QImage gpuImg = gpu.renderOffscreenToImage(arena, targetPixelSize, dpr, evalTimeNs);

        reportStage("Stage 5", QStringLiteral("stage5"),
                    computeAndSaveDiff(cpuImg, gpuImg, outDir, QStringLiteral("stage5")), 40, 0.12);
    }

    // =========================================================================
    // STAGE 6: Hardened Desktop Pipeline (Shared VkDevice, Bindless Slot Recycling,
    //          2x2 Rotations, Exact Nested Rounded Clips, StrokeSegment Polylines,
    //          In-Register PushLayer/PopLayer + Screen Blend, & Stem Darkening)
    // =========================================================================
    {
        QElapsedTimer secInitTimer;
        secInitTimer.start();
        QPhaseVulkanBackend gpuSecondary;
        const bool secOk = gpuSecondary.initializeDevice(false);
        const qint64 secInitUs = secInitTimer.nsecsElapsed() / 1000;

        std::printf("          [Stage 6 Shared Device] Secondary QPhaseVulkanBackend init: %lld us "
                    "(cacheHit=%d, VkDevice=0x%llx, refCount=%u)\n",
                    static_cast<long long>(secInitUs),
                    gpuSecondary.usedSharedDeviceCache() ? 1 : 0,
                    static_cast<unsigned long long>(gpuSecondary.sharedDeviceHandle()),
                    gpu.sharedDeviceUseCount());

        if (!secOk || !gpuSecondary.usedSharedDeviceCache()
            || gpuSecondary.sharedDeviceHandle() != gpu.sharedDeviceHandle()
            || gpu.sharedDeviceUseCount() < 2) {
            std::fprintf(stderr, "FAIL: Stage 6 shared QPhaseSharedVulkanDevice singleton check failed!\n");
            allPassed = false;
        }

        QPhaseSceneArena arena;
        arena.setClearColor(QColor(15, 18, 26, 255));
        arena.setTextStemDarkening(0.35f);

        // Verify bindless texture slot free-list recycling + zero-stall staging replacement
        const QImage tempImg = createProceduralGradientGridImage(64, 64);
        const QImage keepImg = createProceduralGradientGridImage(96, 72);
        const uint32_t slot0 = arena.registerOrGetImage(tempImg);
        const uint32_t slot1 = arena.registerOrGetImage(keepImg);
        Q_UNUSED(slot1);
        gpuSecondary.benchmarkFrameOffscreen(arena, QSize(64, 64), dpr, false, 0, nullptr);

        arena.unregisterImage(slot0);
        const QImage recycledOrb = createTranslucentOrbImage(96);
        const uint32_t recycledSlot = arena.registerOrGetImage(recycledOrb);

        std::printf("          [Stage 6 Bindless Free-List] Freed slot %u -> recycled slot %u "
                    "(tableSize=%zu, activeSlots=%zu)\n",
                    slot0, recycledSlot, arena.images().size(), arena.activeImageSlotCount());

        if (recycledSlot != slot0 || arena.images().size() != 2 || arena.activeImageSlotCount() != 2) {
            std::fprintf(stderr, "FAIL: Stage 6 bindless image slot recycling invariant failed!\n");
            allPassed = false;
        }

        QPhaseView root;
        root.setGeometry(QRectF(0, 0, logicalSize.width(), logicalSize.height()));

        // 1. Exact Nested Rounded-Clip Chain + Analytical StrokeSegment Polylines
        auto *outerClipCard = root.add<QPhaseBox>();
        outerClipCard->setGeometry(QRectF(24, 22, 324, 176));
        outerClipCard->setClipsToBounds(true, QPhaseCorners::all(26.0f));
        outerClipCard->setBoxStyle({
            .radius = QPhaseCorners::all(26.0f),
            .fill = QPhaseFill::solid(QColor(26, 32, 46)),
            .border = { .width = 1.25f, .color = QColor(255, 255, 255, 45) }
        });

        auto *innerClipCard = outerClipCard->add<QPhaseBox>();
        innerClipCard->setGeometry(QRectF(14, 14, 296, 148));
        innerClipCard->setClipsToBounds(true, QPhaseCorners::all(20.0f));
        innerClipCard->setBoxStyle({
            .radius = QPhaseCorners::all(20.0f),
            .fill = QPhaseFill::solid(QColor(33, 41, 59)),
            .border = { .width = 1.0f, .color = QColor(144, 205, 244, 65) }
        });

        auto *nestedOverhang = innerClipCard->add<QPhaseBox>();
        nestedOverhang->setGeometry(QRectF(-24, -16, 344, 64));
        nestedOverhang->setBoxStyle({
            .radius = QPhaseCorners::all(10.0f),
            .fill = QPhaseFill::linearGradient({0, 0}, {344, 64}, QColor(49, 130, 206), QColor(128, 90, 213))
        });

        auto *chart1 = innerClipCard->add<SparklineChartView>();
        chart1->setGeometry(QRectF(12, 54, 272, 84));

        // 2. Full 2x2 Affine Rotated Card + Rotated Vector Icon + Recycled Bindless Texture Slot
        auto *rotCard = root.add<QPhaseBox>();
        rotCard->setGeometry(QRectF(38, 226, 296, 156));
        rotCard->setRotationDegrees(-8.5f);
        rotCard->setBoxStyle({
            .radius = QPhaseCorners::all(18.0f),
            .fill = QPhaseFill::linearGradient({0, 0}, {296, 156}, QColor(44, 82, 130), QColor(40, 94, 97)),
            .border = { .width = 1.5f, .color = QColor(255, 255, 255, 95) }
        });

        auto *orbImgView = rotCard->add<QPhaseImage>(recycledOrb);
        orbImgView->setGeometry(QRectF(18, 22, 108, 108));
        orbImgView->setCornerRadius(QPhaseCorners::all(54.0f));

        auto *rotBolt = rotCard->add<QPhaseIcon>(
                QStringLiteral("M13 2 L4 14 H11 L10 22 L20 10 H13 Z"),
                QRectF(0, 0, 24, 24), QColor(246, 224, 94));
        rotBolt->setGeometry(QRectF(196, 36, 68, 68));
        rotBolt->setRotationDegrees(24.0f);

        // 3. In-Register Isolated PushLayer/PopLayer Group Opacity + Screen Blend Mode (0 MB VRAM!)
        auto *layerPanel = root.add<QPhaseBox>();
        layerPanel->setGeometry(QRectF(374, 22, 322, 204));
        layerPanel->setBoxStyle({
            .radius = QPhaseCorners::all(18.0f),
            .fill = QPhaseFill::linearGradient({0, 0}, {322, 204}, QColor(28, 34, 48), QColor(39, 34, 62)),
            .border = { .width = 1.0f, .color = QColor(255, 255, 255, 42) }
        });

        // Isolated group at 55% opacity: overlapping opaque children MUST NOT bleed through each other!
        auto *isoGroup = layerPanel->add<QPhaseView>();
        isoGroup->setGeometry(QRectF(18, 18, 210, 168));
        isoGroup->setOpacity(0.55f);
        isoGroup->setIsolatedGroup(true, QPhaseBlendMode::Normal);

        auto *backCard = isoGroup->add<QPhaseBox>();
        backCard->setGeometry(QRectF(8, 10, 132, 96));
        backCard->setBoxStyle({
            .radius = QPhaseCorners::all(14.0f),
            .fill = QPhaseFill::solid(QColor(229, 62, 62))
        });

        auto *frontCard = isoGroup->add<QPhaseBox>();
        frontCard->setGeometry(QRectF(62, 54, 138, 102));
        frontCard->setBoxStyle({
            .radius = QPhaseCorners::all(14.0f),
            .fill = QPhaseFill::solid(QColor(56, 161, 105)),
            .border = { .width = 2.0f, .color = QColor(255, 255, 255, 220) }
        });

        // Screen-blended highlight badge
        auto *screenGroup = layerPanel->add<QPhaseView>();
        screenGroup->setGeometry(QRectF(188, 34, 114, 136));
        screenGroup->setOpacity(0.85f);
        screenGroup->setIsolatedGroup(true, QPhaseBlendMode::Screen);

        auto *screenGlow = screenGroup->add<QPhaseBox>();
        screenGlow->setGeometry(QRectF(0, 0, 114, 136));
        screenGlow->setBoxStyle({
            .radius = QPhaseCorners::all(16.0f),
            .fill = QPhaseFill::linearGradient({0, 0}, {114, 136}, QColor(99, 179, 237), QColor(237, 100, 166))
        });

        // 4. Perceptual Stem-Darkened Typography + Secondary Sparkline
        auto *typoPanel = root.add<QPhaseBox>();
        typoPanel->setGeometry(QRectF(374, 242, 322, 154));
        typoPanel->setBoxStyle({
            .radius = QPhaseCorners::all(16.0f),
            .fill = QPhaseFill::solid(QColor(23, 28, 40)),
            .border = { .width = 1.0f, .color = QColor(255, 255, 255, 38) }
        });

        auto *stTitle = typoPanel->add<QPhaseLabel>(QStringLiteral("Stem-Darkened 2-Stage Binned UI"));
        QFont stFont;
        stFont.setPixelSize(17);
        stFont.setWeight(QFont::DemiBold);
        stTitle->setFont(stFont);
        stTitle->setTextColor(QColor(247, 250, 252));
        stTitle->setGeometry(QRectF(16, 12, 290, 28));

        auto *stSub = typoPanel->add<QPhaseLabel>(QStringLiteral("Shared VkDevice • 0 Bezier Uber-Frag • Layers"));
        QFont subFont;
        subFont.setPixelSize(12);
        stSub->setFont(subFont);
        stSub->setTextColor(QColor(160, 174, 192));
        stSub->setGeometry(QRectF(16, 42, 290, 22));

        auto *chart2 = typoPanel->add<SparklineChartView>();
        chart2->setGeometry(QRectF(14, 68, 294, 74));

        arena.syncRootView(&root);

        const uint64_t evalTimeNs = arena.monotonicTimestampNs();
        const QImage cpuImg = cpu.renderToImage(arena, targetPixelSize, dpr, evalTimeNs);
        const QImage gpuImg = gpuSecondary.renderOffscreenToImage(arena, targetPixelSize, dpr, evalTimeNs);

        reportStage("Stage 6", QStringLiteral("stage6"),
                    computeAndSaveDiff(cpuImg, gpuImg, outDir, QStringLiteral("stage6")), 40, 0.15);
    }

    // =========================================================================
    // STAGE 7: Generalized Per-Widget Renderer Affinity (Any / GpuOnly / CpuOnly),
    //          Blocking Policy Aggregation, & Custom Vulkan 3D/Depth QPhaseGpuView
    // =========================================================================
    {
        QPhaseSceneArena arena;
        arena.setClearColor(QColor(15, 18, 26, 255));

        QPhaseView root;
        root.setGeometry(QRectF(0, 0, logicalSize.width(), logicalSize.height()));

        // Shared background frame (Any affinity -> renders identically on CPU and GPU)
        auto *sharedPanel = root.add<QPhaseBox>();
        sharedPanel->setGeometry(QRectF(24, 24, 672, 372));
        sharedPanel->setBoxStyle({
            .radius = QPhaseCorners::all(18.0f),
            .fill = QPhaseFill::solid(QColor(28, 34, 48)),
            .border = { .width = 1.0f, .color = QColor(255, 255, 255, 42) }
        });

        // CpuOnly widget subtree (with child hb-raster label):
        // - Renders directly via QPhaseCpuRasterizer on CPU boot frames
        // - Automatically rasterized by QPhaseCpuRasterizer into a bridged QImage and uploaded
        //   via zero-stall staging when GpuOnly forces the window onto Vulkan!
        auto *cpuOnlyCard = sharedPanel->add<QPhaseBox>();
        cpuOnlyCard->setGeometry(QRectF(28, 32, 260, 140));
        cpuOnlyCard->setRendererAffinity(QPhaseRendererAffinity::CpuOnly, false);
        cpuOnlyCard->setBoxStyle({
            .radius = QPhaseCorners::all(14.0f),
            .fill = QPhaseFill::solid(QColor(56, 178, 120)),
            .border = { .width = 1.5f, .color = QColor(255, 255, 255, 160) }
        });

        auto *cpuSubLabel = cpuOnlyCard->add<QPhaseLabel>(QStringLiteral("CPU Rasterizer Bridge"));
        QFont cpuLblFont;
        cpuLblFont.setPixelSize(16);
        cpuLblFont.setWeight(QFont::Bold);
        cpuSubLabel->setFont(cpuLblFont);
        cpuSubLabel->setTextColor(QColor(15, 23, 42));
        cpuSubLabel->setGeometry(QRectF(18, 16, 224, 28));

        // Dedicated QPhaseGpuView (defaults to GpuOnly): executes custom Vulkan 1.3 dynamic rendering
        // with D32_SFLOAT depth attachment into a bindless slot, skipped cleanly on CPU boot frames!
        auto *gpu3dView = sharedPanel->add<QPhaseGpuView>();
        gpu3dView->setGeometry(QRectF(320, 32, 320, 308));
        gpu3dView->setDepthBufferEnabled(true);
        gpu3dView->setCornerRadius(QPhaseCorners::all(20.0f));
        gpu3dView->setRenderCallback([](const QPhaseGpuSurfaceContext &ctx) {
            auto cmd = static_cast<VkCommandBuffer>(ctx.vkCommandBuffer);
            const uint32_t w = uint32_t(ctx.pixelSize.width());
            const uint32_t h = uint32_t(ctx.pixelSize.height());

            VkRenderingAttachmentInfo colorAtt{};
            colorAtt.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            colorAtt.imageView = VkImageView(ctx.colorImageView);
            colorAtt.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            colorAtt.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            colorAtt.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            colorAtt.clearValue.color = {{ 0.14f, 0.32f, 0.78f, 1.0f }};

            VkRenderingAttachmentInfo depthAtt{};
            depthAtt.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            depthAtt.imageView = VkImageView(ctx.depthImageView);
            depthAtt.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            depthAtt.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            depthAtt.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            depthAtt.clearValue.depthStencil = { 1.0f, 0 };

            VkRenderingInfo ri{};
            ri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
            ri.renderArea = { {0, 0}, { w, h } };
            ri.layerCount = 1;
            ri.colorAttachmentCount = 1;
            ri.pColorAttachments = &colorAtt;
            ri.pDepthAttachment = ctx.depthImageView ? &depthAtt : nullptr;

            vkCmdBeginRendering(cmd, &ri);

            // Render inner vivid orange band inside the custom Vulkan render pass
            VkClearAttachment ca{};
            ca.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            ca.colorAttachment = 0;
            ca.clearValue.color = {{ 0.94f, 0.45f, 0.16f, 1.0f }};

            VkClearRect cr{};
            cr.rect = { { int(w / 5), int(h / 5) }, { (w * 3) / 5, (h * 3) / 5 } };
            cr.baseArrayLayer = 0;
            cr.layerCount = 1;
            vkCmdClearAttachments(cmd, 1, &ca, 1, &cr);

            vkCmdEndRendering(cmd);
        });

        // 2D HUD pill (Any affinity) stacked ON TOP of the 3D QPhaseGpuView in the same single pass!
        auto *hudOverlay = sharedPanel->add<QPhaseBox>();
        hudOverlay->setGeometry(QRectF(344, 276, 272, 44));
        hudOverlay->setBoxStyle({
            .radius = QPhaseCorners::all(12.0f),
            .fill = QPhaseFill::solid(QColor(15, 23, 42, 210)),
            .border = { .width = 1.0f, .color = QColor(255, 255, 255, 90) }
        });

        arena.syncRootView(&root);
        const PhaseRendererRequirements reqsNonBlock = arena.rendererRequirements();

        gpu3dView->setBackendBlocking(true);
        arena.syncRootView(&root);
        const PhaseRendererRequirements reqsBlock = arena.rendererRequirements();

        const QImage cpuBootImg = cpu.renderToImage(arena, targetPixelSize, dpr);
        const QImage gpuFullImg = gpu.renderOffscreenToImage(arena, targetPixelSize, dpr);
        QDir().mkpath(outDir);
        cpuBootImg.save(outDir + u"/stage7_cpu_boot.png");
        gpuFullImg.save(outDir + u"/stage7_gpu_full.png");

        auto pxAtLogical = [&](const QImage &img, double lx, double ly) -> QColor {
            const int px = std::clamp(int(std::lround(lx * dpr)), 0, img.width() - 1);
            const int py = std::clamp(int(std::lround(ly * dpr)), 0, img.height() - 1);
            return img.pixelColor(px, py);
        };

        const QColor bgRef = QColor(28, 34, 48);
        const QColor cpuAtCpuCard = pxAtLogical(cpuBootImg, 150, 120);
        const QColor gpuAtCpuCard = pxAtLogical(gpuFullImg, 150, 120);
        const QColor cpuAtGpu3d   = pxAtLogical(cpuBootImg, 504, 180);
        const QColor gpuAtGpu3d   = pxAtLogical(gpuFullImg, 504, 180);

        const bool reqsOk = reqsNonBlock.forcesGpu()
                && reqsNonBlock.hasConflict()
                && !reqsNonBlock.blocksUntilGpuReady()
                && reqsBlock.blocksUntilGpuReady();
        const bool cpuBootOk = (cpuAtCpuCard.green() > 150)
                && (std::abs(cpuAtGpu3d.red() - bgRef.red()) <= 2);
        // On Vulkan steady state, BOTH the CpuOnly widget (bridged via QPhaseCpuRasterizer -> staging QImage!)
        // AND the GpuOnly 3D widget render visibly side-by-side!
        const bool gpuBridgeCoexistOk = (gpuAtCpuCard.green() > 150)
                && (std::abs(gpuAtCpuCard.green() - cpuAtCpuCard.green()) <= 2)
                && (gpuAtGpu3d.red() > 210 && gpuAtGpu3d.blue() < 80);

        const bool stage7Ok = reqsOk && cpuBootOk && gpuBridgeCoexistOk;
        std::printf("[Stage 7] %-4s  Affinity, QPhaseGpuView & CPU-to-GPU Conflict Bridge:\n"
                    "          reqs(forcesGpu=%d, conflict=%d, nonBlock=%d -> block=%d, bridges=%zu)\n"
                    "          CPU boot  : CpuOnly=(%d,%d,%d) [direct CPU],  GpuOnly=(%d,%d,%d) [skipped!]\n"
                    "          GPU steady: CpuOnly=(%d,%d,%d) [CPU->GPU bridge!], QPhaseGpuView=(%d,%d,%d) [3D pass!]\n",
                    stage7Ok ? "PASS" : "FAIL",
                    reqsNonBlock.forcesGpu() ? 1 : 0,
                    reqsNonBlock.hasConflict() ? 1 : 0,
                    reqsNonBlock.blocksUntilGpuReady() ? 1 : 0,
                    reqsBlock.blocksUntilGpuReady() ? 1 : 0,
                    arena.cpuBridges().size(),
                    cpuAtCpuCard.red(), cpuAtCpuCard.green(), cpuAtCpuCard.blue(),
                    cpuAtGpu3d.red(), cpuAtGpu3d.green(), cpuAtGpu3d.blue(),
                    gpuAtCpuCard.red(), gpuAtCpuCard.green(), gpuAtCpuCard.blue(),
                    gpuAtGpu3d.red(), gpuAtGpu3d.green(), gpuAtGpu3d.blue());
        if (!stage7Ok)
            allPassed = false;
    }

    if (!allPassed) {
        std::fprintf(stderr, "FAIL: One or more QtPhase parity suites failed!\n");
        return 2;
    }

    std::printf("\nALL 7 QTPHASE STAGES PASSED CPU <-> VULKAN 1.3 VERIFICATION!\n");
    return 0;
}
