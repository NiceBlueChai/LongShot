#pragma once
#include <QImage>
#include <QString>
#include <atomic>
class QWidget;
namespace Ocr {
struct Result { QString text, language, error; };
Result recognize(const QImage &image, const QString &language, const std::atomic_bool &cancel);
Result recognizeModel(const QImage &image, const std::atomic_bool &cancel);
void showDialog(QWidget *parent, const QImage &image);
}
