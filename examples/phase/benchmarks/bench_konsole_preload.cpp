#include <QApplication>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QKeyEvent>
#include <QMainWindow>
#include <QTimer>
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

class KonsoleBenchDriver : public QObject
{
public:
    KonsoleBenchDriver()
    {
        m_findTimer = new QTimer(this);
        m_findTimer->setInterval(30);
        connect(m_findTimer, &QTimer::timeout, this, &KonsoleBenchDriver::pollForWindow);
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
                QTimer::singleShot(900, this, &KonsoleBenchDriver::startResize);
                return;
            }
        }
    }

    void startResize()
    {
        m_sizes = {
            {900, 600}, {980, 640}, {1060, 680}, {1140, 720}, {1220, 760},
            {1300, 800}, {1380, 840}, {1260, 780}, {1140, 720}, {1020, 660},
            {900, 600}, {1060, 680}, {1220, 760}, {1380, 840}, {1200, 750}
        };
        m_step = 0;
        m_timer = new QTimer(this);
        m_timer->setInterval(40);
        connect(m_timer, &QTimer::timeout, this, &KonsoleBenchDriver::doResize);
        m_timer->start();
    }

    void doResize()
    {
        if (!m_win || m_step >= int(m_sizes.size())) {
            m_timer->stop();
            // Send 15 sparse keystrokes @ 160 ms into the live terminal
            m_step = 0;
            m_timer->disconnect();
            m_timer->setInterval(160);
            connect(m_timer, &QTimer::timeout, this, &KonsoleBenchDriver::doType);
            m_timer->start();
            return;
        }
        QElapsedTimer t;
        t.start();
        m_win->resize(m_sizes[size_t(m_step)]);
        QCoreApplication::processEvents();
        m_resizeMs.push_back(double(t.nsecsElapsed()) / 1e6);
        ++m_step;
    }

    void doType()
    {
        if (!m_win || m_step >= 15) {
            m_timer->stop();
            auto summarize = [](const char *label, std::vector<double> v) {
                if (v.empty()) return;
                std::sort(v.begin(), v.end());
                double s = 0; for (double x : v) s += x;
                std::fprintf(stderr, "[KONSOLE_SUMMARY] %s n=%zu min=%.2f p50=%.2f p90=%.2f max=%.2f mean=%.2f\n",
                             label, v.size(), v.front(), v[v.size()/2], v[(v.size()*9)/10], v.back(), s / double(v.size()));
            };
            summarize("resize_ms", m_resizeMs);
            summarize("sparse_type_ms", m_typeMs);
            std::fflush(stderr);
            QTimer::singleShot(120, qApp, &QCoreApplication::quit);
            return;
        }
        QWidget *target = QApplication::focusWidget() ? QApplication::focusWidget() : static_cast<QWidget *>(m_win);
        QElapsedTimer t;
        t.start();
        const QString ch = QString(QChar('a' + (m_step % 26)));
        QKeyEvent ev(QEvent::KeyPress, Qt::Key_A + (m_step % 26), Qt::NoModifier, ch);
        QApplication::sendEvent(target, &ev);
        QCoreApplication::processEvents();
        m_typeMs.push_back(double(t.nsecsElapsed()) / 1e6);
        ++m_step;
    }

    QTimer *m_findTimer = nullptr;
    QTimer *m_timer = nullptr;
    QMainWindow *m_win = nullptr;
    std::vector<QSize> m_sizes;
    std::vector<double> m_resizeMs;
    std::vector<double> m_typeMs;
    int m_step = 0;
};

static void installKonsoleBench()
{
    if (qEnvironmentVariableIntValue("QT_KONSOLE_BENCH") > 0)
        new KonsoleBenchDriver();
}

Q_COREAPP_STARTUP_FUNCTION(installKonsoleBench)

} // namespace
