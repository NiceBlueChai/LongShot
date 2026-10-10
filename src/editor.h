#pragma once

#include <QGraphicsView>
#include <QMainWindow>
#include <QUndoStack>

class EditorView : public QGraphicsView
{
    Q_OBJECT
public:
    enum class Tool { Select, Crop, Rectangle, Arrow, Text, Mosaic };
    explicit EditorView(QWidget *parent = nullptr);
    void setTool(Tool tool);
    void setZoom(qreal zoom);
    qreal zoom() const { return transform().m11(); }
signals:
    void drawFinished(int tool, QPointF start, QPointF end);
    void itemMoved(QGraphicsItem *item, QPointF before, QPointF after);
    void editText(QGraphicsItem *item);
    void zoomChanged(qreal zoom);
protected:
    void wheelEvent(QWheelEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void keyReleaseEvent(QKeyEvent *event) override;
private:
    Tool tool_ = Tool::Select;
    bool space_ = false;
    bool drawing_ = false;
    QPointF start_;
    QPointF itemStart_;
    QGraphicsItem *moving_ = nullptr;
    QGraphicsRectItem *preview_ = nullptr;
};

class Editor : public QMainWindow
{
    Q_OBJECT
public:
    explicit Editor(QWidget *parent = nullptr);
    void setImage(const QImage &image, const QString &message = QString());
    QImage renderedImage(const QRectF &area = QRectF()) const;
    void paintImage(QPainter &painter, const QRectF &target, const QRectF &source) const;
    void draw(int tool, QPointF from, QPointF to);
    void addMosaic(const QRectF &rect);
    void crop(const QRectF &rect);
    void addRectangle(const QRectF &rect);
    void addArrow(QPointF from, QPointF to);
    void addText(QPointF position, const QString &text);
    bool savePng(const QString &path, QString *error = nullptr) const;
    bool copyImage() const;
    EditorView *view() const { return view_; }
    QUndoStack *undoStack() { return &undo_; }
signals:
    void newCapture();
protected:
    void paintEvent(QPaintEvent *event) override;
private:
    void addItem(QGraphicsItem *item);
    QGraphicsScene *scene_;
    EditorView *view_;
    QUndoStack undo_;
    QRectF imageRect_;
};
