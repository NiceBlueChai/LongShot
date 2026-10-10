#include "stitcher.h"
#include "capturecomposition.h"
#include <QPainter>
#include <algorithm>
#include <array>
#include <limits>

namespace {


int distance(QRgb a, QRgb b)
{
    return (qAbs(qRed(a) - qRed(b)) + qAbs(qGreen(a) - qGreen(b)) + qAbs(qBlue(a) - qBlue(b))) / 3;
}

double rowError(const QImage &a, const QImage &b, int y)
{
    const auto left = reinterpret_cast<const QRgb *>(a.constScanLine(y));
    const auto right = reinterpret_cast<const QRgb *>(b.constScanLine(y));
    double sum = 0;
    for (int i = 0; i < 31; ++i) {
        const int x = (i + 1) * (a.width() - 1) / 32;
        sum += distance(left[x], right[x]);
    }
    return sum / 31;
}

double bandError(const QImage &a, const QImage &b, int top, int bottom, int shift, int band, bool edges = true)
{
    const int overlap = a.height() - top - bottom - shift;
    double sum = 0;
    int samples = 0;
    const int firstX = qMax(1, a.width() * (2 + band * 32) / 100);
    const int lastX = a.width() * (32 + band * 32) / 100;
    const int stepX = qMax(1, (lastX - firstX) / 64);
    // Translucent sticky bars can change with scrolling; match the interior overlap.
    const int inset = edges && shift > 0 ? overlap / 8 : 0;
    for (int y = top + inset; y < top + overlap - inset; y += 2) {
        const auto previous = reinterpret_cast<const QRgb *>(a.constScanLine(y + shift));
        const auto current = reinterpret_cast<const QRgb *>(b.constScanLine(y));
        for (int x = firstX; x < lastX; x += stepX) {
            const auto p = previous[x], p0 = previous[x - 1];
            const auto c = current[x], c0 = current[x - 1];
            const int error = edges ? (qAbs((qRed(p) - qRed(p0)) - (qRed(c) - qRed(c0)))
                + qAbs((qGreen(p) - qGreen(p0)) - (qGreen(c) - qGreen(c0)))
                + qAbs((qBlue(p) - qBlue(p0)) - (qBlue(c) - qBlue(c0)))) / 3 : distance(p, c);
            // Horizontal edges retain text detail without treating hover-row fills as movement.
            if (!error && previous[x] == previous[x - 1] && current[x] == current[x - 1]) continue;
            sum += error;
            ++samples;
        }
    }
    return samples ? sum / samples : 0;
}

double shiftError(const QImage &a, const QImage &b, int top, int bottom, int shift,
                  const std::array<bool, 3> &active)
{
    double sum = 0;
    int count = 0;
    for (int band = 0; band < 3; ++band) {
        if (active[band]) { sum += bandError(a, b, top, bottom, shift, band); ++count; }
    }
    return count ? sum / count : std::numeric_limits<double>::max();
}
}

int Stitcher::nextScrollTicks(int currentTicks) const
{
    if (lastShift_ <= 0) return 1;
    const int body = previous_.height() - top_ - bottom_;
    return qBound(1, currentTicks * (body / 2) / lastShift_, 8);
}

bool Stitcher::start(const QImage &frame, QSize outputSize)
{
    outputSize_ = outputSize;
    strips_.clear();
    previous_ = {};
    height_ = frames_ = top_ = bottom_ = lastShift_ = 0;
    error_.clear();
    if (frame.isNull() || frame.width() < 32 || frame.height() < 80) {
        error_ = QStringLiteral("采集图片为空或尺寸过小");
        return false;
    }
    if (!captureMemoryFits(frame.size(), frame.height(), outputSize_)) {
        error_ = QStringLiteral("单帧过大，超过 256 MiB 图像内存上限");
        return false;
    }
    previous_ = frame.convertToFormat(QImage::Format_RGB32);
    if (previous_.isNull()) {
        error_ = QStringLiteral("无法分配采集图片内存");
        return false;
    }
    strips_ = { previous_ };
    height_ = frame.height();
    frames_ = 1;
    top_ = bottom_ = 0;
    error_.clear();
    return !previous_.isNull();
}

Stitcher::Result Stitcher::append(const QImage &input)
{
    if (previous_.isNull() || input.size() != previous_.size() || input.isNull()) {
        error_ = QStringLiteral("采集尺寸变化，已停止拼接");
        return Result::Rejected;
    }
    const auto frame = input.convertToFormat(QImage::Format_RGB32);
    if (frame.isNull()) {
        error_ = QStringLiteral("无法分配采集图片内存");
        return Result::Rejected;
    }
    if (similar(previous_, frame)) return Result::Unchanged;
    int top = top_;
    int bottom = bottom_;
    if (frames_ == 1) {
        while (top < frame.height() / 3 && rowError(previous_, frame, top) < 1.5) ++top;
        while (bottom < frame.height() / 3
               && rowError(previous_, frame, frame.height() - bottom - 1) < 1.5) ++bottom;
    } else {
        for (int y = 0; y < top_; ++y) {
            if (rowError(previous_, frame, y) > 5) { top = y; break; }
        }
        for (int y = 0; y < bottom_; ++y) {
            if (rowError(previous_, frame, frame.height() - y - 1) > 5) { bottom = y; break; }
        }
    }
    const int body = frame.height() - top - bottom;
    const int maxShift = body - qMax(32, body / 5);
    if (maxShift < 1) {
        error_ = QStringLiteral("可滚动内容太小，请扩大选区或排除固定头尾");
        return Result::Rejected;
    }
    QVector<double> scores(maxShift + 1, std::numeric_limits<double>::max());
    std::array<bool, 3> active = {};
    for (int band = 0; band < 3; ++band)
        active[band] = bandError(previous_, frame, top, bottom, 0, band) > 1;
    int bestShift = 0;
    double best = std::numeric_limits<double>::max();
    for (int shift = 1; shift <= maxShift; ++shift) {
        const auto score = shiftError(previous_, frame, top, bottom, shift, active);
        scores[shift] = score;
        if (score < best) { best = score; bestShift = shift; }
    }
    bool ambiguous = false;
    for (int shift = 1; shift <= maxShift; ++shift) {
        if (qAbs(shift - bestShift) > 3 && scores[shift] <= qMax(0.5, best * 1.5)) ambiguous = true;
    }
    if (!bestShift || best > 5 || ambiguous) {
        error_ = ambiguous ? QStringLiteral("内容重复，无法确定滚动距离；已保留确认的内容")
                           : QStringLiteral("找不到可靠重叠区域；请缩小滚动步长或排除动态内容");
        return Result::Rejected;
    }
    const int newHeight = height_ + bestShift;
    if (frames_ >= 100 || !captureMemoryFits(frame.size(), newHeight, outputSize_)) {
        error_ = QStringLiteral("已达到 100 帧或 256 MiB 图像内存上限");
        return Result::LimitReached;
    }
    const auto strip = frame.copy(0, frame.height() - bottom - bestShift, frame.width(), bestShift);
    if (strip.isNull()) {
        error_ = QStringLiteral("无法分配拼接图片内存");
        return Result::Rejected;
    }
    QImage restored;
    if (bottom < bottom_) {
        // First-pair blank space can move later; restore it before appending new content.
        restored = previous_.copy(0, previous_.height() - bottom_, frame.width(), bottom_ - bottom);
        if (restored.isNull()) {
            error_ = QStringLiteral("无法分配拼接图片内存");
            return Result::Rejected;
        }
    }
    if (frames_ == 1) {
        const auto first = previous_.copy(0, 0, frame.width(), frame.height() - bottom);
        if (first.isNull()) {
            error_ = QStringLiteral("无法分配拼接图片内存");
            return Result::Rejected;
        }
        strips_[0] = first;
    }
    if (!restored.isNull()) strips_.append(restored);
    top_ = top;
    bottom_ = bottom;
    strips_.append(strip);
    previous_ = frame;
    height_ = newHeight;
    lastShift_ = bestShift;
    ++frames_;
    return Result::Appended;
}

QImage Stitcher::image() const
{
    if (previous_.isNull()) return {};
    if (frames_ == 1) return previous_;
    QImage result(previous_.width(), height_, QImage::Format_RGB32);
    if (result.isNull()) return {};
    QPainter painter(&result);
    int y = 0;
    for (const auto &strip : strips_) {
        painter.drawImage(0, y, strip);
        y += strip.height();
    }
    if (bottom_)
        painter.drawImage(QRect(0, y, previous_.width(), bottom_), previous_,
                          QRect(0, previous_.height() - bottom_, previous_.width(), bottom_));
    return result;
}

bool Stitcher::similar(const QImage &a, const QImage &b)
{
    if (a.isNull() || b.isNull() || a.size() != b.size()) return false;
    const auto left = a.convertToFormat(QImage::Format_RGB32);
    const auto right = b.convertToFormat(QImage::Format_RGB32);
    for (int band = 0; band < 3; ++band)
        if (bandError(left, right, 0, 0, 0, band, false) >= 1.5) return false;
    return true;
}
