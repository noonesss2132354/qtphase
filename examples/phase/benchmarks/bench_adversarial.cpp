#include <QApplication>
#include <QElapsedTimer>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <QPlainTextEdit>
#include <QRadialGradient>
#include <QStackedWidget>
#include <QStandardItemModel>
#include <QTimer>
#include <QTreeView>
#include <QVBoxLayout>
#include <QWidget>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

// Adversarial Widget #1: Forces heavy dynamic emitRasterFallback() every single frame
// (complex radial gradient + cubic bezier waves + dashed stroke, changing phase every frame!)
class DynamicFallbackCanvas : public QWidget
{
public:
    explicit DynamicFallbackCanvas(QWidget *parent = nullptr) : QWidget(parent) {}
    void advance() { ++m_tick; update(); }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.fillRect(rect(), QColor(24, 27, 34));

        const qreal w = width();
        const qreal h = height();
        const qreal phase = double(m_tick) * 0.25;

        // Multi-stop radial gradient + complex cubic path -> guaranteed emitRasterFallback()
        QRadialGradient grad(w * 0.5 + std::sin(phase) * 120.0, h * 0.5, std::min(w, h) * 0.45);
        grad.setColorAt(0.0, QColor::fromHsv((m_tick * 13) % 360, 200, 240));
        grad.setColorAt(0.5, QColor::fromHsv((m_tick * 13 + 90) % 360, 180, 180));
        grad.setColorAt(1.0, QColor(24, 27, 34, 220));

        QPainterPath wave;
        wave.moveTo(40.0, h * 0.5);
        for (int i = 1; i <= 12; ++i) {
            const qreal x0 = 40.0 + (w - 80.0) * (double(i - 1) / 12.0);
            const qreal x1 = 40.0 + (w - 80.0) * (double(i) / 12.0);
            const qreal y1 = h * 0.5 + std::sin(phase + i * 0.8) * (h * 0.32);
            const qreal y2 = h * 0.5 + std::cos(phase + i * 0.6) * (h * 0.32);
            wave.cubicTo((x0 + x1) * 0.5, y1, (x0 + x1) * 0.5, y2, x1, h * 0.5 + std::sin(phase + i) * 40.0);
        }
        wave.lineTo(w - 40.0, h - 40.0);
        wave.lineTo(40.0, h - 40.0);
        wave.closeSubpath();

        QPen dashed(QColor(240, 245, 255), 2.5, Qt::DashDotLine);
        p.setPen(dashed);
        p.setBrush(grad);
        p.drawPath(wave);
    }

private:
    int m_tick = 0;
};

int main(int argc, char **argv)
{
    QApplication app(argc, argv);

    QWidget win;
    win.setObjectName(QStringLiteral("AdversarialBenchWin"));
    win.resize(1100, 720);

    auto *rootLay = new QVBoxLayout(&win);
    rootLay->setContentsMargins(0, 0, 0, 0);
    auto *stack = new QStackedWidget(&win);
    rootLay->addWidget(stack);

    // Page 0: Split QTreeView + QPlainTextEdit (for Resize + Sparse Typing/Idle)
    auto *splitPage = new QWidget;
    auto *splitLay = new QHBoxLayout(splitPage);
    auto *tree = new QTreeView;
    auto *model = new QStandardItemModel(40, 3, tree);
    model->setHorizontalHeaderLabels({QStringLiteral("File"), QStringLiteral("Size"), QStringLiteral("Kind")});
    for (int r = 0; r < 40; ++r) {
        model->setItem(r, 0, new QStandardItem(QStringLiteral("module_%1.cpp").arg(r + 1, 2, 10, QLatin1Char('0'))));
        model->setItem(r, 1, new QStandardItem(QStringLiteral("%1 KB").arg((r + 1) * 19)));
        model->setItem(r, 2, new QStandardItem(QStringLiteral("C++ Source")));
    }
    tree->setModel(model);
    tree->setAlternatingRowColors(true);
    splitLay->addWidget(tree, 1);

    auto *editor = new QPlainTextEdit;
    QString code;
    for (int i = 0; i < 80; ++i) {
        code += QStringLiteral("int computeTileScore_%1(const PhaseMetrics &m) { return m.cpuUs < m.gpuUs ? %1 : -%1; }\n").arg(i);
    }
    editor->setPlainText(code);
    splitLay->addWidget(editor, 2);
    stack->addWidget(splitPage);

    // Page 1: Dynamic Fallback Canvas (worst-case GPU upload churn!)
    auto *canvas = new DynamicFallbackCanvas;
    stack->addWidget(canvas);

    win.show();

    std::vector<double> resizeMs, sparseMs, fallbackMs, popupMs;
    auto summarize = [](const char *name, std::vector<double> v) {
        if (v.empty())
            return;
        std::sort(v.begin(), v.end());
        double sum = 0.0;
        for (double x : v) sum += x;
        std::fprintf(stderr, "[ADV_SUMMARY] %s n=%zu min=%.2f p50=%.2f p90=%.2f max=%.2f mean=%.2f\n",
                     name, v.size(), v.front(), v[v.size() / 2], v[(v.size() * 9) / 10], v.back(), sum / double(v.size()));
        std::fflush(stderr);
    };

    // Phase 1: Interactive Resize (20 steps @ 35 ms)
    const std::vector<QSize> sizes = {
        {960, 640}, {1040, 680}, {1120, 720}, {1200, 760}, {1280, 800},
        {1360, 840}, {1440, 880}, {1320, 820}, {1200, 760}, {1080, 700},
        {960, 640}, {1100, 710}, {1240, 780}, {1380, 850}, {1480, 900},
        {1340, 830}, {1180, 750}, {1020, 670}, {1160, 740}, {1200, 760}
    };

    int phase = 0;
    int step = 0;
    auto *timer = new QTimer(&app);

    QObject::connect(timer, &QTimer::timeout, [&]() {
        if (phase == 0) {
            if (step < int(sizes.size())) {
                QElapsedTimer t;
                t.start();
                win.resize(sizes[size_t(step)]);
                QCoreApplication::processEvents();
                resizeMs.push_back(double(t.nsecsElapsed()) / 1e6);
                ++step;
            } else {
                // Phase 2: Sparse typing / idle cursor updates (16 steps @ 180 ms)
                phase = 1;
                step = 0;
                editor->setFocus();
                timer->setInterval(180);
            }
        } else if (phase == 1) {
            if (step < 16) {
                QElapsedTimer t;
                t.start();
                const QString ch = QString(QChar('a' + (step % 26)));
                QKeyEvent kp(QEvent::KeyPress, Qt::Key_A + (step % 26), Qt::NoModifier, ch);
                QApplication::sendEvent(editor, &kp);
                QCoreApplication::processEvents();
                sparseMs.push_back(double(t.nsecsElapsed()) / 1e6);
                ++step;
            } else {
                // Phase 3: Heavy Dynamic Fallback Canvas (25 frames @ 33 ms)
                phase = 2;
                step = 0;
                stack->setCurrentIndex(1);
                QCoreApplication::processEvents();
                timer->setInterval(33);
            }
        } else if (phase == 2) {
            if (step < 25) {
                QElapsedTimer t;
                t.start();
                canvas->advance();
                QCoreApplication::processEvents();
                fallbackMs.push_back(double(t.nsecsElapsed()) / 1e6);
                ++step;
            } else {
                // Phase 4: Rapid Popup Churn (15 sequential Qt::Popup menus @ 40 ms)
                phase = 3;
                step = 0;
                stack->setCurrentIndex(0);
                timer->setInterval(45);
            }
        } else if (phase == 3) {
            if (step < 15) {
                QElapsedTimer t;
                t.start();
                QMenu menu(&win);
                menu.addAction(QStringLiteral("Open File"));
                menu.addAction(QStringLiteral("Split Vertically"));
                menu.addAction(QStringLiteral("Rename Symbol"));
                menu.addSeparator();
                menu.addAction(QStringLiteral("Properties"));
                menu.popup(win.mapToGlobal(QPoint(120 + step * 10, 120 + step * 6)));
                QCoreApplication::processEvents();
                menu.hide();
                QCoreApplication::processEvents();
                popupMs.push_back(double(t.nsecsElapsed()) / 1e6);
                ++step;
            } else {
                timer->stop();
                summarize("resize_ms", resizeMs);
                summarize("sparse_typing_ms", sparseMs);
                summarize("dynamic_fallback_ms", fallbackMs);
                summarize("popup_churn_ms", popupMs);
                QTimer::singleShot(120, &app, &QApplication::quit);
            }
        }
    });

    QTimer::singleShot(350, [timer]() {
        timer->setInterval(35);
        timer->start();
    });

    return app.exec();
}
