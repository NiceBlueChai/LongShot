#pragma once

#include "stitcher.h"
#include "selectionoverlay.h"
#include <QAbstractNativeEventFilter>
#include <QElapsedTimer>
#include <QObject>
#include <QTimer>
#include <QVector>
#include <Windows.h>

class Editor;
class SelectionOverlay;

class CaptureController : public QObject, public QAbstractNativeEventFilter
{
    Q_OBJECT
public:
    enum class State { Idle, Selecting, ChoosingWindow, ChoosingRegion, Capturing, Editing };
    explicit CaptureController(Editor *editor);
    ~CaptureController() override;
    void start();
    void stop(const QString &reason = QString());
    void cancel();
    State state() const { return state_; }
    bool nativeEventFilter(const QByteArray &type, void *message, long *result) override;
signals:
    void captureFinished(const QImage &image, const QString &reason);
private:
    void begin(SelectionOverlay *overlay, CaptureSelection selection);
    void recover(SelectionOverlay *overlay, const QString &reason);
    void waitForActivation(SelectionOverlay *chosen, int generation, int attempt);
    void poll();
    void scroll();
    bool targetValid();
    void closeOverlays();
    Editor *editor_;
    QVector<SelectionOverlay *> overlays_;
    QVector<QMetaObject::Connection> screenConnections_;
    State state_ = State::Idle;
    QTimer timer_;
    QElapsedTimer scrollTime_;
    Stitcher stitch_;
    QRect rect_;
    QRect outputRect_;
    QPoint wheelPoint_;
    DWORD targetProcess_ = 0;
    QImage base_;
    RECT targetRect_ = {};
    HWND target_ = nullptr;
    QImage candidate_;
    bool hotkey_ = false;
    bool finishHotkey_ = false;
    int stable_ = 0;
    int unchanged_ = 0;
    int scrollTicks_ = 1;
    int generation_ = 0;
};
