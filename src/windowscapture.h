#pragma once

#include <QImage>
#include <QRect>
#include <QString>
#include <QVector>
#include <QRegion>
#include <QIcon>
#include <Windows.h>

namespace WindowsCapture
{
    struct WindowCandidate {
        HWND window = nullptr;
        DWORD process = 0;
        QString title;
        QIcon icon;
        QRect bounds;
        QRect client;
        QRegion visible;
    };
    QVector<WindowCandidate> candidates(const QRect &selection);
    bool validTarget(HWND window, DWORD process, const QRect &region);
    QVector<QRect> refineElementRegions(const QVector<QRect> &candidates, QRect window, QPoint point,
                                       QRect current = {});
    QVector<QRect> pixelRegions(const QImage &image, QPoint point);
    QVector<QRect> elementRegions(HWND window, QPoint physicalPoint, bool accessibility = false);
    QRect scrollRegion(HWND window, QPoint physicalPoint);
    QRect toLogical(const QRect &physical, const QSize &logical, const QRect &screen);
    QImage grab(const QRect &physicalRect, QString *error);
    bool scrollDown(int ticks, QString *error);
    QRect monitorRect(QPoint physicalPoint);
    QRect screenRect(const QString &screenName);
    QRect toPhysical(const QRect &localRect, const QSize &logicalSize, const QRect &physicalScreen);
    void enableDpiAwareness();
}
