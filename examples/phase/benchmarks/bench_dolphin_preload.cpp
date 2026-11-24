#include <QApplication>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QKeyEvent>
#include <QMainWindow>
#include <QTimer>
#include <QWidget>
#include <QWindow>
#include <cstdio>
#include <ctime>
#include <vector>
#include <algorithm>

namespace {

static double realtimeMs()
{
    struct timespec ts{};
    clock_gettime(CLOCK_REALTIME, &ts);
    return double(ts.tv_sec % 10000) * 1000.0 + double(ts.tv_nsec) / 1e6;
}

static double g_launchRealtimeMs = 0.0;
static QElapsedTimer g_bootTimer;

__attribute__((constructor))
static void onLibraryLoad()
{
    g_launchRealtimeMs = realtimeMs();
    g_bootTimer.start();
    std::fprintf(stderr, "[BENCH] launch_rt=%.3f\n", g_launchRealtimeMs);
    std::fflush(stderr);
}

static long readVmHwmKb()
{
    QFile f(QStringLiteral("/proc/self/status"));
    if (!f.open(QIODevice::ReadOnly))
        return 0;
    while (!f.atEnd()) {
        const QByteArray line = f.readLine();
        if (line.startsWith("VmHWM:")) {
            const auto parts = line.simplified().split(' ');
            if (parts.size() >= 2)
                return parts[1].toLong();
        }
    }
    return 0;
}

class DolphinBenchDriver : public QObject
{
public:
    DolphinBenchDriver()
    {
        m_findTimer = new QTimer(this);
        m_findTimer->setInterval(50);
        connect(m_findTimer, &QTimer::timeout, this, &DolphinBenchDriver::pollForWindow);
        m_findTimer->start();
    }

private:
    void pollForWindow()
    {
        for (QWidget *w : QApplication::topLevelWidgets()) {
            if (auto *mw = qobject_cast<QMainWindow *>(w); mw && mw->isVisible() && mw->windowHandle() && mw->windowHandle()->isExposed()) {
                m_win = mw;
                m_findTimer->stop();
                std::fprintf(stderr, "[BENCH] window_exposed_ms=%.2f rt=%.3f size=%dx%d dpr=%.2f\n",
                             double(g_bootTimer.nsecsElapsed()) / 1e6,
                             realtimeMs(),
                             mw->width(), mw->height(),
                             mw->devicePixelRatioF());
                std::fflush(stderr);
                // Wait for KIO directory listing to finish populating the view
                QTimer::singleShot(900, this, &DolphinBenchDriver::startResizeBench);
                return;
            }
        }
    }

    void startResizeBench()
    {
        if (!m_win)
            return;
        m_sizes = {
            {960, 640}, {1040, 680}, {1120, 720}, {1200, 760}, {1280, 800},
            {1360, 840}, {1440, 880}, {1320, 820}, {1200, 760}, {1080, 700},
            {960, 640}, {1100, 710}, {1240, 780}, {1380, 850}, {1480, 900},
            {1340, 830}, {1180, 750}, {1020, 670}, {1160, 740}, {1280, 800}
        };
        m_step = 0;
        m_resizeTimer = new QTimer(this);
        m_resizeTimer->setInterval(45);
        connect(m_resizeTimer, &QTimer::timeout, this, &DolphinBenchDriver::doResizeStep);
        m_resizeTimer->start();
    }

    void doResizeStep()
    {
        if (!m_win || m_step >= int(m_sizes.size())) {
            m_resizeTimer->stop();
            QTimer::singleShot(200, this, &DolphinBenchDriver::startKeyNavBench);
            return;
        }
        const QSize sz = m_sizes[size_t(m_step)];
        QElapsedTimer t;
        t.start();
        const double rtStart = realtimeMs();
        m_win->resize(sz);
        QCoreApplication::processEvents();
        const double elapsedMs = double(t.nsecsElapsed()) / 1e6;
        m_resizeMs.push_back(elapsedMs);
        std::fprintf(stderr, "[BENCH] resize step=%02d sz=%dx%d wall_ms=%.2f rt=%.3f\n",
                     m_step, sz.width(), sz.height(), elapsedMs, rtStart);
        std::fflush(stderr);
        ++m_step;
    }

    void startKeyNavBench()
    {
        m_keyStep = 0;
        m_keyTimer = new QTimer(this);
        m_keyTimer->setInterval(45);
        connect(m_keyTimer, &QTimer::timeout, this, &DolphinBenchDriver::doKeyStep);
        m_keyTimer->start();
    }

    void doKeyStep()
    {
        if (!m_win || m_keyStep >= 20) {
            m_keyTimer->stop();
            QTimer::singleShot(150, this, &DolphinBenchDriver::finishBench);
            return;
        }
        QWidget *target = QApplication::focusWidget();
        if (!target)
            target = m_win;
        QElapsedTimer t;
        t.start();
        const double rtStart = realtimeMs();
        QKeyEvent ev(QEvent::KeyPress, Qt::Key_Down, Qt::NoModifier);
        QApplication::sendEvent(target, &ev);
        QCoreApplication::processEvents();
        const double elapsedMs = double(t.nsecsElapsed()) / 1e6;
        m_keyMs.push_back(elapsedMs);
        std::fprintf(stderr, "[BENCH] keynav step=%02d wall_ms=%.2f rt=%.3f\n",
                     m_keyStep, elapsedMs, rtStart);
        std::fflush(stderr);
        ++m_keyStep;
    }

    void finishBench()
    {
        auto summarize = [](const char *label, std::vector<double> v) {
            if (v.empty())
                return;
            std::sort(v.begin(), v.end());
            const double p50 = v[v.size() / 2];
            const double p90 = v[(v.size() * 9) / 10];
            std::fprintf(stderr, "[BENCH_SUMMARY] %s n=%zu min=%.2f p50=%.2f p90=%.2f max=%.2f\n",
                         label, v.size(), v.front(), p50, p90, v.back());
        };
        summarize("resize_wall_ms", m_resizeMs);
        summarize("keynav_wall_ms", m_keyMs);
        std::fprintf(stderr, "[BENCH_SUMMARY] peak_rss_mb=%.1f\n", double(readVmHwmKb()) / 1024.0);
        std::fflush(stderr);
        QCoreApplication::quit();
    }

    QTimer *m_findTimer = nullptr;
    QTimer *m_resizeTimer = nullptr;
    QTimer *m_keyTimer = nullptr;
    QMainWindow *m_win = nullptr;
    std::vector<QSize> m_sizes;
    std::vector<double> m_resizeMs;
    std::vector<double> m_keyMs;
    int m_step = 0;
    int m_keyStep = 0;
};

static void installBenchDriver()
{
    if (qEnvironmentVariableIntValue("QT_DOLPHIN_BENCH") > 0)
        new DolphinBenchDriver();
}

Q_COREAPP_STARTUP_FUNCTION(installBenchDriver)

} // namespace
