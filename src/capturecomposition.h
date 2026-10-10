#pragma once
#include <QImage>
#include <QRect>
#include <QString>
bool captureMemoryFits(QSize frame, int stitchedHeight, QSize output = {});
QImage composeCapture(const QImage &base, const QImage &scroll, const QRect &region, QString *error = nullptr);
