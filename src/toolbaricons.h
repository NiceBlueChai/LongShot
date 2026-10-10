#pragma once
#include <QIcon>
#include <QPainter>
#include <QPixmap>
#include <QSvgRenderer>

namespace ToolbarIcons {
enum Kind { Select, Crop, Rectangle, Arrow, Text, Mosaic, Undo, Redo, Save, Copy,
            Ocr, Capture, Scroll, Play, Window, Finish, Cancel, ZoomOut, ZoomIn, ActualSize, Fit,
            Move, Region, CompleteCopy, Count };
inline QIcon icon(Kind kind)
{
    static const char *const files[] = {
        "select", "crop", "rectangle", "arrow", "text", "mosaic", "undo", "redo", "save", "copy",
        "ocr", "capture", "scroll", "play", "window", "finish", "cancel", "zoom-out", "zoom-in",
        "actual-size", "fit", "move", "region", "complete-copy"
    };
    static_assert(sizeof(files) / sizeof(files[0]) == Count, "Every toolbar icon needs an SVG");
    QSvgRenderer renderer(QString(":/icons/toolbar/%1.svg").arg(files[kind]));
    if (!renderer.isValid()) {
        qWarning("Unable to load toolbar SVG: %s", files[kind]);
        return {};
    }
    QIcon result;
    for (int scale : { 1, 2, 3 }) {
        QPixmap image(24 * scale, 24 * scale);
        image.fill(Qt::transparent);
        image.setDevicePixelRatio(scale);
        QPainter painter(&image);
        renderer.render(&painter, QRectF(0, 0, 24, 24));
        painter.end();
        result.addPixmap(image);
    }
    return result;
}
}
