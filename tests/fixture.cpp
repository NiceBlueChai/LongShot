#include "windowscapture.h"
#include <QApplication>
#include <QLabel>
#include <QPainter>
#include <QScrollArea>
#include <QScreen>
#include <QVBoxLayout>
#include <QMouseEvent>
#include <QTimer>
#include <QScrollBar>
#include <QHBoxLayout>
#include <cstdio>

class Fixture : public QWidget
{
public:
    Fixture()
    {
        setWindowTitle("LongShot capture fixture");
        auto layout = new QVBoxLayout(this);
        layout->setContentsMargins(30, 30, 30, 30);
        auto label = new QLabel("Capture test: outside clicks must not reach this window.");
        layout->addWidget(label);
        auto scroll = new QScrollArea;
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        QImage document(500, 1200, QImage::Format_RGB32);
        document.fill(Qt::white);
        QPainter painter(&document);
        for (int y = 0; y < document.height(); y += 20) {
            painter.fillRect(0, y, document.width(), 20, QColor::fromHsv((y * 7) % 360, 60, 250));
            painter.drawText(10, y + 15, QString("Row %1 - LongShot integration test").arg(y / 20));
        }
        painter.end();
        auto content = new QLabel;
        content->setPixmap(QPixmap::fromImage(document));
        content->setFixedSize(document.size());
        scroll->setWidget(content);
        scroll->setFixedSize(500, 300);
        auto panes = new QHBoxLayout;
        panes->addWidget(scroll);
        layout->addLayout(panes);
        connect(scroll->verticalScrollBar(), &QScrollBar::valueChanged, this, [](int) {
            std::printf("SCROLL1\n"); std::fflush(stdout);
        });
        if (QApplication::arguments().contains("--two-panes")) {
            second_ = new QScrollArea;
            second_->setFrameShape(QFrame::NoFrame);
            second_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
            second_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
            auto secondContent = new QLabel;
            secondContent->setPixmap(QPixmap::fromImage(document));
            secondContent->setFixedSize(document.size());
            second_->setWidget(secondContent);
            second_->setFixedSize(500, 300);
            panes->addWidget(second_);
            connect(second_->verticalScrollBar(), &QScrollBar::valueChanged, this, [](int) {
                std::printf("SCROLL2\n"); std::fflush(stdout);
            });
        }
        setFixedSize(sizeHint());
        move(QGuiApplication::primaryScreen()->availableGeometry().topLeft() + QPoint(80, 80));
        if (QApplication::arguments().contains("--right"))
            move(QGuiApplication::primaryScreen()->availableGeometry().topLeft() + QPoint(800, 80));
        const auto args = QApplication::arguments();
        const int position = args.indexOf("--position");
        if (position >= 0 && position + 2 < args.size())
            move(QGuiApplication::primaryScreen()->availableGeometry().topLeft()
                 + QPoint(args[position + 1].toInt(), args[position + 2].toInt()));
        qApp->installEventFilter(this);
        scroll_ = scroll;
    }
    void report()
    {
        const auto origin = scroll_->viewport()->mapToGlobal(QPoint());
        const auto screen = QGuiApplication::primaryScreen();
        const auto physical = WindowsCapture::toPhysical(
            QRect(origin - screen->geometry().topLeft(), scroll_->viewport()->size()),
            screen->size(), WindowsCapture::screenRect(screen->name()));
        std::printf("RECT %d %d %d %d\n", physical.x(), physical.y(), physical.width(), physical.height());
        if (second_) {
            const auto point = second_->viewport()->mapToGlobal(QPoint());
            const auto area = WindowsCapture::toPhysical(
                QRect(point - screen->geometry().topLeft(), second_->viewport()->size()),
                screen->size(), WindowsCapture::screenRect(screen->name()));
            std::printf("SECOND %d %d %d %d\n", area.x(), area.y(), area.width(), area.height());
        }
        std::fflush(stdout);
    }
protected:
    bool eventFilter(QObject *, QEvent *event) override
    {
        if (event->type() == QEvent::MouseButtonPress) {
            std::printf("CLICK\n");
            std::fflush(stdout);
        }
        if (event->type() == QEvent::Wheel) {
            std::printf("WHEEL\n");
            std::fflush(stdout);
        }
        return false;
    }
private:
    QScrollArea *scroll_;
    QScrollArea *second_ = nullptr;
};

int main(int argc, char **argv)
{
    WindowsCapture::enableDpiAwareness();
    QApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
    QApplication app(argc, argv);
    Fixture fixture;
    fixture.show();
    fixture.raise();
    fixture.activateWindow();
    QTimer::singleShot(400, &fixture, [&fixture] { fixture.report(); });
    QTimer::singleShot(45000, &app, &QApplication::quit);
    return app.exec();
}
