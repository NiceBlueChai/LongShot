#include "editor.h"
#include "toolbaricons.h"
#include "ocr.h"

#include <QActionGroup>
#include <QApplication>
#include <QClipboard>
#include <QFileDialog>
#include <QGraphicsItem>
#include <QInputDialog>
#include <QKeyEvent>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QSaveFile>
#include <QShortcut>
#include <QScreen>
#include <QScrollBar>
#include <QStatusBar>
#include <QToolBar>
#include <QWheelEvent>
#include <QVBoxLayout>
#include <cmath>
#include <functional>

namespace {
class EditCommand : public QUndoCommand
{
public:
    EditCommand(const QString &name, std::function<void()> redo, std::function<void()> undo)
        : QUndoCommand(name), redo_(std::move(redo)), undo_(std::move(undo)) {}
    void redo() override { redo_(); }
    void undo() override { undo_(); }
private:
    std::function<void()> redo_;
    std::function<void()> undo_;
};

class AddCommand : public QUndoCommand
{
public:
    AddCommand(QGraphicsScene *scene, QGraphicsItem *item)
        : QUndoCommand(QStringLiteral("添加标注")), scene_(scene), item_(item) {}
    ~AddCommand() override { if (!item_->scene()) delete item_; }
    void redo() override { scene_->addItem(item_); }
    void undo() override { scene_->removeItem(item_); }
private:
    QGraphicsScene *scene_;
    QGraphicsItem *item_;
};
}

EditorView::EditorView(QWidget *parent) : QGraphicsView(parent)
{
    setRenderHints(QPainter::Antialiasing | QPainter::SmoothPixmapTransform);
    setBackgroundBrush(Qt::NoBrush);
    setStyleSheet("QGraphicsView { background: transparent; }");
    viewport()->setAutoFillBackground(false);
    setTransformationAnchor(AnchorUnderMouse);
    setDragMode(ScrollHandDrag);
}

void EditorView::setTool(Tool tool)
{
    tool_ = tool;
    setDragMode(tool == Tool::Select ? ScrollHandDrag : NoDrag);
    setCursor(tool == Tool::Select ? Qt::OpenHandCursor : Qt::CrossCursor);
}

void EditorView::setZoom(qreal zoom)
{
    zoom = qBound(0.05, zoom, 8.0);
    scale(zoom / this->zoom(), zoom / this->zoom());
    emit zoomChanged(zoom);
}

void EditorView::wheelEvent(QWheelEvent *event)
{
    setZoom(zoom() * (event->angleDelta().y() > 0 ? 1.2 : 1.0 / 1.2));
    event->accept();
}

void EditorView::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && !space_) {
        start_ = mapToScene(event->pos());
        if (tool_ != Tool::Select) {
            drawing_ = true;
            preview_ = scene()->addRect(QRectF(start_, start_), QPen(Qt::cyan, 1, Qt::DashLine));
            preview_->setZValue(10000);
            event->accept();
            return;
        }
        scene()->clearSelection();
        moving_ = itemAt(event->pos());
        if (moving_ && (moving_->flags() & QGraphicsItem::ItemIsMovable))
            itemStart_ = moving_->pos();
        else
            moving_ = nullptr;
    }
    QGraphicsView::mousePressEvent(event);
}

void EditorView::mouseMoveEvent(QMouseEvent *event)
{
    if (drawing_) {
        preview_->setRect(QRectF(start_, mapToScene(event->pos())).normalized());
        return;
    }
    QGraphicsView::mouseMoveEvent(event);
}

void EditorView::mouseReleaseEvent(QMouseEvent *event)
{
    if (drawing_ && event->button() == Qt::LeftButton) {
        drawing_ = false;
        delete preview_;
        preview_ = nullptr;
        emit drawFinished(int(tool_), start_, mapToScene(event->pos()));
        return;
    }
    QGraphicsView::mouseReleaseEvent(event);
    if (moving_) {
        if (moving_->pos() != itemStart_)
            emit itemMoved(moving_, itemStart_, moving_->pos());
        moving_ = nullptr;
    }
}

void EditorView::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (tool_ == Tool::Select && !space_) {
        if (auto item = qgraphicsitem_cast<QGraphicsTextItem *>(itemAt(event->pos()))) {
            emit editText(item);
            return;
        }
    }
    QGraphicsView::mouseDoubleClickEvent(event);
}

void EditorView::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Space && !event->isAutoRepeat()) {
        space_ = true;
        setDragMode(ScrollHandDrag);
        event->accept();
    } else {
        QGraphicsView::keyPressEvent(event);
    }
}

void EditorView::keyReleaseEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Space && !event->isAutoRepeat()) {
        space_ = false;
        setTool(tool_);
        event->accept();
    } else {
        QGraphicsView::keyReleaseEvent(event);
    }
}

Editor::Editor(QWidget *parent)
    : QMainWindow(parent), scene_(new QGraphicsScene(this)), view_(new EditorView(this))
{
    setWindowTitle(QStringLiteral("LongShot · 截图"));
    setWindowFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
    setAttribute(Qt::WA_TranslucentBackground);
    setGeometry(QGuiApplication::primaryScreen()->availableGeometry());
    setStyleSheet("QMainWindow { background: transparent; }"
                  "QToolBar { background: #202938; spacing: 6px; padding: 8px; border: 1px solid #42516a; border-radius: 8px; }"
                  "QToolButton { color: #e5eaf2; padding: 7px 9px; border-radius: 5px; }"
                  "QToolButton:hover { background: #36465c; }"
                  "QToolButton:checked { background: #2563eb; color: white; }"
                  "QStatusBar { background: transparent; color: #e5eaf2; }");
    connect(new QShortcut(QKeySequence(Qt::Key_Escape), this), &QShortcut::activated, this, &QWidget::close);
    view_->setScene(scene_);
    view_->setFrameShape(QFrame::NoFrame);
    auto canvas = new QWidget(this);
    auto layout = new QVBoxLayout(canvas);
    layout->setContentsMargins(12, 12, 12, 8);
    layout->setSpacing(12);
    layout->addWidget(view_, 1);
    auto bar = new QToolBar(QStringLiteral("编辑"), canvas);
    auto tools = new QHBoxLayout;
    tools->addStretch();
    tools->addWidget(bar);
    tools->addStretch();
    layout->addLayout(tools);
    setCentralWidget(canvas);
    bar->setMovable(false);
    bar->setToolButtonStyle(Qt::ToolButtonIconOnly);
    bar->setIconSize(QSize(20, 20));
    bar->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);
    auto capture = bar->addAction(QStringLiteral("新截图"));
    connect(capture, &QAction::triggered, this, &Editor::newCapture);
    bar->addSeparator();
    auto group = new QActionGroup(this);
    const QStringList names = { QStringLiteral("选择"), QStringLiteral("裁剪"), QStringLiteral("矩形"),
                                QStringLiteral("箭头"), QStringLiteral("文字"), QStringLiteral("马赛克") };
    for (int i = 0; i < names.size(); ++i) {
        auto action = bar->addAction(names[i]);
        action->setCheckable(true);
        group->addAction(action);
        action->setChecked(i == 0);
        connect(action, &QAction::triggered, this, [this, i] { view_->setTool(EditorView::Tool(i)); });
    }
    bar->addSeparator();
    auto undo = undo_.createUndoAction(this, QStringLiteral("撤销"));
    undo->setShortcut(QKeySequence::Undo);
    bar->addAction(undo);
    auto redo = undo_.createRedoAction(this, QStringLiteral("重做"));
    redo->setShortcut(QKeySequence::Redo);
    bar->addAction(redo);
    bar->addSeparator();
    connect(bar->addAction(QStringLiteral("−")), &QAction::triggered, this,
            [this] { view_->setZoom(view_->zoom() / 1.2); });
    connect(bar->addAction(QStringLiteral("+")), &QAction::triggered, this,
            [this] { view_->setZoom(view_->zoom() * 1.2); });
    connect(bar->addAction(QStringLiteral("100%")), &QAction::triggered, this,
            [this] { view_->setZoom(1); });
    connect(bar->addAction(QStringLiteral("适应")), &QAction::triggered, this, [this] {
        view_->fitInView(scene_->sceneRect(), Qt::KeepAspectRatio);
        view_->setZoom(view_->zoom());
    });
    bar->addSeparator();
    auto save = bar->addAction(QStringLiteral("保存 PNG"));
    save->setShortcut(QKeySequence::Save);
    connect(save, &QAction::triggered, this, [this] {
        if (imageRect_.isEmpty()) return;
        auto path = QFileDialog::getSaveFileName(this, QStringLiteral("保存截图"),
                                                "LongShot.png", "PNG (*.png)");
        if (path.isEmpty()) return;
        if (!path.endsWith(".png", Qt::CaseInsensitive)) path += ".png";
        QString error;
        if (!savePng(path, &error)) QMessageBox::warning(this, QStringLiteral("保存失败"), error);
    });
    auto copy = bar->addAction(QStringLiteral("复制图片"));
    copy->setShortcut(QKeySequence::Copy);
    connect(copy, &QAction::triggered, this, &Editor::copyImage);
    bar->addSeparator();
    connect(bar->addAction(QStringLiteral("完成 · 复制并收起")), &QAction::triggered, this, [this] {
        if (copyImage()) close();
        else QMessageBox::warning(this, QStringLiteral("复制失败"), QStringLiteral("无法生成图片，请保存或重试。"));
    });
    const ToolbarIcons::Kind icons[] = { ToolbarIcons::Capture, ToolbarIcons::Select, ToolbarIcons::Crop,
        ToolbarIcons::Rectangle, ToolbarIcons::Arrow, ToolbarIcons::Text, ToolbarIcons::Mosaic,
        ToolbarIcons::Undo, ToolbarIcons::Redo, ToolbarIcons::ZoomOut, ToolbarIcons::ZoomIn,
        ToolbarIcons::ActualSize, ToolbarIcons::Fit, ToolbarIcons::Save, ToolbarIcons::Copy, ToolbarIcons::CompleteCopy };
    int iconIndex = 0;
    for (auto action : bar->actions()) {
        if (action->isSeparator()) continue;
        action->setIcon(ToolbarIcons::icon(icons[iconIndex++]));
        action->setToolTip(action->text() + (action->shortcut().isEmpty() ? QString()
            : QString(" (%1)").arg(action->shortcut().toString(QKeySequence::NativeText))));
    }
    auto ocr = bar->addAction(ToolbarIcons::icon(ToolbarIcons::Ocr), QStringLiteral("识别文字"));
    ocr->setToolTip(QStringLiteral("识别文字（离线 OCR）"));
    connect(ocr, &QAction::triggered, this, [this] { Ocr::showDialog(this, renderedImage()); });
    connect(view_, &EditorView::drawFinished, this, &Editor::draw);
    connect(view_, &EditorView::itemMoved, this, [this](QGraphicsItem *item, QPointF before, QPointF after) {
        undo_.push(new EditCommand(QStringLiteral("移动标注"), [=] { item->setPos(after); },
                                  [=] { item->setPos(before); }));
    });
    connect(view_, &EditorView::editText, this, [this](QGraphicsItem *item) {
        auto text = static_cast<QGraphicsTextItem *>(item);
        const auto before = text->toPlainText();
        bool ok = false;
        const auto after = QInputDialog::getMultiLineText(this, QStringLiteral("编辑文字"),
                                                         QStringLiteral("文字"), before, &ok);
        if (ok && before != after)
            undo_.push(new EditCommand(QStringLiteral("编辑文字"), [=] { text->setPlainText(after); },
                                      [=] { text->setPlainText(before); }));
    });
    connect(view_, &EditorView::zoomChanged, this, [this](qreal zoom) {
        statusBar()->showMessage(QStringLiteral("缩放 %1% · 滚轮缩放，选择模式直接拖动，空格临时拖动")
                                 .arg(qRound(zoom * 100)));
    });
    statusBar()->showMessage(QStringLiteral("点击“新截图”，框选需要截取的画面"));
}

void Editor::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.fillRect(rect(), QColor(12, 20, 32, 110));
}

void Editor::setImage(const QImage &image, const QString &message)
{
    undo_.clear();
    scene_->clear();
    imageRect_ = QRectF(QPointF(), image.size());
    if (!image.isNull()) {
        auto background = scene_->addPixmap(QPixmap::fromImage(image));
        background->setTransformationMode(Qt::SmoothTransformation);
        background->setZValue(-1);
    }
    scene_->setSceneRect(imageRect_);
    view_->resetTransform();
    if (!image.isNull())
        view_->setZoom(qMin(1.0, qMax(1, view_->viewport()->width() - 24) / qreal(image.width())));
    view_->verticalScrollBar()->setValue(view_->verticalScrollBar()->minimum());
    view_->setFocus();
    statusBar()->showMessage(message.isEmpty() ? QStringLiteral("滚轮缩放，选择模式直接拖动；拖动标注可调整位置") : message);
}

void Editor::addItem(QGraphicsItem *item)
{
    item->setFlags(QGraphicsItem::ItemIsSelectable | QGraphicsItem::ItemIsMovable);
    undo_.push(new AddCommand(scene_, item));
}

void Editor::crop(const QRectF &rect)
{
    const auto before = scene_->sceneRect();
    const QRect after = rect.normalized().toAlignedRect().intersected(before.toRect());
    if (after.width() < 2 || after.height() < 2) return;
    undo_.push(new EditCommand(QStringLiteral("裁剪"), [=] { scene_->setSceneRect(after); },
                              [=] { scene_->setSceneRect(before); }));
}

void Editor::addRectangle(const QRectF &rect)
{
    auto item = new QGraphicsRectItem(rect.normalized());
    item->setPen(QPen(QColor("#ef4444"), 3));
    addItem(item);
}

void Editor::addArrow(QPointF from, QPointF to)
{
    const auto angle = std::atan2(to.y() - from.y(), to.x() - from.x());
    QPainterPath path(from);
    path.lineTo(to);
    for (double side : { -0.5, 0.5 }) {
        path.moveTo(to);
        path.lineTo(to - QPointF(16 * std::cos(angle + side), 16 * std::sin(angle + side)));
    }
    auto item = new QGraphicsPathItem(path);
    item->setPen(QPen(QColor("#ef4444"), 3, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    addItem(item);
}

void Editor::addText(QPointF position, const QString &text)
{
    auto item = new QGraphicsTextItem(text);
    QFont font(QStringLiteral("Microsoft YaHei"));
    font.setPixelSize(24);
    item->setFont(font);
    item->setDefaultTextColor(QColor("#ef4444"));
    item->setPos(position);
    addItem(item);
}

void Editor::draw(int tool, QPointF from, QPointF to)
{
    if (imageRect_.isEmpty()) return;
    const auto type = EditorView::Tool(tool);
    const auto rect = QRectF(from, to).normalized();
    if (type == EditorView::Tool::Crop) crop(rect);
    if (type == EditorView::Tool::Rectangle && rect.width() > 2 && rect.height() > 2) addRectangle(rect);
    if (type == EditorView::Tool::Arrow && QLineF(from, to).length() > 4) addArrow(from, to);
    if (type == EditorView::Tool::Mosaic) addMosaic(rect);
    if (type == EditorView::Tool::Text) {
        bool ok = false;
        const auto text = QInputDialog::getMultiLineText(parentWidget() ? parentWidget() : this, QStringLiteral("添加文字"),
                                                        QStringLiteral("文字"), QString(), &ok);
        if (ok && !text.isEmpty()) addText(from, text);
    }
}

void Editor::addMosaic(const QRectF &area)
{
    const QRect rect = area.normalized().toAlignedRect().intersected(scene_->sceneRect().toAlignedRect());
    if (rect.width() < 2 || rect.height() < 2) return;
    const auto source = renderedImage(rect);
    if (source.isNull()) return;
    const auto mosaic = source.scaled(qMax(1, (rect.width() + 11) / 12),
        qMax(1, (rect.height() + 11) / 12), Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
        .scaled(rect.size(), Qt::IgnoreAspectRatio, Qt::FastTransformation);
    auto item = new QGraphicsPixmapItem(QPixmap::fromImage(mosaic));
    item->setPos(rect.topLeft());
    // Keep redaction anchored: moving it would expose the original pixels.
    undo_.push(new AddCommand(scene_, item));
}

void Editor::paintImage(QPainter &painter, const QRectF &target, const QRectF &source) const
{
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    scene_->render(&painter, target, source, Qt::IgnoreAspectRatio);
    painter.restore();
}

QImage Editor::renderedImage(const QRectF &area) const
{
    const auto rect = area.isEmpty() ? scene_->sceneRect() : area.intersected(imageRect_);
    if (imageRect_.isEmpty() || rect.isEmpty()) return {};
    QImage image(rect.size().toSize(), QImage::Format_RGB32);
    if (image.isNull()) return {};
    image.fill(Qt::white);
    const auto selected = scene_->selectedItems();
    scene_->clearSelection();
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    scene_->render(&painter, QRectF(QPointF(), image.size()), rect, Qt::IgnoreAspectRatio);
    painter.end();
    for (auto item : selected) item->setSelected(true);
    return image;
}

bool Editor::savePng(const QString &path, QString *error) const
{
    const auto image = renderedImage();
    QSaveFile file(path);
    if (image.isNull() || !file.open(QIODevice::WriteOnly) || !image.save(&file, "PNG") || !file.commit()) {
        if (error) *error = file.errorString().isEmpty() ? QStringLiteral("无法生成 PNG 图片") : file.errorString();
        return false;
    }
    return true;
}

bool Editor::copyImage() const
{
    const auto image = renderedImage();
    if (image.isNull()) return false;
    QApplication::clipboard()->setImage(image);
    return true;
}
