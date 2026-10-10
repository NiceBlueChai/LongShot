#include "editor.h"
#include "toolbaricons.h"
#include "ocr.h"
#include "stitcher.h"
#include "capturecontroller.h"
#include "windowscapture.h"
#include "selectionoverlay.h"
#include "capturecomposition.h"
#include <QComboBox>
#include <QDialog>
#include <QLabel>
#include <QFrame>
#include <QHelpEvent>
#include <QToolTip>
#include <QPlainTextEdit>
#include <QTimer>
#include <QApplication>
#include <QClipboard>
#include <QPainter>
#include <QProcess>
#include <QProcessEnvironment>
#include <QPushButton>
#include <QScreen>
#include <QScrollBar>
#include <QTemporaryDir>
#include <QWheelEvent>
#include <QToolBar>
#include <QtTest>
#include <tlhelp32.h>
#include <algorithm>
#include <future>
#include <objbase.h>
#include <oleacc.h>
#include <UIAutomation.h>
#include <wrl/client.h>

static QImage document(int height = 1000)
{
    QImage image(180, height, QImage::Format_RGB32);
    quint32 value = 12345;
    for (int y = 0; y < height; ++y) {
        auto row = reinterpret_cast<QRgb *>(image.scanLine(y));
        for (int x = 0; x < image.width(); ++x) {
            value = value * 1664525U + 1013904223U;
            row[x] = qRgb(value >> 24, (value >> 16) & 255, (value >> 8) & 255);
        }
    }
    return image;
}

static QRect nativeBounds(QWidget *widget)
{
    RECT bounds = {};
    GetWindowRect(reinterpret_cast<HWND>(widget->winId()), &bounds);
    return QRect(bounds.left, bounds.top, bounds.right - bounds.left, bounds.bottom - bounds.top);
}

static UINT nativeClick(QPoint position)
{
    SetCursorPos(position.x(), position.y());
    INPUT events[2] = {};
    events[0].type = events[1].type = INPUT_MOUSE;
    events[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    events[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
    return SendInput(2, events, sizeof(INPUT));
}

static HWND processWindow(qint64 process)
{
    struct Search { DWORD process; HWND window; } search { DWORD(process), nullptr };
    EnumWindows([](HWND window, LPARAM data) -> BOOL {
        auto search = reinterpret_cast<Search *>(data);
        DWORD process = 0;
        GetWindowThreadProcessId(window, &process);
        if (process == search->process && IsWindowVisible(window)) {
            search->window = window;
            return FALSE;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&search));
    return search.window;
}

class CoreTests : public QObject
{
    Q_OBJECT
private slots:
    void taskbarOverlayReceivesClick()
    {
        const HWND taskbar = FindWindowW(L"Shell_TrayWnd", nullptr);
        if (!taskbar || !IsWindowVisible(taskbar)) QSKIP("No visible taskbar");
        RECT box = {};
        QVERIFY(GetWindowRect(taskbar, &box));
        const QPoint point((box.left + box.right) / 2, (box.top + box.bottom) / 2);
        QScreen *screen = nullptr;
        for (auto candidate : QGuiApplication::screens())
            if (WindowsCapture::screenRect(candidate->name()).contains(point)) screen = candidate;
        QVERIFY(screen);
        const HWND foreground = GetForegroundWindow();
        SetForegroundWindow(taskbar);
        SelectionOverlay overlay(screen, [](SelectionOverlay *, CaptureSelection) {}, [] {}, [] {},
                                 [](SelectionOverlay::Phase) {});
        overlay.show();
        QTest::qWait(100);
        QVERIFY2(nativeBounds(&overlay).contains(point), "Overlay does not cover taskbar");
        POINT previous = {};
        GetCursorPos(&previous);
        SetCursorPos(point.x(), point.y());
        SetWindowPos(taskbar, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        QTest::qWait(300);
        const HWND hit = WindowFromPoint(POINT { point.x(), point.y() });
        QCOMPARE(GetAncestor(hit, GA_ROOT), reinterpret_cast<HWND>(overlay.winId()));
        QVERIFY(!overlay.outer_.isEmpty());
        QCOMPARE(nativeClick(point), UINT(2));
        QTRY_VERIFY_WITH_TIMEOUT(overlay.confirmed_, 2000);
        SetCursorPos(previous.x, previous.y);
        QVERIFY(overlay.panel_->isVisible());
        SetForegroundWindow(foreground);
    }

    void visibleTaskbarIsCaptureCandidate()
    {
        const HWND taskbar = FindWindowW(L"Shell_TrayWnd", nullptr);
        if (!taskbar || !IsWindowVisible(taskbar)) QSKIP("No visible Windows taskbar");
        RECT native = {};
        QVERIFY(GetWindowRect(taskbar, &native));
        const QRect bounds(native.left, native.top, native.right - native.left, native.bottom - native.top);
        const QRect monitor = WindowsCapture::monitorRect(bounds.center());
        if (bounds.intersected(monitor).height() < 5) QSKIP("Taskbar is auto-hidden");
        const auto found = WindowsCapture::candidates(bounds);
        QVERIFY2(std::any_of(found.begin(), found.end(), [=](const WindowsCapture::WindowCandidate &candidate) {
            return candidate.window == taskbar;
        }), "Visible taskbar was excluded from screenshot targets");
        QString error;
        const auto image = WindowsCapture::grab(bounds.intersected(monitor), &error);
        QVERIFY2(!image.isNull(), qPrintable(error));
        QCOMPARE(image.size(), bounds.intersected(monitor).size());
    }
    void completionButtonsFollowCapturePhase()
    {
        int snapshots = 0;
        SelectionOverlay overlay(QGuiApplication::primaryScreen(),
            [&](SelectionOverlay *, CaptureSelection) { ++snapshots; }, [] {}, [] {}, [](SelectionOverlay::Phase) {});
        overlay.outer_ = QRect(40, 40, 300, 240);
        overlay.region_ = QRect(50, 50, 200, 180);
        overlay.confirmed_ = true;
        overlay.refresh();
        QVERIFY(!overlay.snapshot_->isHidden());
        QVERIFY(overlay.start_->isHidden());
        QVERIFY(overlay.finish_->isHidden());
        bool preparationCorrect = true;
        for (auto stage : { SelectionOverlay::Phase::Windows, SelectionOverlay::Phase::Region }) {
            overlay.phase(stage);
            overlay.refresh();
            preparationCorrect &= overlay.snapshot_->isHidden() && overlay.finish_->isHidden();
            QCOMPARE(overlay.start_->isHidden(), stage != SelectionOverlay::Phase::Region);
            overlay.snapshot_->click();
        }
        overlay.phase(SelectionOverlay::Phase::Capturing);
        overlay.refresh();
        QVERIFY(overlay.snapshot_->isHidden());
        QVERIFY(overlay.start_->isHidden());
        QVERIFY(!overlay.finish_->isHidden());
        QVERIFY(preparationCorrect);
        QCOMPARE(snapshots, 0);
    }
    void wheelSelectionResumesOnMovement()
    {
        SelectionOverlay overlay(QGuiApplication::primaryScreen(), {}, {}, {}, {});
        QVERIFY(overlay.valid());
        WindowsCapture::WindowCandidate target;
        target.window = GetDesktopWindow();
        target.bounds = overlay.screenRect_;
        target.visible = QRegion(target.bounds);
        overlay.hoverWindows_ = { target };
        overlay.samplingScreen_ = {};
        const QRect button(40, 40, 80, 60), parent(20, 20, 200, 160);
        const QRect physicalButton = overlay.physical(button);
        overlay.hoverDetector_ = [=](HWND, QPoint, bool) {
            return QVector<QRect> { target.bounds, physicalButton };
        };
        overlay.samplePoint_ = QPoint(60, 60);
        overlay.hoverPoint_ = overlay.samplePoint_;
        overlay.outer_ = button;
        overlay.hoverRegions_ = { overlay.logical(target.bounds), parent, button };
        overlay.hoverLevel_ = 2;
        QWheelEvent wheel(QPointF(60, 60), QPointF(60, 60), QPoint(), QPoint(0, 120),
                          Qt::NoButton, Qt::NoModifier, Qt::ScrollUpdate, false);
        overlay.wheelEvent(&wheel);
        const QRect chosen = overlay.outer_;
        overlay.updateHover();
        const QRect stationary = overlay.outer_;
        overlay.samplePoint_ = QPoint(61, 60);
        for (int i = 0; i < 10; ++i) {
            overlay.updateHover();
            QThread::msleep(10);
        }
        QCOMPARE(chosen, parent);
        QCOMPARE(stationary, parent);
        QCOMPARE(overlay.outer_, button);
        QVERIFY(!overlay.hoverManualLock_);
    }
    void magnifierDirtyPaintMatchesFull()
    {
        SelectionOverlay overlay(QGuiApplication::primaryScreen(), {}, {}, {}, {});
        QVERIFY(overlay.valid());
        overlay.outer_ = QRect(40, 40, 120, 100);
        QImage actual(overlay.size(), QImage::Format_ARGB32_Premultiplied);
        actual.fill(Qt::transparent);
        overlay.samplePoint_ = QPoint(100, 100);
        overlay.render(&actual);
        for (const QPoint next : { QPoint(110, 106), QPoint(overlay.width() - 5, overlay.height() - 5),
                                  QPoint(-1, -1) }) {
            const QRect previous = overlay.magnifierRect(overlay.samplePoint_);
            overlay.samplePoint_ = next;
            const QRegion dirty = QRegion(previous) | QRegion(overlay.magnifierRect(next));
            overlay.render(&actual, dirty.boundingRect().topLeft(), dirty);
            QImage expected(actual.size(), actual.format());
            expected.fill(Qt::transparent);
            overlay.render(&expected);
            QCOMPARE(actual, expected);
        }
    }
    void hoverPaintCost()
    {
        SelectionOverlay overlay(QGuiApplication::primaryScreen(), {}, {}, {}, {});
        QVERIFY(overlay.valid());
        overlay.samplePoint_ = QPoint(100, 100);
        overlay.outer_ = QRect(40, 40, 120, 100);
        QImage canvas(overlay.size() * overlay.devicePixelRatioF(), QImage::Format_ARGB32_Premultiplied);
        canvas.setDevicePixelRatio(overlay.devicePixelRatioF());
        canvas.fill(Qt::transparent);
        for (const bool partial : { false, true }) {
            QElapsedTimer elapsed;
            elapsed.start();
            for (int i = 0; i < 20; ++i) {
                const QRegion region = partial ? QRegion(QRect(122, 122, 182, 220)) : QRegion(overlay.rect());
                overlay.render(&canvas, QPoint(), region);
            }
            qInfo() << (partial ? "Magnifier-region paint" : "Whole-screen paint")
                    << elapsed.nsecsElapsed() / 20000000.0 << "ms/frame" << overlay.size();
        }
    }
    void hoverMovementKeepsCurrentBounds()
    {
        SelectionOverlay overlay(QGuiApplication::primaryScreen(), {}, {}, {}, {});
        QVERIFY(overlay.valid());
        WindowsCapture::WindowCandidate target;
        target.window = GetDesktopWindow();
        target.bounds = overlay.screenRect_;
        target.visible = QRegion(target.bounds);
        overlay.hoverWindows_ = { target };
        overlay.samplingScreen_ = {};
        const QRect window = overlay.logical(target.bounds);
        const QRect buttonBounds(40, 40, 80, 60), parent(20, 20, 200, 160);
        const QRect physicalParent = overlay.physical(parent);
        overlay.hoverDetector_ = [=](HWND, QPoint, bool) {
            return QVector<QRect> { target.bounds, physicalParent };
        };
        overlay.outer_ = buttonBounds;
        overlay.hoverPublished_ = true;
        overlay.hoverPoint_ = QPoint(60, 60);
        overlay.samplePoint_ = QPoint(61, 60);
        overlay.hoverCache_.append(qMakePair(overlay.samplePoint_, QVector<QRect> { window, parent }));
        overlay.updateHover();
        const QRect cached = overlay.outer_;
        for (int i = 0; i < 5; ++i) {
            QThread::msleep(10);
            overlay.samplePoint_ += QPoint(1, 0);
            overlay.updateHover();
        }
        const QRect afterQueries = overlay.outer_;
        overlay.samplePoint_ = QPoint(150, 100);
        overlay.updateHover();
        QThread::msleep(30);
        overlay.updateHover();
        const QRect outside = overlay.outer_;
        QThread::msleep(30);
        QCOMPARE(cached, buttonBounds);
        QCOMPARE(afterQueries, buttonBounds);
        QCOMPARE(outside, parent);
    }
    void hoverProvidersDoNotBlockEachOther()
    {
        SelectionOverlay overlay(QGuiApplication::primaryScreen(), {}, {}, {}, {});
        QVERIFY(overlay.valid());
        WindowsCapture::WindowCandidate target;
        target.window = GetDesktopWindow();
        target.bounds = overlay.screenRect_;
        target.visible = QRegion(target.bounds);
        overlay.hoverWindows_ = { target };
        const QRect bounds = target.bounds;
        auto fast = std::make_shared<std::atomic<int>>(0);
        auto slow = std::make_shared<std::atomic<int>>(0);
        overlay.hoverDetector_ = [=](HWND, QPoint, bool msaa) {
            if (msaa) { ++*slow; QThread::msleep(300); }
            else { ++*fast; QThread::msleep(2); }
            return QVector<QRect> { bounds, bounds.adjusted(20, 20, -20, -20) };
        };
        for (int i = 0; i < 16; ++i) {
            overlay.samplePoint_ = QPoint(100 + i, 100);
            overlay.updateHover();
            QThread::msleep(10);
        }
        const int fastCount = fast->load(), slowCount = slow->load();
        QThread::msleep(350);
        qInfo() << "Moving pointer: UIA jobs" << fastCount << "while MSAA jobs" << slowCount;
        QVERIFY(fastCount >= 3);
        QCOMPARE(slowCount, 1);
    }
    void liveExplorerSmallButton()
    {
        const auto handle = qEnvironmentVariable("LONGSHOT_LIVE_HWND").toULongLong();
        if (!handle) QSKIP("Set LONGSHOT_LIVE_HWND to an observed Explorer window for read-only detection.");
        const HWND window = reinterpret_cast<HWND>(quintptr(handle));
        wchar_t type[64] = {};
        GetClassNameW(window, type, 64);
        QCOMPARE(QString::fromWCharArray(type), QStringLiteral("CabinetWClass"));
        using Microsoft::WRL::ComPtr;
        QVERIFY(SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)));
        struct Apartment { ~Apartment() { CoUninitialize(); } } apartment;
        ComPtr<IUIAutomation> automation;
        QVERIFY(SUCCEEDED(CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&automation))));
        ComPtr<IUIAutomationElement> root, button;
        QVERIFY(SUCCEEDED(automation->ElementFromHandle(window, &root)));
        VARIANT value;
        VariantInit(&value);
        value.vt = VT_BSTR;
        value.bstrVal = SysAllocString(L"AddButton");
        ComPtr<IUIAutomationCondition> condition;
        const HRESULT created = automation->CreatePropertyCondition(UIA_AutomationIdPropertyId, value, &condition);
        VariantClear(&value);
        QVERIFY(SUCCEEDED(created));
        QVERIFY(SUCCEEDED(root->FindFirst(TreeScope_Descendants, condition.Get(), &button)) && button);
        RECT box = {};
        QVERIFY(SUCCEEDED(button->get_CurrentBoundingRectangle(&box)));
        const QRect expected(box.left, box.top, box.right - box.left, box.bottom - box.top);
        QVERIFY(!expected.isEmpty());
        QVector<QRect> found;
        for (bool msaa : { true, false }) {
            QElapsedTimer timer;
            timer.start();
            const auto regions = std::async(std::launch::async, [=] {
                return WindowsCapture::elementRegions(window, expected.center(), msaa);
            }).get();
            qInfo() << (msaa ? "MSAA" : "UIA") << timer.elapsed() << "ms" << regions;
            found += regions;
        }
        qInfo() << "Expected Explorer tab button:" << expected;
        QVERIFY(!found.isEmpty());
        QCOMPARE(WindowsCapture::refineElementRegions(found, found.first(), expected.center()).last(), expected);
    }
    void hoverNestedRefinement()
    {
        const QRect window(0, 0, 800, 600), sidebar(0, 0, 260, 600), button(20, 480, 36, 36);
        const QPoint point(38, 498);
        auto chain = WindowsCapture::refineElementRegions({ sidebar }, window, point);
        QCOMPARE(chain.last(), sidebar);
        chain = WindowsCapture::refineElementRegions({ sidebar, button }, window, point, chain.last());
        QCOMPARE(chain.last(), button);
        QVERIFY(chain.contains(sidebar));
        QCOMPARE(WindowsCapture::refineElementRegions({ sidebar }, window, point, button).last(), button);
        const QRect crossing(30, 478, 36, 36);
        QCOMPARE(WindowsCapture::refineElementRegions({ crossing }, window, point, button).last(), button);
        QCOMPARE(WindowsCapture::refineElementRegions({ button, sidebar }, window, point).last(), button);
        const QRect next(90, 480, 36, 36);
        QCOMPARE(WindowsCapture::refineElementRegions({ sidebar, next }, window, next.center(), button).last(), next);
        const QRect negative(-200, -200, 800, 600), tiny(-180, -180, 24, 24);
        QCOMPARE(WindowsCapture::refineElementRegions({ tiny }, negative, tiny.center()).last(), tiny);
    }
    void modelOcrSample()
    {
        const auto path = QString::fromLocal8Bit(qgetenv("LONGSHOT_OCR_SAMPLE"));
        if (path.isEmpty()) QSKIP("Set LONGSHOT_OCR_SAMPLE to compare a local screenshot");
        const QImage image(path);
        QVERIFY(!image.isNull());
        std::atomic_bool canceled(false);
        for (const auto &language : {QStringLiteral("ppocr"), QStringLiteral("zh-Hans")}) {
            QElapsedTimer elapsed;
            elapsed.start();
            const auto result = std::async(std::launch::async, [&] {
                return Ocr::recognize(image, language, canceled);
            }).get();
            QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
            qInfo() << language << "ms:" << elapsed.elapsed() << "characters:" << result.text.size();
            QFile output(QCoreApplication::applicationDirPath() + "/ocr-sample-" + language + ".txt");
            QVERIFY(output.open(QIODevice::WriteOnly));
            QVERIFY(output.write(result.text.toUtf8()) >= 0);
        }
    }

    void modelOcr()
    {
        std::atomic_bool canceled(false);
        QVERIFY(!Ocr::recognizeModel({}, canceled).error.isEmpty());
        QImage image(720, 180, QImage::Format_RGB32);
        image.fill(Qt::white);
        QPainter painter(&image);
        painter.setPen(Qt::black);
        QFont font("Microsoft YaHei");
        font.setPixelSize(20);
        painter.setFont(font);
        painter.drawText(QPoint(20, 40), QStringLiteral("LongShot OCR 12345"));
        painter.drawText(QPoint(20, 85), QStringLiteral("截图识别文字，中英文混合测试。"));
        font.setPixelSize(14);
        painter.setFont(font);
        painter.drawText(QPoint(20, 125), QStringLiteral("保存截图 Ctrl+S 2026-10-10"));
        painter.end();
        QElapsedTimer elapsed;
        elapsed.start();
        const auto result = Ocr::recognizeModel(image, canceled);
        qInfo().noquote() << "model OCR ms:" << elapsed.elapsed() << result.text << result.error;
        QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
        QVERIFY(result.text.contains("12345"));
        QVERIFY(result.text.contains(QStringLiteral("截图识别文字")));
        QVERIFY(result.text.contains("2026-10-10"));
        image.fill(Qt::white);
        const auto blank = Ocr::recognizeModel(image, canceled);
        QVERIFY2(blank.error.isEmpty(), qPrintable(blank.error));
        QVERIFY(blank.text.isEmpty());
        QVERIFY(!Ocr::recognizeModel(QImage(1, 5000, QImage::Format_RGB32), canceled).error.isEmpty());
        canceled = true;
        QVERIFY(Ocr::recognizeModel(image, canceled).text.isEmpty());
    }

    void windowsOcr()
    {
        std::atomic_bool canceled(false);
        QVERIFY(!Ocr::recognize({}, {}, canceled).error.isEmpty());
        QImage image(960, 180, QImage::Format_RGB32);
        image.fill(Qt::white);
        QPainter painter(&image);
        painter.setPen(Qt::black);
        painter.setFont(QFont("Microsoft YaHei", 28));
        painter.drawText(QPoint(30, 65), QStringLiteral("LongShot OCR 12345"));
        painter.drawText(QPoint(30, 135), QStringLiteral("截图识别文字"));
        painter.end();
        const auto result = std::async(std::launch::async, [&] {
            return Ocr::recognize(image, QStringLiteral("zh-Hans"), canceled);
        }).get();
        qInfo().noquote() << result.language << result.text << result.error;
        QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
        QVERIFY(result.text.contains("12345"));
        QVERIFY(result.text.contains(QStringLiteral("截图")));
        QVERIFY(result.text.contains('\n'));
        image.fill(Qt::white);
        const auto blank = std::async(std::launch::async, [&] {
            return Ocr::recognize(image, QStringLiteral("en-US"), canceled);
        }).get();
        QVERIFY2(blank.error.isEmpty(), qPrintable(blank.error));
        QVERIFY(blank.text.trimmed().isEmpty());
        const auto oversized = std::async(std::launch::async, [&] {
            return Ocr::recognize(QImage(1, 100000, QImage::Format_RGB32), {}, canceled);
        }).get();
        QVERIFY(!oversized.error.isEmpty());
        canceled = true;
        QVERIFY(Ocr::recognize(image, {}, canceled).text.isEmpty());
    }

    void ocrDialogCopyAndCancel()
    {
        QImage image(600, 100, QImage::Format_RGB32);
        image.fill(Qt::white);
        QPainter painter(&image);
        painter.setFont(QFont("Arial", 28));
        painter.drawText(QPoint(20, 60), "LongShot 12345");
        painter.end();
        QTimer poll;
        QElapsedTimer elapsed;
        elapsed.start();
        bool copied = false;
        connect(&poll, &QTimer::timeout, this, [&] {
            auto dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            if (!dialog) return;
            auto text = dialog->findChild<QPlainTextEdit *>();
            if (text && text->toPlainText().contains("12345")) {
                dialog->grab().save(QCoreApplication::applicationDirPath() + "/ocr-dialog.png");
                text->setPlainText(QStringLiteral("修改后的识别文字"));
                for (auto button : dialog->findChildren<QPushButton *>()) {
                    if (button->text() == QStringLiteral("复制文字")) button->click();
                }
                copied = QApplication::clipboard()->text() == QStringLiteral("修改后的识别文字");
                dialog->reject();
            } else if (elapsed.elapsed() > 15000) dialog->reject();
        });
        poll.start(50);
        Ocr::showDialog(nullptr, image);
        poll.stop();
        QVERIFY(copied);
        const auto before = QApplication::clipboard()->text();
        QTimer::singleShot(0, this, [] { QApplication::activeModalWidget()->close(); });
        Ocr::showDialog(nullptr, image);
        QCOMPARE(QApplication::clipboard()->text(), before);
    }

    void iconToolbars()
    {
        QApplication::setEffectEnabled(Qt::UI_AnimateTooltip, false);
        QApplication::setEffectEnabled(Qt::UI_FadeTooltip, false);
        for (int kind = 0; kind < ToolbarIcons::Count; ++kind) {
            const auto icon = ToolbarIcons::icon(static_cast<ToolbarIcons::Kind>(kind));
            QVERIFY2(!icon.isNull(), qPrintable(QString("Missing SVG icon %1").arg(kind)));
            for (int size : { 20, 40, 60 }) {
                const auto image = icon.pixmap(size, size).toImage().convertToFormat(QImage::Format_ARGB32);
                QVERIFY(!image.isNull());
                bool visible = false;
                for (int y = 0; y < image.height(); ++y)
                    for (int x = 0; x < image.width(); ++x)
                        visible |= qAlpha(image.pixel(x, y)) != 0;
                QVERIFY2(visible, qPrintable(QString("Blank SVG icon %1 at %2px").arg(kind).arg(size)));
            }
        }
        SelectionOverlay overlay(QGuiApplication::primaryScreen(),
            [](SelectionOverlay *, CaptureSelection) {}, [] {}, [] {}, [](SelectionOverlay::Phase) {});
        overlay.outer_ = QRect(100, 100, 300, 200);
        overlay.confirmed_ = true;
        overlay.refresh();
        for (auto button : overlay.panel_->findChildren<QPushButton *>()) {
            QVERIFY(button->text().isEmpty());
            QVERIFY(!button->icon().isNull());
            QVERIFY(!button->toolTip().isEmpty());
            QVERIFY(!button->accessibleName().isEmpty());
            QCOMPARE(button->width(), 40);
        }
        QVERIFY(overlay.testAttribute(Qt::WA_AlwaysShowToolTips));
        QVERIFY(overlay.panel_->width() < 700);
        auto row = overlay.panel_->findChild<QWidget *>("captureActions");
        QVERIFY(row);
        QVERIFY(overlay.label_->y() >= row->geometry().bottom());
        auto move = overlay.annotationBar_->findChild<QPushButton *>("annotationTool0");
        auto rectangle = overlay.annotationBar_->findChild<QPushButton *>("annotationTool2");
        QVERIFY(move->isChecked());
        rectangle->click();
        QVERIFY(rectangle->isChecked());
        QVERIFY(!move->isChecked());
        QCOMPARE(overlay.annotationTool_, 2);
        move->click();
        QVERIFY(row->width() >= row->minimumSizeHint().width());
        QVERIFY(overlay.annotationBar_->width() >= overlay.annotationBar_->minimumSizeHint().width());
        const int rowY = overlay.snapshot_->mapTo(overlay.panel_, QPoint()).y();
        for (auto button : overlay.panel_->findChildren<QPushButton *>()) {
            if (!button->isVisibleTo(overlay.panel_)) continue;
            QCOMPARE(button->mapTo(overlay.panel_, QPoint()).y(), rowY);
            const QRect bounds(button->mapTo(row, QPoint()), button->size());
            QVERIFY(row->rect().contains(bounds));
            for (auto sibling : overlay.panel_->findChildren<QPushButton *>()) {
                if (sibling == button || !sibling->isVisibleTo(overlay.panel_)) continue;
                QVERIFY(!bounds.intersects(QRect(sibling->mapTo(row, QPoint()), sibling->size())));
            }
        }
        QWidget *scrollSeparator = nullptr, *cancelSeparator = nullptr;
        for (auto line : overlay.panel_->findChildren<QFrame *>()) {
            if (line->property("group").toString() == "scrollSeparator") scrollSeparator = line;
            if (line->property("group").toString() == "cancelSeparator") cancelSeparator = line;
        }
        auto showTip = [](QWidget *button) {
            const QPoint point = button->rect().center();
            QHelpEvent help(QEvent::ToolTip, point, button->mapToGlobal(point));
            QApplication::sendEvent(button, &help);
            QVERIFY(QToolTip::isVisible());
            QCOMPARE(QToolTip::text(), button->toolTip());
            QToolTip::hideText();
        };
        for (auto button : overlay.panel_->findChildren<QPushButton *>()) {
            if (button->isVisibleTo(overlay.panel_)) showTip(button);
        }
        QVERIFY(scrollSeparator && cancelSeparator);
        QVERIFY(!scrollSeparator->isHidden());
        QVERIFY(!cancelSeparator->isHidden());
        overlay.label_->setText(QStringLiteral("选择工具添加标注 · 完成复制 · Esc 取消"));
        QVERIFY(overlay.panel_->grab().save(QCoreApplication::applicationDirPath() + "/icon-toolbar.png"));
        overlay.phase(SelectionOverlay::Phase::Windows);
        overlay.refresh();
        QVERIFY(scrollSeparator->isHidden());
        QVERIFY(cancelSeparator->isHidden());
        overlay.phase(SelectionOverlay::Phase::Region);
        overlay.region_ = QRect(110, 110, 100, 100);
        overlay.refresh();
        QVERIFY(scrollSeparator->isHidden());
        QVERIFY(!cancelSeparator->isHidden());
        overlay.candidates_.resize(2);
        overlay.refresh();
        for (auto button : overlay.panel_->findChildren<QPushButton *>()) {
            if (button->isVisibleTo(overlay.panel_)) showTip(button);
        }
        QToolTip::hideText();
        QEvent enter(QEvent::Enter);
        QApplication::sendEvent(overlay.start_, &enter);
        QVERIFY(QToolTip::isVisible());
        QCOMPARE(QToolTip::text(), overlay.start_->toolTip());
        QEvent leave(QEvent::Leave);
        QApplication::sendEvent(overlay.start_, &leave);
        QTRY_VERIFY_WITH_TIMEOUT(!QToolTip::isVisible(), 1000);
        overlay.label_->setText(QStringLiteral("确认绿色滚动区域，点击开始采集"));
        QVERIFY(overlay.panel_->grab().save(QCoreApplication::applicationDirPath() + "/scroll-toolbar.png"));
        overlay.phase(SelectionOverlay::Phase::Capturing);
        overlay.refresh();
        QVERIFY(!overlay.finish_->isHidden());
        QVERIFY(!cancelSeparator->isHidden());
        overlay.completionKey(true);
        QVERIFY(overlay.finish_->text().isEmpty());
        QVERIFY(overlay.finish_->toolTip().contains("Q"));
        showTip(overlay.finish_);
        overlay.completionKey(false);
        QVERIFY(overlay.finish_->toolTip().contains(QStringLiteral("不可用")));
        showTip(overlay.finish_);
        QVERIFY(overlay.panel_->grab().save(QCoreApplication::applicationDirPath() + "/capturing-toolbar.png"));
        overlay.phase(SelectionOverlay::Phase::Outer);
        overlay.outer_ = QRect(overlay.width() - 80, overlay.height() - 80, 60, 60);
        overlay.refresh();
        QVERIFY(overlay.rect().contains(overlay.panel_->geometry()));
        Editor editor;
        QVERIFY(editor.testAttribute(Qt::WA_AlwaysShowToolTips));
        const auto bar = editor.findChild<QToolBar *>();
        QCOMPARE(bar->toolButtonStyle(), Qt::ToolButtonIconOnly);
        for (auto action : bar->actions()) {
            if (action->isSeparator()) continue;
            QVERIFY(!action->icon().isNull());
            QVERIFY(!action->toolTip().isEmpty());
            QVERIFY(action->toolTip() != QStringLiteral("+"));
            QVERIFY(action->toolTip() != QStringLiteral("−"));
            auto button = bar->widgetForAction(action);
            QVERIFY(button);
            QCOMPARE(button->toolTip(), action->toolTip());
            showTip(button);
        }
    }

    void arrowPreviewMatchesExport()
    {
        QImage source(160, 120, QImage::Format_RGB32);
        source.fill(Qt::white);
        Editor editor;
        editor.setImage(source);
        editor.addArrow(QPointF(15, 18), QPointF(141, 96));
        QImage preview(source.size(), source.format());
        preview.fill(Qt::white);
        QPainter painter(&preview);
        editor.paintImage(painter, preview.rect(), source.rect());
        QVERIFY(!painter.testRenderHint(QPainter::Antialiasing));
        painter.end();
        QCOMPARE(preview, editor.renderedImage());
    }

    void mosaicExportAndUndo()
    {
        Editor editor;
        const auto source = document(140);
        editor.setImage(source);
        editor.addMosaic(QRectF(24, 24, 48, 36));
        const auto result = editor.renderedImage();
        QCOMPARE(result.size(), source.size());
        QCOMPARE(result.copy(0, 0, 20, 20), source.copy(0, 0, 20, 20));
        QVERIFY(result.copy(24, 24, 48, 36) != source.copy(24, 24, 48, 36));
        QCOMPARE(result.pixel(24, 24), result.pixel(30, 30));
        editor.view()->setZoom(0.25);
        QCOMPARE(editor.renderedImage(), result);
        QCOMPARE(editor.renderedImage(QRectF(20, 20, 60, 50)), result.copy(20, 20, 60, 50));
        editor.undoStack()->undo();
        QCOMPARE(editor.renderedImage(), source);
        editor.undoStack()->redo();
        QCOMPARE(editor.renderedImage(), result);
        editor.addMosaic(QRectF(-20, -20, 30, 30));
        QCOMPARE(editor.renderedImage().size(), source.size());
    }
    void pixelCardCandidates()
    {
        QImage image(360, 220, QImage::Format_RGB32);
        image.fill(Qt::white);
        const QRect card(30, 35, 240, 42);
        {
            QPainter painter(&image);
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor(237, 238, 239));
            painter.drawRoundedRect(card, 7, 7);
            painter.fillRect(QRect(70, 49, 85, 3), Qt::black);
            painter.setBrush(QColor(241, 242, 255));
            painter.drawRoundedRect(QRect(30, 90, 240, 42), 7, 7);
        }
        const auto regions = WindowsCapture::pixelRegions(image, QPoint(110, 50));
        QVERIFY(std::any_of(regions.begin(), regions.end(), [&](const QRect &area) {
            return (area.topLeft() - card.topLeft()).manhattanLength() <= 2
                && qAbs(area.width() - card.width()) <= 2 && qAbs(area.height() - card.height()) <= 2;
        }));
        QVERIFY(WindowsCapture::pixelRegions(image, QPoint(330, 180)).isEmpty());
        image.fill(Qt::white);
        QVERIFY(WindowsCapture::pixelRegions(image, QPoint(110, 50)).isEmpty());
    }
    void selectionBorderInteraction()
    {
        auto screen = QGuiApplication::primaryScreen();
        QVERIFY(screen);
        CaptureSelection result;
        SelectionOverlay overlay(screen, [&](SelectionOverlay *, CaptureSelection s) { result = s; },
            [] {}, [] {}, [](SelectionOverlay::Phase) {});
        auto mouse = [&](QEvent::Type type, QPoint point, Qt::MouseButton button = Qt::NoButton) {
            QMouseEvent event(type, QPointF(point), button,
                type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(&overlay, &event);
        };
        mouse(QEvent::MouseButtonPress, {100, 100}, Qt::LeftButton);
        mouse(QEvent::MouseButtonRelease, {300, 300}, Qt::LeftButton);
        const QPoint points[] = { {100, 100}, {200, 100}, {300, 100}, {300, 200},
                                 {300, 300}, {200, 300}, {100, 300}, {100, 200} };
        const Qt::CursorShape cursors[] = { Qt::SizeFDiagCursor, Qt::SizeVerCursor, Qt::SizeBDiagCursor,
            Qt::SizeHorCursor, Qt::SizeFDiagCursor, Qt::SizeVerCursor, Qt::SizeBDiagCursor, Qt::SizeHorCursor };
        for (int i = 0; i < 8; ++i) {
            mouse(QEvent::MouseMove, points[i]);
            QCOMPARE(overlay.cursor().shape(), cursors[i]);
        }
        mouse(QEvent::MouseMove, {200, 200});
        QCOMPARE(overlay.cursor().shape(), Qt::OpenHandCursor);
        mouse(QEvent::MouseButtonPress, {200, 200}, Qt::LeftButton);
        QCOMPARE(overlay.cursor().shape(), Qt::ClosedHandCursor);
        mouse(QEvent::MouseButtonRelease, {220, 220}, Qt::LeftButton);
        // Cross the opposite corner while retaining the originally grabbed edges.
        mouse(QEvent::MouseButtonPress, {120, 120}, Qt::LeftButton);
        mouse(QEvent::MouseButtonRelease, {360, 360}, Qt::LeftButton);
        overlay.findChild<QPushButton *>("captureStill")->click();
        QCOMPARE(result.output, WindowsCapture::toPhysical(QRect(QPoint(320, 320), QPoint(360, 360)),
            overlay.size(), WindowsCapture::screenRect(screen->name())));
    }
    void trayLifecycle()
    {
        QProcess app;
        app.start(QCoreApplication::applicationDirPath() + "/../dist/LongShot.exe", QStringList());
        QVERIFY(app.waitForStarted());
        QTest::qWait(1000);
        QCOMPARE(app.state(), QProcess::Running);
        QVERIFY(processWindow(app.processId()) == nullptr);
        const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        QVERIFY(snapshot != INVALID_HANDLE_VALUE);
        THREADENTRY32 entry = {};
        entry.dwSize = sizeof(entry);
        DWORD thread = 0;
        if (Thread32First(snapshot, &entry)) {
            do {
                if (entry.th32OwnerProcessID == DWORD(app.processId())) {
                    thread = entry.th32ThreadID;
                    break;
                }
            } while (Thread32Next(snapshot, &entry));
        }
        CloseHandle(snapshot);
        QVERIFY(thread != 0);
        // Exercise the application's hotkey dispatch without sending input to other apps.
        QVERIFY(PostThreadMessageW(thread, WM_HOTKEY, 0x4c54, 0));
        QTRY_VERIFY(processWindow(app.processId()) != nullptr);
        QVERIFY(PostThreadMessageW(thread, WM_HOTKEY, 0x4c53, 0));
        QTRY_VERIFY(processWindow(app.processId()) == nullptr);
        QCOMPARE(app.state(), QProcess::Running);
        QVERIFY(PostThreadMessageW(thread, WM_QUIT, 0, 0));
        QVERIFY(app.waitForFinished(3000));
        QCOMPARE(app.exitCode(), 0);
    }
    void translucentEditor()
    {
        Editor editor;
        editor.resize(900, 600);
        QImage source(200, 200, QImage::Format_RGB32);
        source.fill(QColor(240, 180, 80));
        editor.show();
        editor.setImage(source);
        QTest::qWait(100);
        const auto preview = editor.grab().toImage();
        const auto outside = editor.view()->mapTo(&editor, QPoint(20, 20));
        const auto inside = editor.view()->viewport()->mapTo(&editor,
            editor.view()->mapFromScene(QPointF(100, 100)));
        const auto shade = preview.pixelColor(outside);
        QVERIFY(shade.alpha() > 50 && shade.alpha() < 180);
        QCOMPARE(preview.pixelColor(inside), QColor(240, 180, 80));
        QCOMPARE(editor.renderedImage(), source);
        QVERIFY(preview.save(QCoreApplication::applicationDirPath() + "/editor-alpha.png"));
    }
    void editorCloseRetainsImage()
    {
        Editor editor;
        const auto image = document();
        editor.setImage(image);
        editor.addRectangle(QRectF(10, 10, 40, 40));
        const auto annotated = editor.renderedImage();
        editor.show();
        QVERIFY(editor.close());
        QVERIFY(!editor.isVisible());
        QCOMPARE(editor.renderedImage(), annotated);
        editor.show();
        editor.undoStack()->undo();
        QCOMPARE(editor.renderedImage(), image);
    }
    void deploymentSmoke()
    {
        QProcess app;
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.insert("PATH", environment.value("SystemRoot") + "\\System32");
        for (const auto &key : { "QT_PLUGIN_PATH", "QT_QPA_PLATFORM_PLUGIN_PATH", "QT_QPA_PLATFORM" })
            environment.remove(key);
        app.setProcessEnvironment(environment);
        QTemporaryDir input;
        QVERIFY(document().save(input.filePath("smoke.png")));
        app.start(QCoreApplication::applicationDirPath() + "/../dist/LongShot.exe",
                  { "--edit", input.filePath("smoke.png") });
        QVERIFY(app.waitForStarted());
        HWND window = nullptr;
        QTRY_VERIFY_WITH_TIMEOUT((window = processWindow(app.processId())) != nullptr, 10000);
        QCOMPARE(app.state(), QProcess::Running);
        RECT rect = {};
        QVERIFY(GetWindowRect(window, &rect));
        QVERIFY(SetWindowPos(window, HWND_TOPMOST, 0, 0, 0, 0,
                             SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE));
        QCOMPARE(nativeClick(QPoint((rect.left + rect.right) / 2, (rect.top + rect.bottom) / 2)), UINT(2));
        QTest::qWait(200);
        QVERIFY(SetWindowPos(window, HWND_NOTOPMOST, 0, 0, 0, 0,
                             SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE));
        QCOMPARE(GetForegroundWindow(), window);
        QString error;
        const auto image = WindowsCapture::grab(
            QRect(rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top), &error);
        QVERIFY2(!image.isNull(), qPrintable(error));
        QVERIFY(image.save(QCoreApplication::applicationDirPath() + "/editor.png"));
        QVERIFY(PostMessageW(window, WM_CLOSE, 0, 0));
        QVERIFY(app.waitForFinished(3000));
        QCOMPARE(app.exitStatus(), QProcess::NormalExit);
        QCOMPARE(app.exitCode(), 0);
    }
    void translucentStickyHeader()
    {
        const auto source = document(500);
        auto first = source.copy(0, 0, 180, 400);
        auto second = source.copy(0, 100, 180, 400);
        for (auto frame : { &first, &second }) {
            QPainter painter(frame);
            painter.fillRect(0, 0, 180, 32, QColor(255, 255, 255, 210));
            painter.fillRect(20, 10, 80, 12, Qt::black);
        }
        Stitcher stitch;
        QVERIFY(stitch.start(first));
        QCOMPARE(stitch.append(second), Stitcher::Result::Appended);
        QImage expected = source;
        QPainter painter(&expected);
        painter.drawImage(0, 0, first);
        painter.end();
        QCOMPARE(stitch.image(), expected);
    }
    void completeSelectionComposition()
    {
        const auto base = document(300);
        const QRect region(35, 60, 90, 100);
        QImage scrolling(90, 180, QImage::Format_RGB32);
        scrolling.fill(Qt::green);
        QString error;
        const auto result = composeCapture(base, scrolling, region, &error);
        QVERIFY2(!result.isNull(), qPrintable(error));
        QCOMPARE(result.size(), QSize(180, 380));
        QCOMPARE(result.copy(35, 60, 90, 180), scrolling);
        QCOMPARE(result.copy(0, 0, 180, 60), base.copy(0, 0, 180, 60));
        QCOMPARE(result.copy(0, 60, 35, 100), base.copy(0, 60, 35, 100));
        QCOMPARE(result.copy(125, 60, 55, 100), base.copy(125, 60, 55, 100));
        QCOMPARE(result.copy(0, 240, 180, 140), base.copy(0, 160, 180, 140));
        for (int y = 160; y < 240; ++y) {
            for (int x = 0; x < 180; ++x) {
                if (x < 35 || x >= 125) QCOMPARE(result.pixelColor(x, y), QColor(Qt::white));
            }
        }
        QCOMPARE(composeCapture(base, base.copy(region), region), base);
        QVERIFY(composeCapture(base, scrolling, QRect(-1, 0, 90, 100)).isNull());
        QVERIFY(!captureMemoryFits(QSize(1000, 1000), 31000, QSize(3000, 2000)));
        QVERIFY(captureMemoryFits(QSize(100, 100), 1000, QSize(500, 300)));
    }
    void dpiCoordinates()
    {
        for (qreal scale : { 1.0, 1.25, 1.5 }) {
            const QSize logical(1280, 720);
            const QRect physical(-qRound(1280 * scale), -180,
                                 qRound(1280 * scale), qRound(720 * scale));
            QCOMPARE(WindowsCapture::toPhysical(QRect(QPoint(), logical), logical, physical), physical);
            QCOMPARE(WindowsCapture::toPhysical(QRect(100, 80, 400, 200), logical, physical),
                     QRect(physical.x() + qRound(100 * scale), physical.y() + qRound(80 * scale),
                           qRound(400 * scale), qRound(200 * scale)));
        }
        QVERIFY(WindowsCapture::toPhysical(QRect(1, 1, 5, 5), {}, QRect(0, 0, 1920, 1080)).isEmpty());
    }
    void frameAndMemoryLimits()
    {
        Stitcher stitch;
        QVERIFY(!stitch.start(QImage()));
        QVERIFY(!stitch.start(QImage(5000, 3000, QImage::Format_RGB32)));
        const auto source = document();
        QVERIFY(stitch.start(source.copy(0, 0, 180, 100)));
        for (int i = 1; i < 100; ++i)
            QCOMPARE(stitch.append(source.copy(0, i * 2, 180, 100)), Stitcher::Result::Appended);
        const auto before = stitch.image();
        QCOMPARE(stitch.append(source.copy(0, 200, 180, 100)), Stitcher::Result::LimitReached);
        QCOMPARE(stitch.image(), before);
    }
    void desktopFlow_data()
    {
        QTest::addColumn<QString>("mode");
        QTest::newRow("activate-background-target") << QString("activate");
        QTest::newRow("automatic-bottom") << QString("bottom");
        QTest::newRow("outside-click") << QString("click");
        QTest::newRow("escape-cancel") << QString("cancel");
    }
    void desktopFlow()
    {
        if (QGuiApplication::platformName() == "offscreen") QSKIP("Requires the Windows desktop");
        QFETCH(QString, mode);
        Editor editor;
        CaptureController capture(&editor);
        QSignalSpy finished(&capture, &CaptureController::captureFinished);
        editor.show();
        struct FixtureProcess : QProcess {
            ~FixtureProcess() {
                if (state() != NotRunning) {
                    terminate();
                    if (!waitForFinished(1000)) { kill(); waitForFinished(1000); }
                }
            }
        } fixture;
        fixture.start(QCoreApplication::applicationDirPath() + "/longshot_fixture.exe", QStringList());
        QVERIFY(fixture.waitForStarted());
        QByteArray output;
        QTRY_VERIFY_WITH_TIMEOUT((output += fixture.readAllStandardOutput()).contains("RECT"), 5000);
        const auto parts = output.trimmed().split(' ');
        QCOMPARE(parts.size(), 5);
        const QRect physical(parts[1].toInt(), parts[2].toInt(), parts[3].toInt(), parts[4].toInt());
        const auto fixtureWindow = processWindow(fixture.processId());
        QVERIFY(fixtureWindow);
        QVERIFY(SetWindowPos(fixtureWindow, HWND_TOPMOST, 0, 0, 0, 0,
                             SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE));
        QCOMPARE(nativeClick(physical.center()), UINT(2));
        QTest::qWait(200);
        QVERIFY(SetWindowPos(fixtureWindow, HWND_NOTOPMOST, 0, 0, 0, 0,
                             SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE));
        QCOMPARE(GetForegroundWindow(), fixtureWindow);
        fixture.readAllStandardOutput();
        output.clear();
        QWidget foreground;
        if (mode == "activate") {
            foreground.setGeometry(10, 10, 80, 60);
            foreground.show();
            foreground.raise();
            foreground.activateWindow();
            QTRY_COMPARE(GetForegroundWindow(), reinterpret_cast<HWND>(foreground.winId()));
        }
        capture.start();
        QCOMPARE(capture.state(), CaptureController::State::Selecting);
        QWidget *overlay = nullptr;
        for (auto widget : QApplication::topLevelWidgets()) {
            if (widget->isVisible() && dynamic_cast<SelectionOverlay *>(widget)
                && nativeBounds(widget).contains(physical.center())) overlay = widget;
        }
        QVERIFY(overlay);
        const auto native = nativeBounds(overlay);
        const auto localArea = WindowsCapture::toLogical(physical, overlay->size(), native);
        const QPoint start = localArea.topLeft(), end = localArea.bottomRight();
        QTest::mousePress(overlay, Qt::LeftButton, Qt::NoModifier, start);
        QTest::mouseMove(overlay, end);
        QTest::mouseRelease(overlay, Qt::LeftButton, Qt::NoModifier, end);
        overlay->findChild<QPushButton *>("enterScroll")->click();
        QCOMPARE(capture.state(), CaptureController::State::ChoosingRegion);
        QVERIFY(!overlay->findChild<QComboBox *>("targetWindows")->isVisible());
        const auto startButton = overlay->findChild<QPushButton *>("startCapture");
        QVERIFY(startButton && startButton->isVisible() && startButton->isEnabled());
        QTest::qWait(100);
        const QPoint button = overlay->mapFromGlobal(startButton->mapToGlobal(startButton->rect().center()));
        const auto buttonPhysical = WindowsCapture::toPhysical(QRect(button, QSize(1, 1)),
                                                               overlay->size(), native).topLeft();
        QCOMPARE(nativeClick(buttonPhysical), UINT(2));
        QTRY_COMPARE(capture.state(), CaptureController::State::Capturing);
        QTest::qWait(1800);
        if (mode == "click") {
            QVERIFY2(finished.isEmpty(), finished.isEmpty() ? "" : qPrintable(finished[0][1].toString()));
            POINT click = { physical.x() + 15, physical.y() - 15 };
            SetCursorPos(click.x, click.y);
            const auto hit = GetAncestor(WindowFromPoint(click), GA_ROOT);
            QCOMPARE(hit, reinterpret_cast<HWND>(overlay->winId()));
            QVERIFY(GetWindowLongPtrW(hit, GWL_EXSTYLE) & WS_EX_NOACTIVATE);
            QCOMPARE(nativeClick(QPoint(click.x, click.y)), UINT(2));
        } else if (mode == "cancel") {
            QVERIFY(PostThreadMessageW(GetCurrentThreadId(), WM_HOTKEY, 0x4c53, 0));
            QTRY_COMPARE(capture.state(), CaptureController::State::Idle);
            QVERIFY(!editor.isVisible());
            QCOMPARE(finished.count(), 0);
            return;
        }
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 25000);
        const auto image = qvariant_cast<QImage>(finished[0][0]);
        const auto reason = finished[0][1].toString();
        QVERIFY2(!image.isNull(), qPrintable(reason));
        image.save(QCoreApplication::applicationDirPath() + "/capture-" + mode + ".png");
        output += fixture.readAllStandardOutput();
        QVERIFY2(image.height() > physical.height(), qPrintable(reason + " " + QString::fromUtf8(output)));
        QCOMPARE(capture.state(), CaptureController::State::Editing);
        QVERIFY(editor.isVisible());
        if (mode == "bottom" || mode == "activate") {
            QVERIFY2(reason.contains(QStringLiteral("滚动已无变化")), qPrintable(reason));
            QCOMPARE(image.height(), qRound(1200 * QGuiApplication::primaryScreen()->devicePixelRatio()));
            QVERIFY(image.save(QCoreApplication::applicationDirPath() + "/integration.png"));
        } else {
            QCOMPARE(reason, QStringLiteral("已停止采集"));
            QTest::qWait(100);
            output += fixture.readAllStandardOutput();
            QVERIFY2(!output.contains("CLICK"), output.constData());
        }
    }
    void multiTargetFlow_data()
    {
        QTest::addColumn<bool>("twoWindows");
        QTest::newRow("two-windows") << true;
        QTest::newRow("two-panes") << false;
    }
    void multiTargetFlow()
    {
        QFETCH(bool, twoWindows);
        struct Process : QProcess {
            ~Process() { if (state() != NotRunning) { kill(); waitForFinished(2000); } }
        } first, second;
        const auto executable = QCoreApplication::applicationDirPath() + "/longshot_fixture.exe";
        first.start(executable, twoWindows ? QStringList() : QStringList { "--two-panes" });
        QVERIFY(first.waitForStarted());
        QByteArray log;
        QTRY_VERIFY_WITH_TIMEOUT((log += first.readAllStandardOutput()).contains(twoWindows ? "RECT" : "SECOND"), 5000);
        auto parse = [](QByteArray line) {
            const auto p = line.trimmed().split(' ');
            return p.size() == 5 ? QRect(p[1].toInt(), p[2].toInt(), p[3].toInt(), p[4].toInt()) : QRect();
        };
        const auto lines = log.trimmed().split('\n');
        const QRect left = parse(lines[0]);
        QRect right = twoWindows ? QRect() : parse(lines[1]);
        if (twoWindows) {
            second.start(executable, { "--right" });
            QVERIFY(second.waitForStarted());
            QByteArray other;
            QTRY_VERIFY_WITH_TIMEOUT((other += second.readAllStandardOutput()).contains("RECT"), 5000);
            right = parse(other.trimmed());
        }
        QVERIFY(!left.isEmpty() && !right.isEmpty());
        const auto target = processWindow(first.processId());
        QVERIFY(target);
        QVERIFY(SetWindowPos(target, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE));
        QCOMPARE(nativeClick(left.center()), UINT(2));
        QTest::qWait(100);
        const QRect output = left.united(right);
        auto drag = [](QWidget *widget, QRect rect) {
            QTest::mousePress(widget, Qt::LeftButton, Qt::NoModifier, rect.topLeft());
            QTest::mouseMove(widget, rect.bottomRight());
            QTest::mouseRelease(widget, Qt::LeftButton, Qt::NoModifier, rect.bottomRight());
        };
        const auto candidates = WindowsCapture::candidates(output);
        QVERIFY(std::any_of(candidates.begin(), candidates.end(), [&](const WindowsCapture::WindowCandidate &c) {
            return c.window == target && c.process == DWORD(first.processId());
        }));
        if (twoWindows) QVERIFY(std::any_of(candidates.begin(), candidates.end(), [&](const WindowsCapture::WindowCandidate &c) {
            return c.process == DWORD(second.processId());
        }));
        Editor editor;
        CaptureController controller(&editor);
        QSignalSpy finished(&controller, &CaptureController::captureFinished);
        controller.start();
        SelectionOverlay *overlay = nullptr;
        for (auto widget : QApplication::topLevelWidgets())
            if (widget->isVisible() && nativeBounds(widget).contains(output.center()))
                if (auto candidate = dynamic_cast<SelectionOverlay *>(widget)) overlay = candidate;
        QVERIFY(overlay);
        auto local = [&](QRect rect) { return WindowsCapture::toLogical(rect, overlay->size(), nativeBounds(overlay)); };
        drag(overlay, local(output));
        overlay->findChild<QPushButton *>("enterScroll")->click();
        QTest::qWait(100);
        const POINT hitPoint = { left.center().x(), left.center().y() };
        QCOMPARE(GetAncestor(WindowFromPoint(hitPoint), GA_ROOT), reinterpret_cast<HWND>(overlay->winId()));
        auto choices = overlay->findChild<QComboBox *>("targetWindows");
        QVERIFY(choices);
        int index = -1;
        for (int i = 0; i < candidates.size(); ++i) if (candidates[i].window == target) index = i;
        QVERIFY(index >= 0);
        choices->setCurrentIndex(index);
        QVERIFY(QMetaObject::invokeMethod(choices, "activated", Q_ARG(int, index)));
        QCOMPARE(controller.state(), CaptureController::State::ChoosingRegion);
        const QRect scroll = twoWindows ? left : right;
        ++overlay->revision_;
        overlay->region_ = {};
        drag(overlay, local(scroll));
        const auto button = overlay->findChild<QPushButton *>("startCapture");
        QVERIFY(button && button->isEnabled());
        const auto buttonPoint = button->mapTo(overlay, button->rect().center());
        first.readAllStandardOutput();
        if (twoWindows) second.readAllStandardOutput();
        QCOMPARE(nativeClick(WindowsCapture::toPhysical(QRect(buttonPoint, QSize(1, 1)), overlay->size(), nativeBounds(overlay)).topLeft()), UINT(2));
        QTRY_COMPARE(controller.state(), CaptureController::State::Capturing);
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 25000);
        const auto image = qvariant_cast<QImage>(finished[0][0]);
        QVERIFY2(image.height() > output.height(), qPrintable(finished[0][1].toString()));
        QCOMPARE(image.width(), output.width());
        log = first.readAllStandardOutput();
        if (twoWindows) {
            QVERIFY(log.contains("SCROLL1"));
            QVERIFY(!second.readAllStandardOutput().contains("SCROLL1"));
        } else {
            QVERIFY2(log.contains("SCROLL2"), log.constData());
            QVERIFY2(!log.contains("SCROLL1"), log.constData());
        }
        QVERIFY(image.save(QCoreApplication::applicationDirPath()
                           + (twoWindows ? "/two-windows.png" : "/two-panes.png")));
    }
    void surroundedWindowFlow()
    {
        const auto screen = QGuiApplication::primaryScreen();
        if (screen->availableGeometry().width() < 1850 || screen->availableGeometry().height() < 1270)
            QSKIP("Five-window layout requires a desktop of at least 1850 x 1270 logical pixels");
        struct Process : QProcess {
            ~Process() { if (state() != NotRunning) { kill(); waitForFinished(2000); } }
        } fixtures[5];
        const QPoint positions[] = { {700, 460}, {700, 50}, {700, 870}, {80, 460}, {1320, 460} };
        QRect areas[5];
        QRect output;
        for (int i = 0; i < 5; ++i) {
            fixtures[i].start(QCoreApplication::applicationDirPath() + "/longshot_fixture.exe",
                { "--position", QString::number(positions[i].x()), QString::number(positions[i].y()) });
            QVERIFY(fixtures[i].waitForStarted());
            QByteArray log;
            QTRY_VERIFY_WITH_TIMEOUT((log += fixtures[i].readAllStandardOutput()).contains("RECT"), 5000);
            const auto p = log.trimmed().split(' ');
            QCOMPARE(p.size(), 5);
            areas[i] = QRect(p[1].toInt(), p[2].toInt(), p[3].toInt(), p[4].toInt());
            output = output.united(areas[i]);
        }
        const auto target = processWindow(fixtures[0].processId());
        QVERIFY(target);
        QVERIFY(SetWindowPos(target, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE));
        QCOMPARE(nativeClick(areas[0].center()), UINT(2));
        QTest::qWait(150);
        const auto baseline = WindowsCapture::grab(output, nullptr);
        QVERIFY(!baseline.isNull());
        for (auto &fixture : fixtures) fixture.readAllStandardOutput();
        const auto candidates = WindowsCapture::candidates(output);
        int index = -1;
        for (int i = 0; i < 5; ++i) {
            bool found = false;
            for (int j = 0; j < candidates.size(); ++j) {
                if (candidates[j].process == DWORD(fixtures[i].processId())) {
                    found = true;
                    if (i == 0) index = j;
                }
            }
            QVERIFY2(found, "A surrounding window was missing from the candidates");
        }
        Editor editor;
        CaptureController controller(&editor);
        QSignalSpy finished(&controller, &CaptureController::captureFinished);
        controller.start();
        SelectionOverlay *overlay = nullptr;
        for (auto widget : QApplication::topLevelWidgets())
            if (widget->isVisible() && nativeBounds(widget).contains(output.center()))
                if (auto candidate = dynamic_cast<SelectionOverlay *>(widget)) overlay = candidate;
        QVERIFY(overlay);
        auto drag = [&](QRect physical) {
            const auto local = WindowsCapture::toLogical(physical, overlay->size(), nativeBounds(overlay));
            QTest::mousePress(overlay, Qt::LeftButton, Qt::NoModifier, local.topLeft());
            QTest::mouseMove(overlay, local.bottomRight());
            QTest::mouseRelease(overlay, Qt::LeftButton, Qt::NoModifier, local.bottomRight());
        };
        drag(output);
        overlay->findChild<QPushButton *>("enterScroll")->click();
        auto choices = overlay->findChild<QComboBox *>("targetWindows");
        QVERIFY(choices);
        QVERIFY(QMetaObject::invokeMethod(choices, "activated", Q_ARG(int, index)));
        ++overlay->revision_;
        overlay->region_ = {};
        drag(areas[0]);
        const auto start = overlay->findChild<QPushButton *>("startCapture");
        QVERIFY(start && start->isEnabled());
        start->click();
        QTRY_COMPARE(controller.state(), CaptureController::State::Capturing);
        QTest::qWait(1200);
        QVERIFY2(finished.isEmpty(), "Capture stopped before the surrounding-window click check");
        const POINT staticPoint = { areas[3].center().x(), areas[3].center().y() };
        QCOMPARE(GetAncestor(WindowFromPoint(staticPoint), GA_ROOT), reinterpret_cast<HWND>(overlay->winId()));
        QCOMPARE(nativeClick(areas[3].center()), UINT(2));
        QTest::qWait(200);
        QVERIFY(finished.isEmpty());
        SetCursorPos(areas[0].center().x(), areas[0].center().y());
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 25000);
        const auto image = qvariant_cast<QImage>(finished[0][0]);
        QVERIFY2(image.height() > output.height(), qPrintable(finished[0][1].toString()));
        QCOMPARE(image.width(), output.width());
        const int extra = image.height() - output.height();
        for (int i = 1; i < 5; ++i) {
            const QRect original = areas[i].translated(-output.topLeft());
            const QRect destination = original.translated(0, i == 2 ? extra : 0);
            QCOMPARE(image.copy(destination), baseline.copy(original));
            const auto log = fixtures[i].readAllStandardOutput();
            QVERIFY2(!log.contains("SCROLL") && !log.contains("CLICK"), log.constData());
        }
        QVERIFY(fixtures[0].readAllStandardOutput().contains("SCROLL1"));
        QVERIFY(image.save(QCoreApplication::applicationDirPath() + "/surrounded-windows.png"));
    }
    void regionProbeExpiry()
    {
        QProcess fixture;
        fixture.start(QCoreApplication::applicationDirPath() + "/longshot_fixture.exe", QStringList());
        QVERIFY(fixture.waitForStarted());
        QByteArray output;
        QTRY_VERIFY((output += fixture.readAllStandardOutput()).contains("RECT"));
        const auto p = output.trimmed().split(' ');
        const QRect area(p[1].toInt(), p[2].toInt(), p[3].toInt(), p[4].toInt());
        const auto target = processWindow(fixture.processId());
        SetWindowPos(target, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
        const auto screen = QGuiApplication::primaryScreen();
        const auto local = WindowsCapture::toLogical(area, screen->size(), WindowsCapture::screenRect(screen->name()));
        CaptureSelection chosen;
        SelectionOverlay overlay(screen, [&](SelectionOverlay *, CaptureSelection s) { chosen = s; }, [] {}, [] {},
            [](SelectionOverlay::Phase) {}, [area](HWND, QPoint) {
                QThread::msleep(1100); return area;
            });
        overlay.show();
        auto drag = [&](QRect rect) {
            QTest::mousePress(&overlay, Qt::LeftButton, Qt::NoModifier, rect.topLeft());
            QTest::mouseMove(&overlay, rect.bottomRight());
            QTest::mouseRelease(&overlay, Qt::LeftButton, Qt::NoModifier, rect.bottomRight());
        };
        drag(local);
        overlay.findChild<QPushButton *>("enterScroll")->click();
        QTest::mouseClick(&overlay, Qt::LeftButton, Qt::NoModifier, local.center());
        QTest::qWait(850);
        const auto start = overlay.findChild<QPushButton *>("startCapture");
        QVERIFY(!start->isEnabled());
        const auto manual = local.adjusted(20, 20, -20, -20);
        drag(manual);
        QTest::qWait(400);
        QVERIFY(start->isEnabled());
        start->click();
        QCOMPARE(chosen.region, WindowsCapture::toPhysical(manual, screen->size(), WindowsCapture::screenRect(screen->name())));
        QTest::mouseClick(&overlay, Qt::LeftButton, Qt::NoModifier, local.center());
        const auto adjusted = local.adjusted(30, 30, -30, -30);
        drag(adjusted);
        QTest::qWait(1200);
        start->click();
        QCOMPARE(chosen.region, WindowsCapture::toPhysical(adjusted, screen->size(), WindowsCapture::screenRect(screen->name())));
        const QPoint edge(adjusted.right(), adjusted.center().y());
        QTest::mousePress(&overlay, Qt::LeftButton, Qt::NoModifier, edge);
        QTest::mouseMove(&overlay, edge - QPoint(10, 0));
        QTest::mouseRelease(&overlay, Qt::LeftButton, Qt::NoModifier, edge - QPoint(10, 0));
        start->click();
        QCOMPARE(chosen.region, WindowsCapture::toPhysical(adjusted.adjusted(0, 0, -10, 0),
            screen->size(), WindowsCapture::screenRect(screen->name())));
        fixture.kill(); fixture.waitForFinished(2000);
        chosen = {};
        start->click();
        QVERIFY(chosen.region.isEmpty());
    }
    void overlapAndShortFinal()
    {
        const auto source = document();
        Stitcher stitch;
        QVERIFY(stitch.start(source.copy(0, 0, 180, 300)));
        QCOMPARE(stitch.append(source.copy(0, 130, 180, 300)), Stitcher::Result::Appended);
        QCOMPARE(stitch.append(source.copy(0, 260, 180, 300)), Stitcher::Result::Appended);
        QCOMPARE(stitch.append(source.copy(0, 285, 180, 300)), Stitcher::Result::Appended);
        QCOMPARE(stitch.image(), source.copy(0, 0, 180, 585));
        QCOMPARE(stitch.append(source.copy(0, 285, 180, 300)), Stitcher::Result::Unchanged);
    }
    void fixedHeaderFooter()
    {
        const auto source = document();
        auto frame = [&](int offset) {
            QImage result(180, 300, QImage::Format_RGB32);
            result.fill(Qt::white);
            QPainter painter(&result);
            painter.fillRect(0, 0, 180, 20, Qt::blue);
            painter.drawImage(0, 20, source.copy(0, offset, 180, 250));
            painter.fillRect(0, 270, 180, 30, Qt::red);
            return result;
        };
        Stitcher stitch;
        QVERIFY(stitch.start(frame(0)));
        QCOMPARE(stitch.append(frame(100)), Stitcher::Result::Appended);
        QCOMPARE(stitch.append(frame(200)), Stitcher::Result::Appended);
        QImage expected(180, 500, QImage::Format_RGB32);
        QPainter painter(&expected);
        painter.fillRect(0, 0, 180, 20, Qt::blue);
        painter.drawImage(0, 20, source.copy(0, 0, 180, 450));
        painter.fillRect(0, 470, 180, 30, Qt::red);
        painter.end();
        QCOMPARE(stitch.image(), expected);
    }
    void narrowContentAndStaticSidebar()
    {
        const auto source = document();
        auto frame = [&](int offset) {
            QImage image(180, 300, QImage::Format_RGB32);
            image.fill(Qt::white);
            QPainter painter(&image);
            painter.drawImage(0, 0, source.copy(0, offset, 50, 300));
            painter.drawImage(120, 0, source.copy(120, 0, 60, 300));
            return image;
        };
        for (bool sidebar : { true, false }) {
            auto first = frame(0);
            auto next = frame(100);
            if (!sidebar) {
                QPainter(&first).fillRect(120, 0, 60, 300, Qt::white);
                QPainter(&next).fillRect(120, 0, 60, 300, Qt::white);
            }
            Stitcher stitch;
            QVERIFY(stitch.start(first));
            QCOMPARE(stitch.append(next), Stitcher::Result::Appended);
            auto result = stitch.image();
            QCOMPARE(result.copy(0, 0, 50, 400), source.copy(0, 0, 50, 400));
        }
    }
    void lowContrastOverlap()
    {
        auto source = document();
        for (int y = 0; y < source.height(); ++y) {
            auto row = reinterpret_cast<QRgb *>(source.scanLine(y));
            for (int x = 0; x < source.width(); ++x) {
                const int gray = 250 + qRed(row[x]) % 6;
                row[x] = qRgb(gray, gray, gray);
            }
        }
        Stitcher stitch;
        QVERIFY(stitch.start(source.copy(0, 0, 180, 300)));
        QCOMPARE(stitch.append(source.copy(0, 100, 180, 300)), Stitcher::Result::Appended);
        QCOMPARE(stitch.append(source.copy(0, 200, 180, 300)), Stitcher::Result::Appended);
        QCOMPARE(stitch.image(), source.copy(0, 0, 180, 500));
        QCOMPARE(stitch.append(source.copy(0, 650, 180, 300)), Stitcher::Result::Rejected);
    }
    void scrollingBlankMargins()
    {
        auto source = document();
        QPainter painter(&source);
        for (int y : { 0, 100, 280, 380 }) painter.fillRect(0, y, 180, 20, Qt::white);
        painter.end();
        Stitcher stitch;
        QVERIFY(stitch.start(source.copy(0, 0, 180, 300)));
        QCOMPARE(stitch.append(source.copy(0, 100, 180, 300)), Stitcher::Result::Appended);
        QCOMPARE(stitch.append(source.copy(0, 200, 180, 300)), Stitcher::Result::Appended);
        QCOMPARE(stitch.image(), source.copy(0, 0, 180, 500));
    }
    void repeatedRowsWithUniqueNames()
    {
        QImage source(600, 1000, QImage::Format_RGB32);
        source.fill(Qt::white);
        QPainter painter(&source);
        painter.setPen(Qt::black);
        for (int y = 0; y < 1000; y += 30) {
            painter.fillRect(0, y, 600, 1, QColor(220, 220, 220));
            painter.drawText(35, y + 20, QString("file_%1.cpp").arg(y));
            painter.drawText(240, y + 20, "same commit");
            painter.drawText(450, y + 20, "yesterday");
        }
        painter.end();
        Stitcher stitch;
        QVERIFY(stitch.start(source.copy(0, 0, 600, 300)));
        QCOMPARE(stitch.append(source.copy(0, 110, 600, 300)), Stitcher::Result::Appended);
        QCOMPARE(stitch.image(), source.copy(0, 0, 600, 410));
        QVERIFY(stitch.start(source.copy(0, 0, 600, 300)));
        QCOMPARE(stitch.append(source.copy(0, 120, 600, 300)), Stitcher::Result::Appended);
        QCOMPARE(stitch.image(), source.copy(0, 0, 600, 420));
        auto first = source.copy(0, 0, 600, 300);
        auto second = source.copy(0, 110, 600, 300);
        for (auto frame : { &first, &second }) {
            QPainter highlight(frame);
            highlight.setCompositionMode(QPainter::CompositionMode_Multiply);
            highlight.fillRect(0, 100, 600, 42, QColor(210, 230, 255));
        }
        QVERIFY(stitch.start(first));
        QCOMPARE(stitch.append(second), Stitcher::Result::Appended);
        QCOMPARE(stitch.image().size(), QSize(600, 410));
    }
    void adaptiveScrollStep()
    {
        const auto source = document();
        Stitcher stitch;
        QVERIFY(stitch.start(source.copy(0, 0, 180, 300)));
        QCOMPARE(stitch.nextScrollTicks(1), 1);
        QCOMPARE(stitch.append(source.copy(0, 30, 180, 300)), Stitcher::Result::Appended);
        QCOMPARE(stitch.nextScrollTicks(1), 5);
        QCOMPARE(stitch.append(source.copy(0, 180, 180, 300)), Stitcher::Result::Appended);
        QCOMPARE(stitch.nextScrollTicks(5), 5);
        QCOMPARE(stitch.image(), source.copy(0, 0, 180, 480));
    }
    void rejectsUnrelatedAndAmbiguous()
    {
        Stitcher stitch;
        auto source = document();
        stitch.start(source.copy(0, 0, 180, 300));
        QCOMPARE(stitch.append(source.copy(0, 600, 180, 300)), Stitcher::Result::Rejected);
        QCOMPARE(stitch.image(), source.copy(0, 0, 180, 300));
        QImage repeated(180, 300, QImage::Format_RGB32);
        for (int y = 0; y < 300; ++y)
            memcpy(repeated.scanLine(y), source.constScanLine(y % 30), 180 * 4);
        stitch.start(repeated);
        QImage shifted = repeated.copy();
        for (int y = 0; y < 300; ++y)
            memcpy(shifted.scanLine(y), source.constScanLine((y + 10) % 30), 180 * 4);
        QCOMPARE(stitch.append(shifted), Stitcher::Result::Rejected);
    }
    void longImagePreviewPan()
    {
        Editor editor;
        editor.show();
        QTest::qWait(50);
        QImage source(800, 31060, QImage::Format_RGB32);
        source.fill(Qt::white);
        editor.setImage(source);
        auto view = editor.view();
        QVERIFY(view->zoom() > 0.5);
        QCOMPARE(view->verticalScrollBar()->value(), view->verticalScrollBar()->minimum());
        view->verticalScrollBar()->setValue(500);
        const int before = view->verticalScrollBar()->value();
        const QPoint start(200, 250), end(200, 150);
        QTest::mousePress(view->viewport(), Qt::LeftButton, Qt::NoModifier, start);
        QMouseEvent move(QEvent::MouseMove, end, Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(view->viewport(), &move);
        QTest::mouseRelease(view->viewport(), Qt::LeftButton, Qt::NoModifier, end);
        QVERIFY(view->verticalScrollBar()->value() > before);
        QCOMPARE(editor.renderedImage(), source);
        view->setTool(EditorView::Tool::Rectangle);
        QCOMPARE(view->dragMode(), QGraphicsView::NoDrag);
        view->setTool(EditorView::Tool::Select);
        QCOMPARE(view->dragMode(), QGraphicsView::ScrollHandDrag);
    }
    void cropAnnotationUndoAndExport()
    {
        Editor editor;
        QImage source(200, 300, QImage::Format_RGB32);
        source.fill(Qt::white);
        editor.setImage(source);
        editor.addRectangle(QRectF(20, 20, 80, 60));
        const auto annotated = editor.renderedImage();
        QVERIFY(annotated != source);
        editor.view()->setZoom(2);
        QCOMPARE(editor.renderedImage(), annotated);
        editor.crop(QRectF(10, 10, 100, 120));
        QCOMPARE(editor.renderedImage(), annotated.copy(10, 10, 100, 120));
        editor.undoStack()->undo();
        QCOMPARE(editor.renderedImage(), annotated);
        editor.undoStack()->undo();
        QCOMPARE(editor.renderedImage(), source);
        editor.undoStack()->redo();
        editor.addArrow(QPointF(30, 100), QPointF(150, 160));
        editor.addText(QPointF(10, 200), QStringLiteral("测试"));
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QString error;
        const auto path = dir.filePath("result.png");
        QVERIFY2(editor.savePng(path, &error), qPrintable(error));
        QCOMPARE(QImage(path), editor.renderedImage());
        editor.copyImage();
        QCOMPARE(QApplication::clipboard()->image(), editor.renderedImage());
        QVERIFY(!editor.savePng(dir.filePath("missing/result.png"), &error));
        QVERIFY(!error.isEmpty());
    }
};

int main(int argc, char **argv)
{
    WindowsCapture::enableDpiAwareness();
    QApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
    QApplication app(argc, argv);
    CoreTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "coretests.moc"
