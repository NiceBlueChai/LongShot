#include "capturecontroller.h"
#include "editor.h"
#include "windowscapture.h"
#include "selectionoverlay.h"
#include "capturecomposition.h"

#include <QApplication>
#include <QClipboard>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <QGuiApplication>
#include <QScreen>

namespace {
constexpr int EscapeHotkey = 0x4c53;
constexpr int FinishHotkey = 0x4c57;

}

CaptureController::CaptureController(Editor *editor) : QObject(editor), editor_(editor)
{
    timer_.setInterval(50);
    connect(&timer_, &QTimer::timeout, this, &CaptureController::poll);
    connect(editor_, &Editor::newCapture, this, &CaptureController::start);
    connect(qApp, &QGuiApplication::screenRemoved, this, [this] {
        if (state_ == State::Capturing) stop(QStringLiteral("显示器配置变化，已停止采集"));
        else if (state_ != State::Idle && state_ != State::Editing) cancel();
    });
    qApp->installNativeEventFilter(this);
}

CaptureController::~CaptureController()
{
    qApp->removeNativeEventFilter(this);
    closeOverlays();
}

void CaptureController::start()
{
    if (state_ != State::Idle && state_ != State::Editing) return;
    hotkey_ = RegisterHotKey(nullptr, EscapeHotkey, MOD_NOREPEAT, VK_ESCAPE) != FALSE;
    if (!hotkey_) {
        emit captureFinished({}, QStringLiteral("Esc 热键注册失败，请关闭占用 Esc 的截图软件后重试"));
        return;
    }
    state_ = State::Selecting;
    ++generation_;
    editor_->hide();
    for (auto screen : QGuiApplication::screens()) {
        screenConnections_.append(connect(screen, &QScreen::geometryChanged, this, [this] {
            if (state_ == State::Capturing) stop(QStringLiteral("显示器尺寸或缩放变化，已停止采集"));
            else if (state_ != State::Idle && state_ != State::Editing) cancel();
        }));
        auto overlay = new SelectionOverlay(screen,
            [this](SelectionOverlay *chosen, CaptureSelection selection) { begin(chosen, selection); },
            [this] { stop(QStringLiteral("已停止采集")); }, [this] { cancel(); },
            [this](SelectionOverlay::Phase phase) {
                if (state_ == State::Capturing) return;
                state_ = phase == SelectionOverlay::Phase::Outer ? State::Selecting
                    : phase == SelectionOverlay::Phase::Windows ? State::ChoosingWindow : State::ChoosingRegion;
            });
        if (!overlay->valid()) delete overlay;
        else { overlays_.append(overlay); overlay->show(); }
    }
    if (overlays_.isEmpty()) {
        cancel();
        emit captureFinished({}, QStringLiteral("无法识别显示器"));
    }
}

void CaptureController::recover(SelectionOverlay *chosen, const QString &reason)
{
    ++generation_;
    timer_.stop();
    if (finishHotkey_) { UnregisterHotKey(nullptr, FinishHotkey); finishHotkey_ = false; }
    state_ = State::ChoosingWindow;
    base_ = {};
    candidate_ = {};
    stitch_ = Stitcher();
    for (auto overlay : overlays_) { overlay->clearMask(); overlay->show(); }
    chosen->recover(reason);
}

void CaptureController::begin(SelectionOverlay *chosen, CaptureSelection selection)
{
    if (!selection.window) {
        if (state_ != State::Selecting && state_ != State::ChoosingWindow && state_ != State::ChoosingRegion) return;
        if (!selection.image.isNull()) {
            const auto image = selection.image;
            closeOverlays();
            state_ = State::Editing;
            editor_->setImage(image);
            QApplication::clipboard()->setImage(image);
            emit captureFinished(image, QStringLiteral("已复制到剪贴板"));
            return;
        }
        state_ = State::Capturing;
        const int generation = generation_;
        for (auto overlay : overlays_) overlay->hide();
        QTimer::singleShot(150, this, [this, chosen, selection, generation] {
            if (generation != generation_ || state_ != State::Capturing) return;
            QString error;
            const auto image = WindowsCapture::grab(selection.output, &error);
            if (image.isNull()) {
                state_ = State::Selecting;
                for (auto overlay : overlays_) overlay->show();
                chosen->recover(error, true);
                return;
            }
            closeOverlays();
            state_ = State::Editing;
            if (selection.edit) {
                editor_->show();
                editor_->raise();
                editor_->activateWindow();
            }
            editor_->setImage(image);
            if (!selection.edit) QApplication::clipboard()->setImage(image);
            emit captureFinished(image, selection.edit ? QStringLiteral("截图完成") : QStringLiteral("已复制到剪贴板"));
        });
        return;
    }
    if (state_ != State::ChoosingRegion) return;
    if (!selection.output.contains(selection.region) || !selection.region.contains(selection.wheel)
        || !WindowsCapture::validTarget(selection.window, selection.process, selection.region)) {
        recover(chosen, QStringLiteral("目标窗口或滚动区域失效，请重新选择")); return;
    }
    if (!captureMemoryFits(selection.region.size(), selection.region.height(), selection.output.size())) {
        recover(chosen, QStringLiteral("选区超过图像内存上限，请缩小外框或滚动区域")); return;
    }
    state_ = State::Capturing;
    outputRect_ = selection.output;
    rect_ = selection.region;
    wheelPoint_ = selection.wheel;
    target_ = selection.window;
    targetProcess_ = selection.process;
    stitch_ = Stitcher();
    base_ = {};
    unchanged_ = 0;
    scrollTicks_ = 1;
    for (auto overlay : overlays_) overlay->hide();
    const int generation = generation_;
    QTimer::singleShot(200, this, [this, chosen, generation] {
        if (generation != generation_ || state_ != State::Capturing) return;
        if (!WindowsCapture::validTarget(target_, targetProcess_, rect_)
            || !GetWindowRect(target_, &targetRect_)) {
            recover(chosen, QStringLiteral("目标窗口已变化，请重新选择")); return;
        }
        QString error;
        base_ = WindowsCapture::grab(outputRect_, &error);
        if (base_.isNull()) { recover(chosen, error); return; }
        SetForegroundWindow(target_);
        waitForActivation(chosen, generation, 0);
    });
}

void CaptureController::waitForActivation(SelectionOverlay *chosen, int generation, int attempt)
{
    if (generation != generation_ || state_ != State::Capturing) return;
    if (!IsWindow(target_)) { recover(chosen, QStringLiteral("目标窗口已关闭，请重新选择")); return; }
    if (GetForegroundWindow() != target_) {
        if (attempt >= 20) {
            recover(chosen, QStringLiteral("目标窗口未切到前台，请切换到目标窗口后重新选择"));
            return;
        }
        QTimer::singleShot(50, this, [this, chosen, generation, attempt] {
            waitForActivation(chosen, generation, attempt + 1);
        });
        return;
    }
    SetCursorPos(wheelPoint_.x(), wheelPoint_.y());
    chosen->capturing();
    for (auto overlay : overlays_) {
        if (overlay != chosen) overlay->capturing(false);
        overlay->show();
    }
    QTimer::singleShot(200, this, [this, chosen, generation] {
        if (generation != generation_ || state_ != State::Capturing) return;
        if (!WindowsCapture::validTarget(target_, targetProcess_, rect_)) {
            recover(chosen, QStringLiteral("目标区域失效，请重新选择")); return;
        }
        if (!targetValid()) return;
        QString error;
        auto frame = WindowsCapture::grab(rect_, &error);
        if (!stitch_.start(frame, outputRect_.size())) {
            recover(chosen, error.isEmpty() ? stitch_.error() : error); return;
        }
        finishHotkey_ = RegisterHotKey(nullptr, FinishHotkey, MOD_NOREPEAT, 'Q') != FALSE;
        for (auto overlay : overlays_) overlay->completionKey(finishHotkey_);
        scroll();
    });
}

void CaptureController::scroll()
{
    if (!targetValid()) return;
    QString error;
    POINT cursor = {};
    if (!GetCursorPos(&cursor) || !rect_.contains(QPoint(cursor.x, cursor.y))
        || GetAncestor(WindowFromPoint(cursor), GA_ROOT) != target_) {
        const int generation = generation_;
        QTimer::singleShot(120, this, [this, generation] {
            if (generation == generation_ && state_ == State::Capturing) scroll();
        });
        return;
    }
    if (!WindowsCapture::scrollDown(scrollTicks_, &error)) { stop(error); return; }
    candidate_ = {};
    stable_ = 0;
    scrollTime_.restart();
    timer_.start();
}

void CaptureController::poll()
{
    if (state_ != State::Capturing) return;
    if (!targetValid()) return;
    QString error;
    auto frame = WindowsCapture::grab(rect_, &error);
    if (frame.isNull()) { stop(error); return; }
    if (!candidate_.isNull() && Stitcher::similar(candidate_, frame)) ++stable_;
    else stable_ = 0;
    candidate_ = frame;
    if (scrollTime_.elapsed() > 4000) {
        stop(QStringLiteral("画面持续变化，无法可靠拼接；已保留确认的内容"));
        return;
    }
    if (stable_ < 2 || scrollTime_.elapsed() < 200) return;
    const auto result = stitch_.append(frame);
    if (result == Stitcher::Result::Unchanged && scrollTime_.elapsed() < 600) return;
    timer_.stop();
    if (result == Stitcher::Result::Unchanged) {
        if (++unchanged_ >= 3) {
            stop(QStringLiteral("滚动已无变化：已到底，或目标未响应滚轮"));
            return;
        }
    } else if (result == Stitcher::Result::Appended) {
        unchanged_ = 0;
        scrollTicks_ = stitch_.nextScrollTicks(scrollTicks_);
    } else {
        QString reason = stitch_.error();
        if (result == Stitcher::Result::Rejected) {
            const QString path = QStandardPaths::writableLocation(QStandardPaths::TempLocation)
                + "/LongShot-diagnostics/" + QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss-zzz");
            if (QDir().mkpath(path) && stitch_.lastFrame().save(path + "/previous.png")
                && frame.save(path + "/current.png")) {
                QFile details(path + "/details.txt");
                if (details.open(QIODevice::WriteOnly)) {
                    details.write(QString("%1\nscrollTicks=%2 elapsedMs=%3 stableSamples=%4\nrect=%5,%6,%7,%8\n")
                        .arg(reason).arg(scrollTicks_).arg(scrollTime_.elapsed()).arg(stable_)
                        .arg(rect_.x()).arg(rect_.y()).arg(rect_.width()).arg(rect_.height()).toUtf8());
                }
                reason += QStringLiteral("；诊断帧已保存：") + QDir::toNativeSeparators(path);
            } else {
                reason += QStringLiteral("；诊断帧保存失败");
            }
        }
        stop(reason);
        return;
    }
    scroll();
}

bool CaptureController::targetValid()
{
    RECT bounds = {};
    if (!WindowsCapture::validTarget(target_, targetProcess_, rect_)
        || !GetWindowRect(target_, &bounds)
        || bounds.left != targetRect_.left || bounds.top != targetRect_.top
        || bounds.right != targetRect_.right || bounds.bottom != targetRect_.bottom
        || GetForegroundWindow() != target_) {
        stop(QStringLiteral("目标窗口关闭、移动或失去焦点，已保留采集结果"));
        return false;
    }
    return true;
}

void CaptureController::closeOverlays()
{
    ++generation_;
    timer_.stop();
    if (finishHotkey_) { UnregisterHotKey(nullptr, FinishHotkey); finishHotkey_ = false; }
    if (hotkey_) { UnregisterHotKey(nullptr, EscapeHotkey); hotkey_ = false; }
    for (const auto &connection : screenConnections_) disconnect(connection);
    screenConnections_.clear();
    for (auto overlay : overlays_) { overlay->hide(); overlay->deleteLater(); }
    overlays_.clear();
}

void CaptureController::stop(const QString &reason)
{
    if (state_ != State::Capturing) return;
    closeOverlays();
    QString compositionError;
    const auto scroll = stitch_.image();
    const auto image = scroll.isNull() ? QImage() : composeCapture(base_, scroll,
        rect_.translated(-outputRect_.topLeft()), &compositionError);
    base_ = {};
    candidate_ = {};
    stitch_ = Stitcher();
    const QString message = compositionError.isEmpty() ? reason : reason + "；" + compositionError;
    state_ = State::Editing;
    if (!image.isNull()) {
        editor_->show();
        editor_->raise();
        editor_->activateWindow();
        editor_->setImage(image, message);
    }
    emit captureFinished(image, message);
}

void CaptureController::cancel()
{
    closeOverlays();
    state_ = State::Idle;
    base_ = {};
    candidate_ = {};
    stitch_ = Stitcher();
    editor_->hide();
}

bool CaptureController::nativeEventFilter(const QByteArray &, void *message, long *result)
{
    auto native = static_cast<MSG *>(message);
    if (native->message == WM_HOTKEY && native->wParam == FinishHotkey) {
        if (finishHotkey_ && state_ == State::Capturing) stop(QStringLiteral("已停止采集"));
        if (result) *result = 0;
        return true;
    }
    if (native->message == WM_HOTKEY && native->wParam == EscapeHotkey) {
        if (auto modal = QApplication::activeModalWidget()) modal->close();
        else cancel();
        if (result) *result = 0;
        return true;
    }
    return false;
}
