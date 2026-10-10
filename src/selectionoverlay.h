#pragma once
#include "windowscapture.h"
#include <QWidget>
#include <functional>
#include <memory>

class QScreen;
class QLabel;
class QPushButton;
class QComboBox;
struct RegionProbe;
struct HoverProbe;
class Editor;

struct CaptureSelection {
    QRect output;
    QRect region;
    QPoint wheel;
    HWND window = nullptr;
    DWORD process = 0;
    bool edit = false;
    QImage image;
};

class SelectionOverlay : public QWidget
{
public:
    enum class Phase { Outer, Windows, Region, Capturing };
    SelectionOverlay(QScreen *screen, std::function<void(SelectionOverlay *, CaptureSelection)> begin,
                     std::function<void()> stop, std::function<void()> cancel,
                     std::function<void(Phase)> changed,
                     std::function<QRect(HWND, QPoint)> detector = WindowsCapture::scrollRegion);
    ~SelectionOverlay() override;
    bool valid() const { return !screenRect_.isEmpty() && !samplingScreen_.isNull(); }
    void capturing(bool selected = true);
    void completionKey(bool available);
    void recover(const QString &reason, bool ordinary = false);
protected:
    void hideEvent(QHideEvent *event) override;
    void showEvent(QShowEvent *event) override;
    bool nativeEvent(const QByteArray &, void *message, long *result) override;
    void paintEvent(QPaintEvent *) override;
    void wheelEvent(QWheelEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
private:
    friend class CoreTests;
    QImage selectionImage() const;
    void ensureAnnotations();
    QPoint imagePoint(QPoint point) const;
    Editor *annotations_ = nullptr;
    QWidget *annotationBar_ = nullptr;
    int annotationTool_ = 0;
    bool annotating_ = false;
    QPoint annotationStart_, annotationEnd_;
    QRect magnifierRect(QPoint point) const;
    void updateHover();
    std::function<QVector<QRect>(HWND, QPoint, bool)> hoverDetector_ = WindowsCapture::elementRegions;
    bool automatic_ = true, confirmed_ = false, tabKey_ = false;
    QPoint hoverPoint_ = QPoint(-100, -100);
    bool hoverManualLock_ = false;
    bool hoverPublished_ = false;
    QVector<QPair<QPoint, QVector<QRect>>> hoverCache_;
    int hoverLevel_ = 0;
    std::shared_ptr<HoverProbe> hoverJobs_[3];
    QVector<QRect> hoverRegions_;
    QVector<WindowsCapture::WindowCandidate> hoverWindows_;
    QRect physical(QRect rect) const;
    QRect logical(QRect rect) const;
    void positionPanel();
    void discover(bool preserve = false);
    struct Hit { bool valid = false; bool outer = false; int edges = 0; };
    Hit hitTest(QPoint point) const;
    static int borderHit(QRect bounds, QPoint point);
    void updateCursor(QPoint point);
    void choose(int index);
    void probe(QPoint point);
    void pollProbe(int token, int attempts);
    void refresh();
    void phase(Phase next);
    QRect limit() const;
    QImage samplingScreen_;
    QPoint samplePoint_;
    bool hexColor_ = false, shiftDown_ = false, colorKey_ = false;
    QString colorText() const;
    void releaseColorKey();
    QRect screenRect_, outer_, region_, before_;
    QPoint origin_, wheel_;
    Phase phase_ = Phase::Outer;
    bool dragging_ = false;
    bool dragOuter_ = false;
    bool captureExcluded_ = false;
    int edges_ = 0;
    int targetIndex_ = -1;
    int revision_ = 0;
    std::shared_ptr<RegionProbe> probe_;
    QVector<WindowsCapture::WindowCandidate> candidates_;
    QWidget *panel_;
    QLabel *label_;
    QPushButton *snapshot_, *scrollMode_, *start_, *windowsButton_, *manualButton_, *finish_;
    QComboBox *windows_;
    std::function<void(SelectionOverlay *, CaptureSelection)> begin_;
    std::function<void()> stop_, cancel_;
    std::function<void(Phase)> changed_;
    std::function<QRect(HWND, QPoint)> detector_;
};
