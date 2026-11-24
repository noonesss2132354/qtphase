#include <QGuiApplication>
#include <QPainter>
#include <QPdfWriter>
#include <QPageSize>
#include <QFont>

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    const QString outPath = (argc > 1)
            ? QString::fromLocal8Bit(argv[1])
            : QStringLiteral("okular_test.pdf");

    QPdfWriter pdf(outPath);
    pdf.setPageSize(QPageSize(QPageSize::A4));
    pdf.setResolution(150);

    QPainter p(&pdf);
    const int w = pdf.width();
    const int h = pdf.height();

    for (int page = 0; page < 20; ++page) {
        if (page > 0)
            pdf.newPage();

        // Header band
        p.fillRect(0, 0, w, 140, QColor::fromHsv((page * 27) % 360, 140, 220));
        p.setPen(Qt::white);
        p.setFont(QFont(QStringLiteral("Sans Serif"), 22, QFont::Bold));
        p.drawText(QRect(50, 25, w - 100, 90), Qt::AlignVCenter | Qt::AlignLeft,
                   QStringLiteral("QtPhase Performance Report — Section %1").arg(page + 1));

        // Vector chart box
        QRect chartRect(50, 180, w - 100, 320);
        p.setPen(QPen(QColor(80, 90, 110), 2));
        p.setBrush(QColor(245, 247, 250));
        p.drawRoundedRect(chartRect, 14, 14);
        for (int b = 0; b < 12; ++b) {
            const int bh = 60 + ((page * 37 + b * 53) % 210);
            const int bx = chartRect.left() + 30 + b * ((chartRect.width() - 60) / 12);
            const int bw = (chartRect.width() - 120) / 14;
            p.setPen(Qt::NoPen);
            p.setBrush(QColor::fromHsv((page * 27 + b * 19) % 360, 175, 210));
            p.drawRoundedRect(QRect(bx, chartRect.bottom() - 25 - bh, bw, bh), 4, 4);
        }

        // Dense body paragraphs
        p.setPen(QColor(30, 35, 42));
        p.setFont(QFont(QStringLiteral("Sans Serif"), 11));
        int y = 540;
        for (int para = 0; para < 10; ++para) {
            const QString text = QStringLiteral(
                    "Paragraph %1.%2: Okular renders high-resolution PDF page tiles via Poppler and blits "
                    "them onto its QAbstractScrollArea PageView viewport. During smooth wheel scrolling, "
                    "window resizing, and rapid page navigation, QtPhase must upload large page QPixmaps "
                    "without bindless slot churn, CPU-side staging stalls, or unnecessary full-frame redraws.")
                    .arg(page + 1).arg(para + 1);
            QRect r(50, y, w - 100, 105);
            p.drawText(r, Qt::TextWordWrap, text);
            y += 110;
        }

        // Footer
        p.setPen(QColor(120, 125, 135));
        p.setFont(QFont(QStringLiteral("Sans Serif"), 9));
        p.drawText(QRect(50, h - 60, w - 100, 40), Qt::AlignRight,
                   QStringLiteral("Page %1 of 20").arg(page + 1));
    }
    p.end();
    return 0;
}
