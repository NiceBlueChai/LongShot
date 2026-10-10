#include "selectionoverlay.h"
#include "editor.h"
#include "toolbaricons.h"
#include "toolbartooltips.h"
#include "ocr.h"
#include <QButtonGroup>
#include <QFileDialog>
#include <QFrame>
#include <QSaveFile>
#include <QApplication>
#include <QClipboard>
#include <QCursor>
#include <QComboBox>
#include <QLabel>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QPainter>
#include <QPushButton>
#include <QScreen>
#include <QTimer>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QStyle>
#include <atomic>
#include <thread>
#include <algorithm>

struct RegionProbe {
    std::atomic<bool> ready { false };
    QRect result;
};
struct HoverProbe {
    HWND window = nullptr;
    QPoint point;
    std::atomic<bool> ready { false };
    QVector<QRect> regions;
};
namespace {
std::atomic<bool> hoverBusy[3] { false, false, false };
std::atomic<bool> probeBusy { false };
}

SelectionOverlay::SelectionOverlay(QScreen *screen,
    std::function<void(SelectionOverlay *, CaptureSelection)> begin, std::function<void()> stop,
    std::function<void()> cancel, std::function<void(Phase)> changed,
    std::function<QRect(HWND, QPoint)> detector)
    : QWidget(nullptr, Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool
                      | Qt::WindowDoesNotAcceptFocus), screenRect_(WindowsCapture::screenRect(screen->name())),
      begin_(std::move(begin)), stop_(std::move(stop)), cancel_(std::move(cancel)), changed_(std::move(changed)), detector_(std::move(detector))
{
    hoverWindows_ = WindowsCapture::candidates(screenRect_);
    samplingScreen_ = WindowsCapture::grab(screenRect_, nullptr);
    auto sampler = new QTimer(this);
    connect(sampler, &QTimer::timeout, this, [this] {
        if (!isVisible() || phase_ == Phase::Capturing) return;
        const QPoint previousPoint = samplePoint_;
        const QRect previousSelection = outer_;
        const bool previousColorKey = colorKey_;
        const bool previousFormat = hexColor_;
        POINT cursor = {};
        GetCursorPos(&cursor);
        samplePoint_ = logical(QRect(cursor.x, cursor.y, 1, 1)).topLeft();
        if (rect().contains(samplePoint_) && !QApplication::activeModalWidget()) {
            const HWND hit = GetAncestor(WindowFromPoint(cursor), GA_ROOT);
            wchar_t type[128] = {};
            GetClassNameW(hit, type, 128);
            if (wcscmp(type, L"Shell_TrayWnd") == 0 || wcscmp(type, L"Shell_SecondaryTrayWnd") == 0) {
                // Explorer can raise the taskbar after the non-activating overlay is shown.
                SetWindowPos(reinterpret_cast<HWND>(winId()), HWND_TOPMOST, 0, 0, 0, 0,
                             SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            }
        }
        if (confirmed_ || QApplication::activeModalWidget() || !rect().contains(samplePoint_)) releaseColorKey();
        else if (!colorKey_)
            colorKey_ = RegisterHotKey(reinterpret_cast<HWND>(winId()), 0x4c58, MOD_NOREPEAT, 'C') != FALSE;
        updateHover();
        const bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
        if (shift && !shiftDown_) hexColor_ = !hexColor_;
        shiftDown_ = shift;
        if (previousSelection != outer_) update();
        else if (previousPoint != samplePoint_ || previousColorKey != colorKey_ || previousFormat != hexColor_)
            update(QRegion(magnifierRect(previousPoint)) | QRegion(magnifierRect(samplePoint_)));
    });
    sampler->setTimerType(Qt::PreciseTimer);
    sampler->start(16);
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_ShowWithoutActivating);
    setAttribute(Qt::WA_AlwaysShowToolTips);
    setMouseTracking(true);
    setCursor(Qt::CrossCursor);
    setGeometry(screen->geometry());
    panel_ = new QWidget(this);
    panel_->setObjectName("captureToolbar");
    panel_->setCursor(Qt::ArrowCursor);
    auto layout = new QVBoxLayout(panel_);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);
    panel_->setStyleSheet("QWidget { background: transparent; color: #e5eaf2; }"
                         "QWidget#captureActions { background: #202938; border: 1px solid #42516a; border-radius: 8px; }"
                         "QPushButton { background: transparent; border: none; padding: 0; border-radius: 5px; }"
                         "QPushButton:hover { background: #36465c; }"
                         "QPushButton:pressed, QPushButton:checked { background: #2563eb; color: white; }"
                         "QPushButton:disabled { background: transparent; color: #aab0bb; }"
                         "QFrame#toolbarSeparator { background: #91a0b5; border: none; border-radius: 0; }"
                         "QComboBox { background: #202938; border: 1px solid #42516a; border-radius: 5px; padding: 5px; }"
                         "QComboBox QAbstractItemView { background: #202938; selection-background-color: #2563eb; }"
                         "QToolTip { background: #202530; color: white; border: 1px solid #64748b; }");
    auto actionBar = new QWidget(panel_);
    actionBar->setObjectName("captureActions");
    auto actions = new QHBoxLayout(actionBar);
    actions->setContentsMargins(8, 8, 8, 8);
    actions->setSpacing(6);
    layout->addWidget(actionBar, 0, Qt::AlignHCenter);
    auto separator = [](QHBoxLayout *row, const QString &name = QString()) {
        auto line = new QFrame;
        line->setObjectName("toolbarSeparator");
        line->setProperty("group", name);
        line->setFixedSize(1, 24);
        row->addWidget(line);
        return line;
    };
    label_ = new QLabel(QStringLiteral("拖动框选完整截图范围 · Esc 取消"), panel_);
    label_->setWordWrap(true);
    label_->setMinimumWidth(qMin(360, qMax(160, width() - 32)));
    label_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    windows_ = new QComboBox(panel_);
    windows_->setObjectName("targetWindows");
    windows_->setFocusPolicy(Qt::NoFocus);
    windows_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    windows_->setMinimumContentsLength(12);
    layout->addWidget(windows_);
    layout->addWidget(label_);
    connect(windows_, QOverload<int>::of(&QComboBox::activated), this, &SelectionOverlay::choose);
    auto button = [&](const QString &text) {
        auto result = new QPushButton(text, actionBar);
        result->setFocusPolicy(Qt::NoFocus);
        actions->addWidget(result);
        return result;
    };
    snapshot_ = button(QStringLiteral("完成 · 复制到剪贴板"));
    snapshot_->setObjectName("captureStill");
    annotationBar_ = new QWidget(actionBar);
    auto annotationLayout = new QHBoxLayout(annotationBar_);
    annotationLayout->setContentsMargins(0, 0, 0, 0);
    annotationLayout->setSpacing(6);
    actions->insertWidget(0, annotationBar_);
    auto group = new QButtonGroup(this);
    const QStringList names = { QStringLiteral("选区"), QStringLiteral("矩形"), QStringLiteral("箭头"),
                               QStringLiteral("文字"), QStringLiteral("马赛克") };
    const int tools[] = { 0, 2, 3, 4, 5 };
    for (int i = 0; i < names.size(); ++i) {
        auto tool = new QPushButton(names[i], annotationBar_);
        tool->setObjectName(QString("annotationTool%1").arg(tools[i]));
        tool->setCheckable(true);
        tool->setChecked(i == 0);
        tool->setFocusPolicy(Qt::NoFocus);
        tool->setToolTip(i ? names[i] + QStringLiteral("：在选区内拖动绘制")
                          : QStringLiteral("拖动框内移动选区，拖动边缘调整范围"));
        group->addButton(tool, tools[i]);
        annotationLayout->addWidget(tool);
        if (i == 0) separator(annotationLayout);
    }
    connect(group, QOverload<int>::of(&QButtonGroup::buttonClicked), this, [this](int tool) {
        annotationTool_ = tool;
        if (tool) ensureAnnotations();
        updateCursor(samplePoint_);
        update();
    });
    auto ocr = new QPushButton(QStringLiteral("识别文字"), annotationBar_);
    annotationLayout->addWidget(ocr);
    connect(ocr, &QPushButton::clicked, this, [this] { Ocr::showDialog(this, selectionImage()); });
    separator(annotationLayout);
    auto undo = new QPushButton(QStringLiteral("撤销"), annotationBar_);
    undo->setEnabled(false);
    annotationLayout->addWidget(undo);
    auto redo = new QPushButton(QStringLiteral("重做"), annotationBar_);
    redo->setEnabled(false);
    annotationLayout->addWidget(redo);
    undo->setObjectName("annotationUndo");
    redo->setObjectName("annotationRedo");
    connect(undo, &QPushButton::clicked, this, [this] { if (annotations_) annotations_->undoStack()->undo(); });
    connect(redo, &QPushButton::clicked, this, [this] { if (annotations_) annotations_->undoStack()->redo(); });
    separator(annotationLayout);
    auto save = new QPushButton(QStringLiteral("保存"), annotationBar_);
    annotationLayout->addWidget(save);
    connect(save, &QPushButton::clicked, this, [this] {
        const auto image = selectionImage();
        if (image.isNull()) { label_->setText(QStringLiteral("无法生成图片，请缩小选区重试")); return; }
        QString path = QFileDialog::getSaveFileName(this, QStringLiteral("保存截图"), "LongShot.png", "PNG (*.png)");
        if (path.isEmpty()) return;
        if (!path.endsWith(".png", Qt::CaseInsensitive)) path += ".png";
        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly) || !image.save(&file, "PNG") || !file.commit())
            label_->setText(QStringLiteral("保存失败：") + file.errorString());
        else label_->setText(QStringLiteral("已保存，可继续标注或点击完成复制"));
    });
    separator(actions, "scrollSeparator");
    scrollMode_ = button(QStringLiteral("长截图"));
    scrollMode_->setObjectName("enterScroll");
    connect(snapshot_, &QPushButton::clicked, this, [this] {
        if (phase_ != Phase::Outer || outer_.isEmpty()) return;
        const auto image = selectionImage();
        if (image.isNull()) { label_->setText(QStringLiteral("无法生成截图，请缩小选区重试")); return; }
        begin_(this, { physical(outer_), {}, {}, nullptr, 0, false, image });
    });
    connect(scrollMode_, &QPushButton::clicked, this, &SelectionOverlay::discover);
    start_ = button(QStringLiteral("开始长截图"));
    start_->setObjectName("startCapture");
    windowsButton_ = button(QStringLiteral("重新选择窗口"));
    finish_ = button(QStringLiteral("完成"));
    auto decorate = [&](QPushButton *control, const QString &tip, ToolbarIcons::Kind icon) {
        control->setAccessibleName(control->text());
        control->setText({});
        control->setToolTip(tip);
        control->setIcon(ToolbarIcons::icon(icon));
        control->setIconSize(QSize(20, 20));
        control->setFixedSize(40, 36);
        control->setStyleSheet("padding: 0;");
    };
    const ToolbarIcons::Kind annotationIcons[] = { ToolbarIcons::Move, ToolbarIcons::Rectangle,
        ToolbarIcons::Arrow, ToolbarIcons::Text, ToolbarIcons::Mosaic };
    for (int i = 0; i < 5; ++i) {
        auto control = qobject_cast<QPushButton *>(group->button(tools[i]));
        decorate(control, (i ? control->toolTip() : names[i] + QStringLiteral("：") + control->toolTip()), annotationIcons[i]);
    }
    decorate(ocr, QStringLiteral("识别文字（离线 OCR）"), ToolbarIcons::Ocr);
    decorate(undo, QStringLiteral("撤销"), ToolbarIcons::Undo);
    decorate(redo, QStringLiteral("重做"), ToolbarIcons::Redo);
    decorate(save, QStringLiteral("保存 PNG"), ToolbarIcons::Save);
    decorate(snapshot_, QStringLiteral("完成：复制截图并收起"), ToolbarIcons::CompleteCopy);
    decorate(scrollMode_, QStringLiteral("长截图：选择滚动区域"), ToolbarIcons::Scroll);
    decorate(start_, QStringLiteral("开始自动滚动采集"), ToolbarIcons::Play);
    decorate(windowsButton_, QStringLiteral("重新选择滚动目标窗口"), ToolbarIcons::Window);
    decorate(finish_, QStringLiteral("完成长截图（Q）"), ToolbarIcons::Finish);
    separator(actions, "cancelSeparator");
    auto cancelButton = button(QStringLiteral("取消"));
    decorate(cancelButton, QStringLiteral("取消截图（Esc）"), ToolbarIcons::Cancel);
    connect(cancelButton, &QPushButton::clicked, this, [this] { cancel_(); });
    connect(finish_, &QPushButton::clicked, this, [this] { stop_(); });
    connect(windowsButton_, &QPushButton::clicked, this, &SelectionOverlay::discover);
    connect(start_, &QPushButton::clicked, this, [this] {
        if (targetIndex_ < 0 || region_.isEmpty()) return;
        const auto &target = candidates_[targetIndex_];
        const QRect area = physical(region_);
        RECT current = {};
        const bool sameBounds = GetWindowRect(target.window, &current)
            && QRect(current.left, current.top, current.right - current.left, current.bottom - current.top) == target.bounds;
        if (!sameBounds || !WindowsCapture::validTarget(target.window, target.process, area)) {
            recover(QStringLiteral("目标关闭、移动或区域失效，请重新选择窗口"));
            return;
        }
        const QPoint point = physical(QRect(wheel_, QSize(1, 1))).topLeft();
        begin_(this, { physical(outer_), area, point, target.window, target.process });
    });
    new ToolbarTooltips(panel_);
    refresh();
}

QRect SelectionOverlay::physical(QRect rect) const { return WindowsCapture::toPhysical(rect, size(), screenRect_); }
QRect SelectionOverlay::logical(QRect rect) const { return WindowsCapture::toLogical(rect, size(), screenRect_); }
void SelectionOverlay::phase(Phase next) { phase_ = next; changed_(next); }
QRect SelectionOverlay::limit() const
{
    return targetIndex_ < 0 ? outer_ : outer_.intersected(logical(candidates_[targetIndex_].client));
}
void SelectionOverlay::refresh()
{
    const bool region = phase_ == Phase::Region;
    snapshot_->setVisible(!outer_.isEmpty() && phase_ == Phase::Outer);
    annotationBar_->setVisible(!outer_.isEmpty() && phase_ == Phase::Outer);
    scrollMode_->setVisible(!outer_.isEmpty() && phase_ == Phase::Outer);
    scrollMode_->setEnabled(physical(outer_).width() >= 32 && physical(outer_).height() >= 80);
    windows_->setVisible(candidates_.size() > 1 && (phase_ == Phase::Windows || region));
    windowsButton_->setVisible(region && candidates_.size() > 1);
    start_->setVisible(region);
    start_->setEnabled(region_.width() >= 32 && region_.height() >= 80);
    finish_->setVisible(phase_ == Phase::Capturing && !region_.isEmpty());
    for (auto button : panel_->findChildren<QPushButton *>()) {
        button->ensurePolished();
        button->setMinimumWidth(40);
        button->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);
    }
    for (auto line : panel_->findChildren<QFrame *>()) {
        const auto group = line->property("group").toString();
        if (group == "scrollSeparator") line->setVisible(!scrollMode_->isHidden());
        else if (group == "cancelSeparator")
            line->setVisible(!snapshot_->isHidden() || !start_->isHidden() || !finish_->isHidden());
    }
    annotationBar_->layout()->invalidate();
    annotationBar_->layout()->activate();
    auto actionBar = panel_->findChild<QWidget *>("captureActions");
    actionBar->layout()->invalidate();
    actionBar->layout()->activate();
    panel_->layout()->invalidate();
    panel_->layout()->activate();
    panel_->resize(panel_->layout()->sizeHint().expandedTo(panel_->layout()->minimumSize()));
    panel_->layout()->setGeometry(panel_->rect());
    actionBar->layout()->setGeometry(actionBar->rect());
    annotationBar_->layout()->setGeometry(annotationBar_->rect());
    panel_->show();
    positionPanel();
    if (phase_ == Phase::Outer && !confirmed_) panel_->hide();
    update();
}
void SelectionOverlay::positionPanel()
{
    POINT cursor = {};
    GetCursorPos(&cursor);
    MONITORINFO monitor = {};
    monitor.cbSize = sizeof(monitor);
    QRect available = rect();
    if (GetMonitorInfoW(MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST), &monitor)) {
        const QRect hoveredScreen = logical(QRect(monitor.rcMonitor.left, monitor.rcMonitor.top,
            monitor.rcMonitor.right - monitor.rcMonitor.left, monitor.rcMonitor.bottom - monitor.rcMonitor.top))
            .intersected(rect());
        if (!hoveredScreen.isEmpty()) available = hoveredScreen;
    }
    if (outer_.isEmpty()) panel_->move(available.topLeft() + QPoint(24, 24));
    else {
        const int maxX = qMax(available.left(), available.right() + 1 - panel_->width());
        const int maxY = qMax(available.top(), available.bottom() + 1 - panel_->height());
        const int x = qBound(available.left(), outer_.center().x() - panel_->width() / 2, maxX);
        const int y = qBound(available.top(), outer_.top(), maxY);
        const QPoint choices[] = {
            { x, outer_.bottom() + 12 }, { x, outer_.top() - panel_->height() - 12 },
            { outer_.right() + 12, y }, { outer_.left() - panel_->width() - 12, y },
            outer_.topLeft() + QPoint(12, 12), available.topLeft(), { maxX, available.top() }, { available.left(), maxY }, { maxX, maxY }
        };
        const QRegion border = QRegion(outer_.adjusted(-7, -7, 7, 7))
            - QRegion(outer_.adjusted(8, 8, -8, -8));
        panel_->move(x, qBound(available.top(), outer_.bottom() + 12, maxY));
        for (auto point : choices) {
            const QRect box(point, panel_->size());
            if (available.contains(box) && (QRegion(box) & border).isEmpty()) {
                panel_->move(point); break;
            }
        }
    }
    if (phase_ == Phase::Capturing) {
        const QRect forbidden = region_.adjusted(-3, -3, 3, 3);
        if (panel_->geometry().intersects(forbidden)) {
            const QRect choices[] = {
                QRect(10, 10, width() - 20, forbidden.top() - 20),
                QRect(10, forbidden.bottom() + 10, width() - 20, height() - forbidden.bottom() - 20),
                QRect(10, 10, forbidden.left() - 20, height() - 20),
                QRect(forbidden.right() + 10, 10, width() - forbidden.right() - 20, height() - 20)
            };
            bool placed = false;
            for (const auto &space : choices) {
                if (space.width() >= panel_->width() && space.height() >= panel_->height()) {
                    panel_->move(space.topLeft()); placed = true; break;
                }
            }
            panel_->setVisible(placed || captureExcluded_);
        }
        if (panel_->isVisible() && panel_->geometry().contains(wheel_)) {
            for (auto corner : { QPoint(10, 10), QPoint(width() - panel_->width() - 10, 10),
                                 QPoint(10, height() - panel_->height() - 10),
                                 QPoint(width() - panel_->width() - 10, height() - panel_->height() - 10) }) {
                if (!QRect(corner, panel_->size()).contains(wheel_)) { panel_->move(corner); break; }
            }
        }
        setMask((QRegion(rect()) - QRegion(region_)) | (panel_->isVisible() ? QRegion(panel_->geometry()) : QRegion()));
    }
}
void SelectionOverlay::discover(bool preserve)
{
    if (annotations_ && annotations_->undoStack()->count() && !preserve) {
        label_->setText(QStringLiteral("已有标注，请先完成本次截图；长截图应在标注前开始。"));
        return;
    }
    annotationTool_ = 0;
    const HWND previousWindow = preserve && targetIndex_ >= 0 ? candidates_[targetIndex_].window : nullptr;
    const DWORD previousProcess = preserve && targetIndex_ >= 0 ? candidates_[targetIndex_].process : 0;
    const QRect previousRegion = region_;
    const QPoint previousWheel = wheel_;
    ++revision_;
    region_ = {};
    targetIndex_ = -1;
    candidates_ = WindowsCapture::candidates(physical(outer_));
    windows_->clear();
    for (int i = 0; i < candidates_.size(); ++i)
        windows_->addItem(candidates_[i].icon, QString::number(i + 1) + ". " + candidates_[i].title);
    windows_->setCurrentIndex(-1);
    phase(Phase::Windows);
    label_->setText(candidates_.isEmpty() ? QStringLiteral("没有找到相交窗口，请调整外框")
                                         : QStringLiteral("点击高亮窗口或从列表选择滚动目标"));
    int retained = -1;
    for (int i = 0; i < candidates_.size(); ++i)
        if (candidates_[i].window == previousWindow && candidates_[i].process == previousProcess) retained = i;
    if (retained >= 0) {
        choose(retained);
        region_ = previousRegion.intersected(limit());
        wheel_ = region_.contains(previousWheel) ? previousWheel : region_.center();
        if (region_.width() < 32 || region_.height() < 80)
            label_->setText(QStringLiteral("滚动区域过小，请调整绿色边框或重新框选滚动区域"));
    } else if (candidates_.size() == 1) choose(0);
    refresh();
}
void SelectionOverlay::choose(int index)
{
    if (index < 0 || index >= candidates_.size()) return;
    ++revision_;
    targetIndex_ = index;
    windows_->setCurrentIndex(index);
    region_ = limit();
    wheel_ = region_.center();
    phase(Phase::Region);
    label_->setText(QStringLiteral("已选好滚动范围，点击开始即可；仅截某个面板时可调整滚动区域。"));
    refresh();
}
void SelectionOverlay::probe(QPoint point)
{
    ++revision_;
    wheel_ = point;
    region_ = {};
    if (targetIndex_ < 0 || !limit().contains(point)) return;
    if (probeBusy.exchange(true)) {
        label_->setText(QStringLiteral("自动识别暂不可用，请直接拖动框选滚动区域"));
        refresh(); return;
    }
    probe_ = std::make_shared<RegionProbe>();
    const auto request = probe_;
    const auto window = candidates_[targetIndex_].window;
    const auto position = physical(QRect(point, QSize(1, 1))).topLeft();
    // Only one provider call may run; a hung provider must not create unbounded threads.
    const auto detector = detector_;
    std::thread([request, window, position, detector] {
        try { request->result = detector(window, position); } catch (...) { request->result = {}; }
        request->ready.store(true);
        probeBusy.store(false);
    }).detach();
    label_->setText(QStringLiteral("正在识别滚动区域…也可直接拖动框选"));
    refresh();
    pollProbe(revision_, 0);
}
void SelectionOverlay::pollProbe(int token, int attempts)
{
    if (token != revision_ || phase_ != Phase::Region) return;
    if (probe_->ready.load()) {
        region_ = logical(probe_->result).intersected(limit());
        if (!region_.contains(wheel_) || region_.width() < 32 || region_.height() < 80) region_ = {};
        label_->setText(region_.isEmpty() ? QStringLiteral("未识别到滚动区域，请拖动框选")
            : QStringLiteral("确认绿色滚动区域；可拖动移动、调整边缘，然后开始"));
        refresh();
    } else if (attempts >= 16) {
        ++revision_;
        label_->setText(QStringLiteral("识别超时，请拖动框选滚动区域"));
        refresh();
    } else QTimer::singleShot(50, this, [this, token, attempts] { pollProbe(token, attempts + 1); });
}
void SelectionOverlay::recover(const QString &reason, bool ordinary)
{
    clearMask();
    if (samplingScreen_.isNull()) samplingScreen_ = WindowsCapture::grab(screenRect_, nullptr);
    if (ordinary) { ++revision_; region_ = {}; phase(Phase::Outer); }
    else discover();
    label_->setText(reason);
    refresh();
    show();
}
void SelectionOverlay::capturing(bool selected)
{
    ++revision_;
    if (!selected) { region_ = {}; outer_ = {}; }
    releaseColorKey();
    samplingScreen_ = {};
    phase(Phase::Capturing);
    setCursor(Qt::ArrowCursor);
    label_->setText(QStringLiteral("准备采集 · 点击外框外或完成结束 · Esc 取消"));
    refresh();
    positionPanel();
}
void SelectionOverlay::completionKey(bool available)
{
    label_->setText(available
        ? QStringLiteral("Q 完成并编辑 · 移出绿色区域暂停 · 点击外框外或完成结束 · Esc 取消")
        : QStringLiteral("Q 注册失败，请点击外框外或完成结束 · 移出绿色区域暂停 · Esc 取消"));
    finish_->setToolTip(available ? QStringLiteral("完成长截图（Q）") : QStringLiteral("完成长截图（Q 不可用）"));
    refresh();
}
SelectionOverlay::~SelectionOverlay() { releaseColorKey(); }
void SelectionOverlay::releaseColorKey()
{
    if (tabKey_) UnregisterHotKey(reinterpret_cast<HWND>(winId()), 0x4c59);
    tabKey_ = false;
    if (colorKey_) UnregisterHotKey(reinterpret_cast<HWND>(winId()), 0x4c58);
    colorKey_ = false;
}
void SelectionOverlay::hideEvent(QHideEvent *event)
{
    releaseColorKey();
    QWidget::hideEvent(event);
}
QString SelectionOverlay::colorText() const
{
    const QPoint pixel = physical(QRect(samplePoint_, QSize(1, 1))).topLeft() - screenRect_.topLeft();
    if (!samplingScreen_.rect().contains(pixel)) return {};
    const QColor color = samplingScreen_.pixelColor(pixel);
    return hexColor_ ? color.name().toUpper()
        : QString("%1, %2, %3").arg(color.red()).arg(color.green()).arg(color.blue());
}
void SelectionOverlay::showEvent(QShowEvent *event)
{
    const auto window = reinterpret_cast<HWND>(winId());
    SetWindowLongPtrW(window, GWL_EXSTYLE, GetWindowLongPtrW(window, GWL_EXSTYLE) | WS_EX_NOACTIVATE);
    captureExcluded_ = SetWindowDisplayAffinity(window, 0x00000011) != FALSE;
    QWidget::showEvent(event);
}
bool SelectionOverlay::nativeEvent(const QByteArray &, void *message, long *result)
{
    auto event = static_cast<MSG *>(message);
    if (event->message == WM_HOTKEY && event->wParam == 0x4c59 && phase_ == Phase::Outer && !dragging_ && !confirmed_) {
        automatic_ = !automatic_;
        confirmed_ = false;
        outer_ = {};
        for (auto &job : hoverJobs_) job.reset();
        hoverManualLock_ = false;
        hoverPoint_ = QPoint(-100, -100);
        label_->setText(automatic_ ? QStringLiteral("自动识别：单击确认，滚轮切换层级；Tab 手工框选")
                                  : QStringLiteral("手工框选：拖动选择区域；Tab 自动识别"));
        refresh();
        if (result) *result = 0;
        return true;
    }
    if (event->message == WM_HOTKEY && event->wParam == 0x4c58 && phase_ != Phase::Capturing) {
        const QString text = colorText();
        if (!text.isEmpty()) QApplication::clipboard()->setText(text);
        if (result) *result = 0;
        return true;
    }
    if (event->message == WM_MOUSEACTIVATE) { if (result) *result = MA_NOACTIVATE; return true; }
    return false;
}
QRect SelectionOverlay::magnifierRect(QPoint point) const
{
    if (confirmed_ || phase_ == Phase::Capturing || !rect().contains(point)) return {};
    const QSize box(180, 218);
    int x = point.x() + 22, y = point.y() + 22;
    if (x + box.width() > width()) x = point.x() - box.width() - 22;
    if (y + box.height() > height()) y = point.y() - box.height() - 22;
    return QRect(QPoint(qBound(0, x, qMax(0, width() - box.width())),
                        qBound(0, y, qMax(0, height() - box.height()))), box);
}

void SelectionOverlay::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    if (phase_ == Phase::Outer) painter.drawImage(rect(), samplingScreen_);
    painter.fillRect(rect(), QColor(0, 0, 0, 140));
    if (phase_ == Phase::Outer && !outer_.isEmpty()) {
        const QRect source = physical(outer_).translated(-screenRect_.topLeft());
        if (annotations_) annotations_->paintImage(painter, outer_, source);
        else painter.drawImage(outer_, samplingScreen_, source);
        if (annotating_) {
            painter.save();
            painter.setRenderHint(QPainter::Antialiasing);
            painter.setClipRect(outer_);
            painter.setPen(QPen(QColor("#ef4444"), 2, Qt::DashLine));
            if (annotationTool_ == int(EditorView::Tool::Arrow)) painter.drawLine(annotationStart_, annotationEnd_);
            else painter.drawRect(QRect(annotationStart_, annotationEnd_).normalized());
            painter.restore();
        }
    } else {
        painter.setCompositionMode(QPainter::CompositionMode_Clear);
        painter.fillRect(outer_, Qt::transparent);
        painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
        painter.fillRect(outer_, QColor(0, 0, 0, 1));
    }
    painter.setPen(QPen(QColor("#2685ff"), 2));
    painter.drawRect(outer_);
    if (phase_ == Phase::Windows) {
        for (int i = 0; i < candidates_.size(); ++i) {
            const auto bounds = logical(candidates_[i].bounds).intersected(outer_);
            painter.fillRect(bounds, QColor(37, 99, 235, 30));
            painter.drawRect(bounds);
            painter.drawText(bounds.adjusted(8, 6, -8, -6), Qt::AlignTop | Qt::AlignLeft,
                             QString::number(i + 1) + ". " + candidates_[i].title);
        }
    }
    painter.setPen(QPen(QColor("#34d399"), 2));
    if (!region_.isEmpty()) {
        painter.drawRect(region_);
        painter.drawEllipse(wheel_, 4, 4);
    }
    auto drawHandles = [&](QRect handles, QColor color) {
        if (handles.isEmpty()) return;
        for (auto point : { handles.topLeft(), handles.topRight(), handles.bottomLeft(), handles.bottomRight(),
                            QPoint(handles.center().x(), handles.top()), QPoint(handles.center().x(), handles.bottom()),
                            QPoint(handles.left(), handles.center().y()), QPoint(handles.right(), handles.center().y()) })
        {
            painter.setRenderHint(QPainter::Antialiasing);
            painter.setPen(QPen(Qt::white, 1));
            painter.setBrush(color);
            painter.drawEllipse(point, 4, 4);
            painter.setBrush(Qt::NoBrush);
            painter.setRenderHint(QPainter::Antialiasing, false);
        }
    };
    if (phase_ != Phase::Capturing) {
        drawHandles(region_, QColor("#34d399"));
        drawHandles(outer_, QColor("#2685ff"));
        if (!outer_.isEmpty()) {
            const auto pixels = physical(outer_).size();
            const QString text = QString("%1 × %2 px").arg(pixels.width()).arg(pixels.height());
            const QSize badge(fontMetrics().horizontalAdvance(text) + 12, fontMetrics().height() + 8);
            const QPoint pos(qBound(0, outer_.left(), qMax(0, width() - badge.width())),
                outer_.top() >= badge.height() + 10 ? outer_.top() - badge.height() - 10 : outer_.top() + 10);
            painter.fillRect(QRect(pos, badge), QColor(25, 30, 40, 220));
            painter.setPen(Qt::white);
            painter.drawText(QRect(pos, badge), Qt::AlignCenter, text);
        }
    }
    if (phase_ == Phase::Outer && !confirmed_) {
        const QString hint = automatic_
            ? QStringLiteral("单击确认 · 拖动框选 · 滚轮切换父/子元素 · Tab 手工模式 · Esc 取消")
            : QStringLiteral("拖动框选 · Tab 自动识别 · Esc 取消");
        const int hintWidth = qMin(width(), fontMetrics().horizontalAdvance(hint) + 24);
        const QRect hintBox((width() - hintWidth) / 2, height() - 40, hintWidth, 28);
        painter.fillRect(hintBox, QColor(28, 28, 28, 230));
        painter.setPen(Qt::white);
        painter.drawText(hintBox, Qt::AlignCenter, hint);
    }
    if (phase_ != Phase::Capturing && !confirmed_ && rect().contains(samplePoint_) && !samplingScreen_.isNull()
        && !panel_->geometry().contains(samplePoint_)) {
        const QPoint pixel = physical(QRect(samplePoint_, QSize(1, 1))).topLeft() - screenRect_.topLeft();
        if (!samplingScreen_.rect().contains(pixel)) return;
        const int boxWidth = 180, boxHeight = 218;
        painter.save();
        painter.translate(magnifierRect(samplePoint_).topLeft());
        painter.fillRect(QRect(0, 0, boxWidth, boxHeight), QColor(28, 28, 28));
        painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
        const QRect preview(1, 1, 178, 120);
        painter.setClipRect(preview);
        // Keep the sampled source pixel centered even at monitor edges.
        painter.drawImage(QRect(4, 5, 170, 110), samplingScreen_, QRect(pixel - QPoint(8, 5), QSize(17, 11)));
        painter.setClipping(false);
        painter.setPen(QPen(QColor(38, 133, 255, 130), 1));
        painter.drawLine(89, 1, 89, 120); painter.drawLine(1, 60, 178, 60);
        painter.setPen(Qt::white); painter.drawRect(QRect(0, 0, 179, 121));
        painter.setPen(Qt::black); painter.drawRect(QRect(84, 55, 10, 10));
        painter.setPen(Qt::white);
        const QPoint global = pixel + screenRect_.topLeft();
        painter.drawText(QRect(0, 125, 180, 22), Qt::AlignCenter,
            QString("(%1, %2)").arg(global.x()).arg(global.y()));
        painter.fillRect(QRect(14, 154, 13, 13), samplingScreen_.pixelColor(pixel));
        painter.drawText(QRect(32, 148, 145, 24), Qt::AlignCenter, colorText());
        painter.drawText(QRect(0, 177, 180, 18), Qt::AlignCenter,
            colorKey_ ? QStringLiteral("按 C 复制颜色值") : QStringLiteral("C 快捷键被占用"));
        painter.drawText(QRect(0, 195, 180, 18), Qt::AlignCenter, QStringLiteral("按 Shift 切换 RGB/HEX"));
        painter.restore();
    }

}

void SelectionOverlay::updateHover()
{
    if (phase_ != Phase::Outer || confirmed_ || dragging_ || annotating_
        || QApplication::activeModalWidget() || !rect().contains(samplePoint_)) return;
    if (!tabKey_) tabKey_ = RegisterHotKey(reinterpret_cast<HWND>(winId()), 0x4c59, MOD_NOREPEAT, VK_TAB) != FALSE;
    if (!automatic_) return;
    if (hoverManualLock_ && samplePoint_ == hoverPoint_) return;
    if (samplePoint_ != hoverPoint_) hoverManualLock_ = false;
    const QPoint point = physical(QRect(samplePoint_, QSize(1, 1))).topLeft();
    HWND window = nullptr;
    QRect windowArea;
    for (const auto &candidate : hoverWindows_) {
        if (candidate.visible.contains(point) && IsWindow(candidate.window)) {
            window = candidate.window;
            windowArea = logical(candidate.bounds).intersected(rect());
            break;
        }
    }
    if (!window) { outer_ = {}; hoverRegions_.clear(); return; }
    const bool moved = samplePoint_ != hoverPoint_;
    if (moved) {
        hoverPoint_ = samplePoint_;
        hoverPublished_ = outer_ != windowArea && windowArea.contains(outer_) && outer_.contains(samplePoint_);
        hoverManualLock_ = false;
        hoverRegions_ = { windowArea };
        for (int i = hoverCache_.size() - 1; i >= 0; --i) {
            if (hoverCache_[i].first != samplePoint_ || hoverCache_[i].second.isEmpty()
                || hoverCache_[i].second.first() != windowArea) continue;
            hoverRegions_ = WindowsCapture::refineElementRegions(hoverCache_[i].second, windowArea,
                samplePoint_, hoverPublished_ ? outer_ : QRect());
            outer_ = hoverRegions_.last();
            hoverPublished_ = hoverRegions_.size() > 1;
            break;
        }
        if (outer_.isEmpty() || !windowArea.contains(outer_)) outer_ = windowArea;
    }
    QVector<QRect> options;
    bool currentResult = false;
    bool semanticCandidate = false;
    for (const int strategy : { 1, 0, 2 }) {
        const auto &job = hoverJobs_[strategy];
        if (!job || !job->ready.load() || job->window != window) continue;
        currentResult |= job->point == point;
        if (strategy == 2 && !options.isEmpty()) continue;
        // Reuse completed geometry that still contains the pointer while requesting its new location.
        for (const auto &physicalArea : job->regions) {
            const QRect area = logical(physicalArea).intersected(rect());
            if (area != windowArea && area.contains(samplePoint_) && windowArea.contains(area)) {
                options.append(area);
                if (strategy != 2) semanticCandidate = true;
            }
        }
    }
    hoverRegions_ = WindowsCapture::refineElementRegions(options, windowArea, samplePoint_,
        hoverPublished_ ? outer_ : QRect());
    hoverLevel_ = hoverRegions_.size() - 1;
    if (hoverRegions_.last() != windowArea || currentResult) {
        outer_ = hoverRegions_.last();
        hoverPublished_ = outer_ != windowArea;
    }
    if (currentResult && !hoverRegions_.isEmpty()
        && (hoverCache_.isEmpty() || hoverCache_.last().first != samplePoint_
            || hoverCache_.last().second.size() != hoverRegions_.size()
            || !std::equal(hoverRegions_.begin(), hoverRegions_.end(), hoverCache_.last().second.begin()))) {
        if (hoverCache_.size() >= 64) hoverCache_.removeFirst();
        hoverCache_.append(qMakePair(samplePoint_, hoverRegions_));
    }
    for (int strategy = 0; strategy < 3; ++strategy) {
        if (strategy == 2 && semanticCandidate) continue;
        const auto &previous = hoverJobs_[strategy];
        if (previous && (!previous->ready.load() || (previous->window == window && previous->point == point))) continue;
        // Each provider consumes the latest position independently; a slow provider cannot hold the others up.
        if (hoverBusy[strategy].exchange(true)) continue;
        const auto job = std::make_shared<HoverProbe>();
        job->window = window;
        job->point = point;
        hoverJobs_[strategy] = job;
        const QImage pixels = samplingScreen_;
        const QPoint screenOrigin = screenRect_.topLeft();
        const auto detector = hoverDetector_;
        try {
            std::thread([job, strategy, pixels, screenOrigin, detector] {
                try {
                    if (strategy == 2) {
                        job->regions = WindowsCapture::pixelRegions(pixels, job->point - screenOrigin);
                        for (auto &area : job->regions) area.translate(screenOrigin);
                    } else job->regions = detector(job->window, job->point, strategy == 1);
                } catch (...) {}
                job->ready.store(true);
                hoverBusy[strategy].store(false);
            }).detach();
        } catch (...) {
            hoverBusy[strategy].store(false);
            job->ready.store(true);
        }
    }
}
void SelectionOverlay::wheelEvent(QWheelEvent *event)
{
    if (phase_ == Phase::Outer && automatic_ && !confirmed_ && !dragging_ && !hoverRegions_.isEmpty()) {
        // Once the user chooses a level, a late provider result must not replace it.
        for (auto &job : hoverJobs_) job.reset();
        samplePoint_ = event->pos();
        hoverPoint_ = samplePoint_;
        hoverManualLock_ = true;
        if (event->angleDelta().y())
            hoverLevel_ = qBound(0, hoverLevel_ + (event->angleDelta().y() > 0 ? -1 : 1), hoverRegions_.size() - 1);
        outer_ = hoverRegions_[hoverLevel_];
        update();
    }
    event->accept();
}

int SelectionOverlay::borderHit(QRect bounds, QPoint point)
{
    if (bounds.isEmpty() || !bounds.adjusted(-7, -7, 7, 7).contains(point)) return 0;
    int edges = 0;
    const int left = qAbs(point.x() - bounds.left()), right = qAbs(point.x() - bounds.right());
    const int top = qAbs(point.y() - bounds.top()), bottom = qAbs(point.y() - bounds.bottom());
    if (qMin(left, right) <= 7) edges |= left <= right ? 1 : 2;
    if (qMin(top, bottom) <= 7) edges |= top <= bottom ? 4 : 8;
    return edges;
}
SelectionOverlay::Hit SelectionOverlay::hitTest(QPoint point) const
{
    if (phase_ == Phase::Capturing) return {};
    if (phase_ == Phase::Outer && !confirmed_) return { true, true, 0 };
    const int outerEdge = borderHit(outer_, point);
    if (outerEdge) return { true, true, outerEdge };
    if (phase_ == Phase::Outer) return { true, true, outer_.contains(point) ? 16 : 0 };
    if (phase_ == Phase::Windows) return {};
    const int innerEdge = borderHit(region_, point);
    if (innerEdge) return { true, false, innerEdge };
    if (region_.contains(point)) return { true, false, 16 };
    return { limit().contains(point), false, 0 };
}
void SelectionOverlay::updateCursor(QPoint point)
{
    if (annotationTool_ && phase_ == Phase::Outer && confirmed_ && outer_.contains(point)
        && !borderHit(outer_, point) && !dragging_) { setCursor(Qt::CrossCursor); return; }
    const auto hit = dragging_ ? Hit { true, dragOuter_, edges_ } : hitTest(point);
    if (!hit.valid) { setCursor(phase_ == Phase::Windows ? Qt::PointingHandCursor : Qt::ArrowCursor); return; }
    if (hit.edges == 16) { setCursor(dragging_ ? Qt::ClosedHandCursor : Qt::OpenHandCursor); return; }
    if (!hit.edges) { setCursor(Qt::CrossCursor); return; }
    const bool horizontal = hit.edges & 3, vertical = hit.edges & 12;
    if (horizontal && vertical)
        setCursor(hit.edges == 5 || hit.edges == 10 ? Qt::SizeFDiagCursor : Qt::SizeBDiagCursor);
    else setCursor(horizontal ? Qt::SizeHorCursor : Qt::SizeVerCursor);
}

void SelectionOverlay::mousePressEvent(QMouseEvent *event)
{
    if (phase_ == Phase::Capturing) {
        if (event->button() == Qt::LeftButton && !outer_.contains(event->pos())) stop_();
        event->accept(); return;
    }
    if (event->button() == Qt::RightButton) { cancel_(); return; }
    if (event->button() != Qt::LeftButton) return;
    if (annotationTool_ && phase_ == Phase::Outer && confirmed_ && outer_.contains(event->pos())
        && !borderHit(outer_, event->pos())) {
        annotating_ = true;
        annotationStart_ = annotationEnd_ = event->pos();
        update();
        return;
    }
    const auto hit = hitTest(event->pos());
    if (!hit.valid && phase_ == Phase::Windows) {
        const QPoint point = physical(QRect(event->pos(), QSize(1, 1))).topLeft();
        for (int i = 0; i < candidates_.size(); ++i)
            if (candidates_[i].visible.contains(point)) { choose(i); break; }
        return;
    }
    if (!hit.valid) return;
    ++revision_;
    for (auto &job : hoverJobs_) job.reset();
    origin_ = event->pos();
    dragOuter_ = hit.outer;
    before_ = dragOuter_ ? outer_ : region_;
    edges_ = hit.edges;
    dragging_ = true;
    panel_->hide();
    updateCursor(event->pos());
}
void SelectionOverlay::mouseMoveEvent(QMouseEvent *event)
{
    if (annotating_) {
        annotationEnd_ = QPoint(qBound(outer_.left(), event->pos().x(), outer_.right()),
                               qBound(outer_.top(), event->pos().y(), outer_.bottom()));
        update();
        return;
    }
    if (!dragging_) { updateCursor(event->pos()); return; }
    if (phase_ == Phase::Outer && !confirmed_ && automatic_
        && (event->pos() - origin_).manhattanLength() < QApplication::startDragDistance()) return;
    const QRect bounds = dragOuter_ ? rect() : limit();
    if (bounds.isEmpty()) return;
    const QPoint point(qBound(bounds.left(), event->pos().x(), bounds.right()),
                       qBound(bounds.top(), event->pos().y(), bounds.bottom()));
    QRect next = before_;
    if (edges_ == 16) {
        next.translate(point - origin_);
        next.moveLeft(qBound(bounds.left(), next.left(), bounds.right() - next.width() + 1));
        next.moveTop(qBound(bounds.top(), next.top(), bounds.bottom() - next.height() + 1));
    } else if (edges_) {
        int left = before_.left(), right = before_.right(), top = before_.top(), bottom = before_.bottom();
        if (edges_ & 1) left = point.x();
        if (edges_ & 2) right = point.x();
        if (edges_ & 4) top = point.y();
        if (edges_ & 8) bottom = point.y();
        next = QRect(QPoint(qMin(left, right), qMin(top, bottom)), QPoint(qMax(left, right), qMax(top, bottom)));
    } else next = QRect(origin_, point).normalized().intersected(bounds);
    if (next.width() < 2 && bounds.width() >= 2) {
        next.setLeft(qMin(next.left(), bounds.right() - 1)); next.setWidth(2);
    }
    if (next.height() < 2 && bounds.height() >= 2) {
        next.setTop(qMin(next.top(), bounds.bottom() - 1)); next.setHeight(2);
    }
    if (dragOuter_) outer_ = next;
    else region_ = next;
    update();
}
void SelectionOverlay::mouseReleaseEvent(QMouseEvent *event)
{
    if (annotating_ && event->button() == Qt::LeftButton) {
        mouseMoveEvent(event);
        annotating_ = false;
        ensureAnnotations();
        annotations_->draw(annotationTool_, imagePoint(annotationStart_), imagePoint(annotationEnd_));
        update();
        return;
    }
    if (!dragging_ || event->button() != Qt::LeftButton) return;
    mouseMoveEvent(event);
    dragging_ = false;
    if (phase_ == Phase::Outer) {
        confirmed_ = !outer_.isEmpty();
        if (confirmed_) releaseColorKey();
    }
    if (dragOuter_) {
        if (phase_ != Phase::Outer) discover(true);
        else {
            label_->setText(QStringLiteral("直接选择矩形、箭头、文字或马赛克标注；完成复制，Esc 取消。"));
            refresh();
        }
    } else {
        if (edges_ == 0 && (event->pos() - origin_).manhattanLength() < 5) { probe(event->pos()); return; }
        if (edges_ == 16) wheel_ += region_.topLeft() - before_.topLeft();
        if (!region_.contains(wheel_) || edges_ == 0) wheel_ = region_.center();
        label_->setText(region_.width() < 32 || region_.height() < 80
            ? QStringLiteral("滚动区域过小，请拖动绿色边缘扩大或重新框选")
            : QStringLiteral("确认绿色滚动区域，然后点击开始；蓝色外框可直接拖边调整"));
        refresh();
    }
    updateCursor(event->pos());
}

QPoint SelectionOverlay::imagePoint(QPoint point) const
{
    return physical(QRect(point, QSize(1, 1))).topLeft() - screenRect_.topLeft();
}

void SelectionOverlay::ensureAnnotations()
{
    if (annotations_) return;
    annotations_ = new Editor(this);
    annotations_->setImage(samplingScreen_);
    connect(annotations_->undoStack(), &QUndoStack::indexChanged, this, [this] { update(); });
    connect(annotations_->undoStack(), &QUndoStack::canUndoChanged,
            annotationBar_->findChild<QPushButton *>("annotationUndo"), &QPushButton::setEnabled);
    connect(annotations_->undoStack(), &QUndoStack::canRedoChanged,
            annotationBar_->findChild<QPushButton *>("annotationRedo"), &QPushButton::setEnabled);
}

QImage SelectionOverlay::selectionImage() const
{
    const QRect area = physical(outer_).translated(-screenRect_.topLeft()).intersected(samplingScreen_.rect());
    if (area.isEmpty()) return {};
    return annotations_ ? annotations_->renderedImage(area) : samplingScreen_.copy(area);
}
