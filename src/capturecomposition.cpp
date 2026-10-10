#include "capturecomposition.h"
#include <QPainter>

bool captureMemoryFits(QSize frame, int height, QSize output)
{
    if (frame.isEmpty() || height < frame.height()) return false;
    const qint64 scrolling = qint64(frame.width()) * 4 * (2LL * height + 4LL * frame.height());
    const qint64 composite = output.isEmpty() ? 0 : qint64(output.width()) * 4
        * (2LL * output.height() + 2LL * height - 2LL * frame.height());
    return scrolling + composite <= 256LL * 1024 * 1024;
}

QImage composeCapture(const QImage &base, const QImage &scroll, const QRect &region, QString *error)
{
    if (base.isNull() || scroll.isNull() || !base.rect().contains(region) || region.isEmpty()
        || scroll.width() != region.width() || scroll.height() < region.height()
        || !captureMemoryFits(region.size(), scroll.height(), base.size())) {
        if (error) *error = QStringLiteral("合成范围无效或超过 256 MiB 图像内存上限");
        return {};
    }
    const int extra = scroll.height() - region.height();
    QImage output(base.width(), base.height() + extra, QImage::Format_RGB32);
    if (output.isNull()) {
        if (error) *error = QStringLiteral("无法分配完整选区图片内存");
        return {};
    }
    output.fill(Qt::white);
    QPainter painter(&output);
    const int split = region.y() + region.height();
    painter.drawImage(QRect(0, 0, base.width(), split), base, QRect(0, 0, base.width(), split));
    if (split < base.height())
        painter.drawImage(QRect(0, split + extra, base.width(), base.height() - split), base,
                          QRect(0, split, base.width(), base.height() - split));
    painter.drawImage(region.topLeft(), scroll);
    return output;
}
