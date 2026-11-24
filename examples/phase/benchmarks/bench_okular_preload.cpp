#include <QAbstractScrollArea>
#include <QApplication>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QKeyEvent>
#include <QMainWindow>
#include <QScrollBar>
#include <QTimer>
#include <QWheelEvent>
#include <QWidget>
#include <QWindow>
#include <algorithm>
#include <cstdio>
#include <ctime>
#include <vector>

namespace {

static double realtimeMs()
{
    struct timespec ts{};
    clock_gettime(CLOCK_REALTIME, &ts);
    return double(ts.tv_sec % 10000) * 1000.0 + double(ts.tv_nsec) / 1e6;
}

static QElapsedTimer g_bootTimer;

__attribute__((constructor))
static void onLibraryLoad()
{
    g_bootTimer.start();
    std::fprintf(stderr, "[BENCH] launch_rt=%.3f\n", realtimeMs());
    std::fflush(stderr);
}

static long readVmHwmKb()
{
    QFile f(QStringLiteral("/proc/self/status"));
    if (!f.open(QIODevice::ReadOnly))
        return 0;
    const QByteArray all = f.readAll();
    for (const QByteArray &line : all.split('\n')) {
        if (line.startsWith("VmHWM:")) {
            const auto parts = line.simplified().split(' ');
            if (parts.size() >= 2)
                return parts[1].toLong();
        }
    }
    return 0;
}

class OkularBenchDriver : public QObject
{
public:
    OkularBenchDriver()
    {
        m_findTimer = new QTimer(this);
        m_findTimer->setInterval(30);
        connect(m_findTimer, &QTimer::timeout, this, &OkularBenchDriver::pollForWindow);
        m_findTimer->start();
    }

private:
    QAbstractScrollArea *findPageView() const
    {
        if (!m_win)
            return nullptr;
        QAbstractScrollArea *best = nullptr;
        int bestArea = 0;
        for (QAbstractScrollArea *sa : m_win->findChildren<QAbstractScrollArea *>()) {
            if (!sa->isVisible() || !sa->viewport())
                continue;
            const int area = sa->viewport()->width() * sa->viewport()->height();
            if (area > bestArea) {
                bestArea = area;
                best = sa;
            }
        }
        return best;
    }

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
                // Wait for Poppler to render initial pages
                QTimer::singleShot(1100, this, &OkularBenchDriver::startResizeBench);
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
        connect(m_resizeTimer, &QTimer::timeout, this, &OkularBenchDriver::doResizeStep);
        m_resizeTimer->start();
    }

    void doResizeStep()
    {
        if (!m_win || m_step >= int(m_sizes.size())) {
            m_resizeTimer->stop();
            QTimer::singleShot(300, this, &OkularBenchDriver::startScrollBench);
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

    void startScrollBench()
    {
        m_pageView = findPageView();
        if (m_pageView && m_pageView->verticalScrollBar()) {
            // Reset saved Okular document position to top of Page 1 so every run is identical!
            m_pageView->verticalScrollBar()->setValue(0);
            QCoreApplication::processEvents();
        }
        std::fprintf(stderr, "[BENCH] pageView=%s viewport=%dx%d vscroll_val=%d vscroll_max=%d\n",
                     m_pageView ? m_pageView->metaObject()->className() : "null",
                     m_pageView ? m_pageView->viewport()->width() : 0,
                     m_pageView ? m_pageView->viewport()->height() : 0,
                     (m_pageView && m_pageView->verticalScrollBar()) ? m_pageView->verticalScrollBar()->value() : -1,
                     (m_pageView && m_pageView->verticalScrollBar()) ? m_pageView->verticalScrollBar()->maximum() : 0);
        std::fflush(stderr);
        m_scrollStep = 0;
        m_scrollTimer = new QTimer(this);
        m_scrollTimer->setInterval(33);
        connect(m_scrollTimer, &QTimer::timeout, this, &OkularBenchDriver::doScrollStep);
        m_scrollTimer->start();
    }

    void doScrollStep()
    {
        if (!m_win || m_scrollStep >= 30) {
            m_scrollTimer->stop();
            QTimer::singleShot(250, this, &OkularBenchDriver::startPageJumpBench);
            return;
        }
        QElapsedTimer t;
        t.start();
        const double rtStart = realtimeMs();
        if (m_pageView && m_pageView->verticalScrollBar()) {
            QScrollBar *sb = m_pageView->verticalScrollBar();
            sb->setValue(sb->value() + 180);
        } else {
            QKeyEvent ev(QEvent::KeyPress, Qt::Key_Down, Qt::NoModifier);
            QApplication::sendEvent(m_win, &ev);
        }
        QCoreApplication::processEvents();
        const double elapsedMs = double(t.nsecsElapsed()) / 1e6;
        m_scrollMs.push_back(elapsedMs);
        std::fprintf(stderr, "[BENCH] scroll step=%02d wall_ms=%.2f rt=%.3f\n",
                     m_scrollStep, elapsedMs, rtStart);
        std::fflush(stderr);
        ++m_scrollStep;
    }

    void startPageJumpBench()
    {
        m_jumpStep = 0;
        m_jumpTimer = new QTimer(this);
        m_jumpTimer->setInterval(90);
        connect(m_jumpTimer, &QTimer::timeout, this, &OkularBenchDriver::doJumpStep);
        m_jumpTimer->start();
    }

    void doJumpStep()
    {
        if (!m_win || m_jumpStep >= 10) {
            m_jumpTimer->stop();
            QTimer::singleShot(200, this, &OkularBenchDriver::finishBench);
            return;
        }
        QWidget *target = m_pageView ? static_cast<QWidget *>(m_pageView) : static_cast<QWidget *>(m_win);
        QElapsedTimer t;
        t.start();
        const double rtStart = realtimeMs();
        if (m_pageView && m_pageView->verticalScrollBar()) {
            QScrollBar *sb = m_pageView->verticalScrollBar();
            sb->setValue(sb->value() + sb->pageStep());
        } else {
            QKeyEvent ev(QEvent::KeyPress, Qt::Key_PageDown, Qt::NoModifier);
            QApplication::sendEvent(target, &ev);
        }
        QCoreApplication::processEvents();
        const double elapsedMs = double(t.nsecsElapsed()) / 1e6;
        m_jumpMs.push_back(elapsedMs);
        std::fprintf(stderr, "[BENCH] pagejump step=%02d wall_ms=%.2f rt=%.3f\n",
                     m_jumpStep, elapsedMs, rtStart);
        std::fflush(stderr);
        ++m_jumpStep;
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
        summarize("scroll_wall_ms", m_scrollMs);
        summarize("pagejump_wall_ms", m_jumpMs);
        std::fprintf(stderr, "[BENCH_SUMMARY] peak_rss_mb=%.1f\n", double(readVmHwmKb()) / 1024.0);
        std::fflush(stderr);
        QCoreApplication::quit();
    }

    QTimer *m_findTimer = nullptr;
    QTimer *m_resizeTimer = nullptr;
    QTimer *m_scrollTimer = nullptr;
    QTimer *m_jumpTimer = nullptr;
    QMainWindow *m_win = nullptr;
    QAbstractScrollArea *m_pageView = nullptr;
    std::vector<QSize> m_sizes;
    std::vector<double> m_resizeMs;
    std::vector<double> m_scrollMs;
    std::vector<double> m_jumpMs;
    int m_step = 0;
    int m_scrollStep = 0;
    int m_jumpStep = 0;
};

static void installBenchDriver()
{
    if (qEnvironmentVariableIntValue("QT_OKULAR_BENCH") > 0)
        new OkularBenchDriver();
}

Q_COREAPP_STARTUP_FUNCTION(installBenchDriver)

} // namespace
