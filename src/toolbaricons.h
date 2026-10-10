#pragma once
#include <QIcon>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>

namespace ToolbarIcons {
enum Kind { Select, Crop, Rectangle, Arrow, Text, Mosaic, Undo, Redo, Save, Copy,
            Ocr, Capture, Scroll, Play, Window, Finish, Cancel, ZoomOut, ZoomIn, ActualSize, Fit };
inline QIcon icon(Kind kind)
{
    QIcon result;
    for (int scale : { 1, 2, 3 }) {
        QPixmap image(24 * scale, 24 * scale);
        image.fill(Qt::transparent);
        image.setDevicePixelRatio(scale);
        QPainter p(&image);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(QPen(QColor("#e5eaf2"), 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        auto line = [&](int x, int y, int x2, int y2) { p.drawLine(x, y, x2, y2); };
        switch (kind) {
        case Select:
            p.drawPolygon(QPolygonF(QVector<QPointF> { {5,3}, {18,12}, {12,13}, {9,20}, {5,3} })); break;
        case Crop:
            line(7,3,7,17); line(7,17,21,17); line(3,7,17,7); line(17,7,17,21); break;
        case Rectangle: p.drawRect(QRectF(4,5,16,14)); break;
        case Arrow: line(4,20,20,4); line(11,4,20,4); line(20,4,20,13); break;
        case Ocr: p.drawRect(QRectF(2,3,20,18)); line(7,7,17,7); line(12,7,12,17); break;
        case Text: line(5,5,19,5); line(12,5,12,20); line(8,20,16,20); break;
        case Mosaic:
            for (int y=0; y<3; ++y) for (int x=0; x<3; ++x)
                p.fillRect(QRect(4+x*5,4+y*5,5,5), QColor((x+y)%2 ? "#64748b" : "#e5eaf2"));
            break;
        case Undo: case Redo: {
            if (kind == Redo) { p.translate(24,0); p.scale(-1,1); }
            QPainterPath path; path.moveTo(5,8); path.cubicTo(22,5,22,19,13,19); p.drawPath(path);
            line(5,8,10,3); line(5,8,10,13); break;
        }
        case Save:
            p.drawRect(QRectF(4,4,16,16)); p.drawRect(QRectF(8,4,8,6));
            p.drawRect(QRectF(8,14,8,6)); break;
        case Copy: p.drawRect(QRectF(8,8,12,13)); line(16,4,4,4); line(4,4,4,17); break;
        case Capture: case Fit:
            for (int i=0;i<4;++i) { p.save(); p.translate(12,12); p.rotate(i*90);
                line(-8,-3,-8,-8); line(-8,-8,-3,-8); p.restore(); }
            if (kind == Capture) { line(9,12,15,12); line(12,9,12,15); } break;
        case Scroll:
            p.drawRect(QRectF(5,3,14,18)); line(12,6,12,17); line(8,13,12,17); line(12,17,16,13); break;
        case Play: p.drawPolygon(QPolygonF(QVector<QPointF> { {7,4}, {20,12}, {7,20}, {7,4} })); break;
        case Window: p.drawRect(QRectF(3,5,18,15)); line(3,9,21,9); break;
        case Finish: line(4,12,10,18); line(10,18,21,5); break;
        case Cancel: line(6,6,18,18); line(6,18,18,6); break;
        case ZoomOut: case ZoomIn:
            p.drawEllipse(QRectF(3,3,13,13)); line(15,15,21,21); line(6,9,13,9);
            if (kind == ZoomIn) line(9,6,9,13); break;
        case ActualSize: {
            QFont font = p.font(); font.setPixelSize(12); font.setBold(true); p.setFont(font);
            p.drawText(QRect(0,0,24,24), Qt::AlignCenter, "1:1"); break;
        }
        }
        p.end();
        result.addPixmap(image);
    }
    return result;
}
}
