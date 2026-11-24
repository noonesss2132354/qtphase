// Real-world Qt Widgets application using 100% standard public Qt Widgets APIs
// (ZERO #include <QtPhase/...> — runs transparently on QtPhase Vulkan 1.3 + CPU tile pool!)

#include <QtWidgets/QApplication>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QGroupBox>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QMainWindow>
#include <QtWidgets/QMenuBar>
#include <QtWidgets/QPlainTextEdit>
#include <QtWidgets/QProgressBar>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QRadioButton>
#include <QtWidgets/QScrollBar>
#include <QtWidgets/QSlider>
#include <QtWidgets/QSplitter>
#include <QtWidgets/QStatusBar>
#include <QtWidgets/QTabWidget>
#include <QtWidgets/QTableView>
#include <QtWidgets/QToolBar>
#include <QtWidgets/QVBoxLayout>
#include <QtWidgets/QWidget>

#include <QtCore/QAbstractTableModel>
#include <QtCore/QCoreApplication>
#include <QtCore/QStringList>
#include <QtCore/QTimer>
#include <QtGui/QLinearGradient>
#include <QtGui/QPainter>
#include <QtGui/QPainterPath>

#include <cmath>
#include <cstdio>

namespace {

// 50,000-row virtualized QAbstractTableModel (standard Qt Model/View)
class TelemetryTableModel final : public QAbstractTableModel
{
public:
    explicit TelemetryTableModel(QObject *parent = nullptr)
        : QAbstractTableModel(parent) {}

    int rowCount(const QModelIndex &parent = {}) const override
    {
        return parent.isValid() ? 0 : 50000;
    }

    int columnCount(const QModelIndex &parent = {}) const override
    {
        return parent.isValid() ? 0 : 4;
    }

    QVariant headerData(int section, Qt::Orientation orientation, int role) const override
    {
        if (orientation == Qt::Horizontal && role == Qt::DisplayRole) {
            static const char *headers[] = {"ID", "Component", "Status", "Latency"};
            return QString::fromLatin1(headers[section]);
        }
        return {};
    }

    QVariant data(const QModelIndex &index, int role) const override
    {
        if (!index.isValid() || role != Qt::DisplayRole)
            return {};
        const int r = index.row();
        switch (index.column()) {
        case 0:
            return QStringLiteral("#%1").arg(r + 1, 5, 10, QLatin1Char('0'));
        case 1: {
            static const char *names[] = {
                "Vulkan Tile Binner", "HarfBuzz hb-gpu Baker", "UI Reflex Pacer",
                "Zero-Stall Staging", "Widget Retained Slot", "Wayland Phased Surface"
            };
            return QString::fromLatin1(names[r % 6]);
        }
        case 2:
            return (r % 5 == 0) ? QStringLiteral("Active") : QStringLiteral("Cached");
        case 3:
            return QStringLiteral("%1.%2 ms").arg((r % 4)).arg((r * 17) % 100, 2, 10, QLatin1Char('0'));
        }
        return {};
    }
};

// Custom QWidget using standard QPainter (gradients, rounded paths, polylines, ellipses, text)
class CustomWaveformWidget final : public QWidget
{
public:
    explicit CustomWaveformWidget(QWidget *parent = nullptr)
        : QWidget(parent)
    {
        setMinimumHeight(130);
    }

    void setPhaseOffset(double p)
    {
        m_phase = p;
        update();
    }

    int paintCount() const noexcept { return m_paintCount; }

protected:
    void paintEvent(QPaintEvent *) override
    {
        ++m_paintCount;
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);

        const QRectF r = rect().adjusted(4, 4, -4, -4);
        QLinearGradient bg(r.topLeft(), r.bottomRight());
        bg.setColorAt(0.0, QColor(24, 34, 52));
        bg.setColorAt(1.0, QColor(36, 28, 62));

        QPainterPath card;
        card.addRoundedRect(r, 12.0, 12.0);
        p.fillPath(card, bg);
        p.setPen(QPen(QColor(110, 168, 254, 130), 1.5));
        p.drawPath(card);

        // Animated analytical polyline waveform
        const int pts = 48;
        QPolygonF poly;
        poly.reserve(pts);
        for (int i = 0; i < pts; ++i) {
            const double t = double(i) / double(pts - 1);
            const double x = r.left() + 14.0 + t * (r.width() - 28.0);
            const double y = r.center().y() + 10.0
                    + std::sin(t * 11.0 + m_phase) * (r.height() * 0.24)
                    + std::cos(t * 5.0 - m_phase * 0.7) * (r.height() * 0.08);
            poly.append(QPointF(x, y));
        }
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(QColor(72, 219, 168), 2.5, Qt::SolidLine, Qt::RoundCap));
        p.drawPolyline(poly);

        // Analytical circle indicator
        p.setBrush(QColor(250, 204, 21));
        p.setPen(QPen(Qt::white, 1.5));
        p.drawEllipse(QRectF(poly.last().x() - 5.0, poly.last().y() - 5.0, 10.0, 10.0));

        // Title text via HarfBuzz hb-gpu
        QFont f = font();
        f.setPixelSize(13);
        f.setWeight(QFont::DemiBold);
        p.setFont(f);
        p.setPen(QColor(235, 242, 255));
        p.drawText(QRectF(r.left() + 14.0, r.top() + 8.0, r.width() - 28.0, 22.0),
                   Qt::AlignLeft | Qt::AlignVCenter,
                   QStringLiteral("Custom QWidget (QPainter -> QtPhase) • paintEvents: %1").arg(m_paintCount));
    }

private:
    double m_phase = 0.0;
    int m_paintCount = 0;
};

} // namespace

int main(int argc, char **argv)
{
#ifdef QT_PHASE_PLUGINS_DIR
    qputenv("QT_PLUGIN_PATH", QT_PHASE_PLUGINS_DIR);
#endif
    QApplication app(argc, argv);

    int autoExitMs = 0;
    bool autoScroll = false;
    const QStringList args = app.arguments();
    for (int i = 1; i < args.size(); ++i) {
        if (args[i] == QLatin1String("--exit-after-ms") && i + 1 < args.size())
            autoExitMs = args[++i].toInt();
        else if (args[i] == QLatin1String("--auto-scroll"))
            autoScroll = true;
    }

    QMainWindow win;
    win.setWindowTitle(QStringLiteral("Standard Qt Widgets running on QtPhase (Vulkan 1.3 + CPU Pool)"));
    win.resize(1180, 760);

    // Menu bar & toolbar
    auto *fileMenu = win.menuBar()->addMenu(QStringLiteral("&File"));
    fileMenu->addAction(QStringLiteral("&New Workspace"));
    fileMenu->addAction(QStringLiteral("&Quit"), &app, &QCoreApplication::quit);
    auto *viewMenu = win.menuBar()->addMenu(QStringLiteral("&View"));
    viewMenu->addAction(QStringLiteral("Reset Layout"));

    auto *tb = win.addToolBar(QStringLiteral("Main"));
    tb->addAction(QStringLiteral("Build"));
    tb->addAction(QStringLiteral("Run"));
    tb->addAction(QStringLiteral("Profile"));

    auto *splitter = new QSplitter(Qt::Horizontal, &win);

    // Left panel: standard controls + custom QWidget
    auto *leftPanel = new QWidget(splitter);
    auto *leftLayout = new QVBoxLayout(leftPanel);

    auto *hdr = new QLabel(QStringLiteral("Zero-API-Change Qt Widgets"), leftPanel);
    QFont hdrFont = hdr->font();
    hdrFont.setPixelSize(18);
    hdrFont.setWeight(QFont::Bold);
    hdr->setFont(hdrFont);
    leftLayout->addWidget(hdr);

    auto *group = new QGroupBox(QStringLiteral("Controls & Inputs"), leftPanel);
    auto *form = new QFormLayout(group);
    auto *lineEdit = new QLineEdit(QStringLiteral("Type here — rendered via hb-gpu!"), group);
    auto *combo = new QComboBox(group);
    combo->addItems({QStringLiteral("Vulkan 1.3 Tile-Binned"),
                     QStringLiteral("CPU 16x16 Tile Pool"),
                     QStringLiteral("Hybrid Auto-Race")});
    auto *slider = new QSlider(Qt::Horizontal, group);
    slider->setRange(0, 100);
    slider->setValue(64);
    auto *progress = new QProgressBar(group);
    progress->setRange(0, 100);
    progress->setValue(64);
    QObject::connect(slider, &QSlider::valueChanged, progress, &QProgressBar::setValue);

    form->addRow(QStringLiteral("Search:"), lineEdit);
    form->addRow(QStringLiteral("Pipeline:"), combo);
    form->addRow(QStringLiteral("Load:"), slider);
    form->addRow(QStringLiteral("Progress:"), progress);
    leftLayout->addWidget(group);

    auto *checksRow = new QHBoxLayout();
    auto *cb1 = new QCheckBox(QStringLiteral("UI Reflex"), leftPanel);
    cb1->setChecked(true);
    auto *cb2 = new QCheckBox(QStringLiteral("Stem Darkening"), leftPanel);
    cb2->setChecked(true);
    auto *rb1 = new QRadioButton(QStringLiteral("120 Hz"), leftPanel);
    rb1->setChecked(true);
    auto *rb2 = new QRadioButton(QStringLiteral("60 Hz"), leftPanel);
    checksRow->addWidget(cb1);
    checksRow->addWidget(cb2);
    checksRow->addWidget(rb1);
    checksRow->addWidget(rb2);
    leftLayout->addLayout(checksRow);

    auto *btnRow = new QHBoxLayout();
    auto *btnPrimary = new QPushButton(QStringLiteral("Pulse Waveform"), leftPanel);
    auto *btnReset = new QPushButton(QStringLiteral("Reset"), leftPanel);
    btnRow->addWidget(btnPrimary);
    btnRow->addWidget(btnReset);
    leftLayout->addLayout(btnRow);

    auto *waveform = new CustomWaveformWidget(leftPanel);
    leftLayout->addWidget(waveform);
    leftLayout->addStretch(1);

    // Right panel: QTabWidget with 50,000-row QTableView + QPlainTextEdit
    auto *tabs = new QTabWidget(splitter);
    auto *table = new QTableView(tabs);
    auto *model = new TelemetryTableModel(table);
    table->setModel(model);
    table->horizontalHeader()->setStretchLastSection(true);
    table->setAlternatingRowColors(true);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    tabs->addTab(table, QStringLiteral("50,000-Row Table"));

    auto *editor = new QPlainTextEdit(tabs);
    editor->setPlainText(QStringLiteral(
            "// Standard QPlainTextEdit running on QtPhase!\n"
            "int main(int argc, char **argv) {\n"
            "    QApplication app(argc, argv);\n"
            "    QMainWindow win;\n"
            "    win.show();\n"
            "    return app.exec();\n"
            "}\n"));
    tabs->addTab(editor, QStringLiteral("Code Editor"));

    splitter->setSizes({420, 740});
    win.setCentralWidget(splitter);
    win.statusBar()->showMessage(QStringLiteral("Ready — 100% standard Qt Widgets API backed by QtPhase"));

    // Animate waveform & optionally scroll table to verify incremental partial updates!
    double phase = 0.0;
    QTimer animTimer;
    QObject::connect(&animTimer, &QTimer::timeout, [&]() {
        phase += 0.14;
        waveform->setPhaseOffset(phase);
        if (autoScroll) {
            QScrollBar *sb = table->verticalScrollBar();
            sb->setValue((sb->value() + 3) % std::max(1, sb->maximum()));
        }
    });
    QObject::connect(btnPrimary, &QPushButton::clicked, [&]() {
        phase += 0.8;
        waveform->setPhaseOffset(phase);
    });
    QObject::connect(btnReset, &QPushButton::clicked, [&]() {
        slider->setValue(50);
        phase = 0.0;
        waveform->setPhaseOffset(0.0);
    });

    win.show();
    animTimer.start(16);

    if (autoExitMs > 0) {
        QTimer::singleShot(autoExitMs, &app, [&]() {
            std::fprintf(stderr, "[widgets_phase_demo] CustomWaveformWidget paintEvents=%d\n",
                         waveform->paintCount());
            std::fflush(stderr);
            app.quit();
        });
    }

    return app.exec();
}
