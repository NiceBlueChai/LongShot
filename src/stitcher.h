#pragma once

#include <QImage>
#include <QString>
#include <QVector>

class Stitcher
{
public:
    enum class Result { Appended, Unchanged, Rejected, LimitReached };
    bool start(const QImage &frame, QSize outputSize = {});
    Result append(const QImage &frame);
    QImage image() const;
    QImage lastFrame() const { return previous_; }
    QString error() const { return error_; }
    static bool similar(const QImage &a, const QImage &b);
    int nextScrollTicks(int currentTicks) const;

private:
    QSize outputSize_;
    QImage previous_;
    QVector<QImage> strips_;
    QString error_;
    int top_ = 0;
    int bottom_ = 0;
    int height_ = 0;
    int frames_ = 0;
    int lastShift_ = 0;
};
