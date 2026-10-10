#include "ocr.h"
#define ORT_API_MANUAL_INIT
#include <onnxruntime_cxx_api.h>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QLibrary>
#include <QThread>
#include <QtMath>
#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <mutex>
#include <vector>

namespace {
struct Engine {
    QLibrary library;
    Ort::Env environment{nullptr};
    Ort::Session detector{nullptr}, recognizer{nullptr};
    QStringList characters;
    std::string detInput, detOutput, recInput, recOutput;

    Engine() : library(QCoreApplication::applicationDirPath() + "/ocr/onnxruntime.dll")
    {
        const auto base = QCoreApplication::applicationDirPath() + "/ocr/";
        if (!QFileInfo::exists(base + "det.onnx") || !QFileInfo::exists(base + "rec.onnx"))
            throw QStringLiteral("缺少离线模型，请将完整 ocr 目录放在程序旁边，或切换 Windows OCR。");
        if (!library.load()) throw QStringLiteral("离线 OCR 运行库加载失败：") + library.errorString();
        using GetApi = const OrtApiBase *(ORT_API_CALL *)();
        const auto getApi = reinterpret_cast<GetApi>(library.resolve("OrtGetApiBase"));
        if (!getApi) throw QStringLiteral("无效的 ONNX Runtime 运行库。");
        const auto api = getApi()->GetApi(ORT_API_VERSION);
        if (!api) throw QStringLiteral("ONNX Runtime 版本不兼容，请使用配套的 ocr 目录。");
        Ort::InitApi(api);
        environment = Ort::Env(ORT_LOGGING_LEVEL_WARNING, "LongShot");
        Ort::SessionOptions options;
        options.SetIntraOpNumThreads(qBound(1, QThread::idealThreadCount() / 2, 4));
        options.SetInterOpNumThreads(1);
        options.DisableCpuMemArena();
        options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        detector = Ort::Session(environment, (base + "det.onnx").toStdWString().c_str(), options);
        recognizer = Ort::Session(environment, (base + "rec.onnx").toStdWString().c_str(), options);
        Ort::AllocatorWithDefaultOptions allocator;
        detInput = detector.GetInputNameAllocated(0, allocator).get();
        detOutput = detector.GetOutputNameAllocated(0, allocator).get();
        recInput = recognizer.GetInputNameAllocated(0, allocator).get();
        recOutput = recognizer.GetOutputNameAllocated(0, allocator).get();
        const auto dictionary = recognizer.GetModelMetadata().LookupCustomMetadataMapAllocated("character", allocator);
        if (!dictionary) throw QStringLiteral("识别模型缺少字典，请重新部署模型。");
        characters = QString::fromUtf8(dictionary.get()).split('\n');
        characters.prepend(QString()); // CTC blank is index zero; the model dictionary excludes it.
        characters.append(QStringLiteral(" "));
    }
};

std::mutex engineMutex;
std::unique_ptr<Engine> engine;

void checkCanceled(const std::atomic_bool &cancel, const QElapsedTimer &timer)
{
    if (cancel) throw QString();
    if (timer.elapsed() > 60000) throw QStringLiteral("识别超时，请缩小选区后重试。");
}

Ort::Value infer(Ort::Session &session, const std::string &inputName, const std::string &outputName,
                 const QImage &image, int width, int height)
{
    const auto resized = image.scaled(width, height, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                             .convertToFormat(QImage::Format_RGB32);
    if (resized.isNull()) throw QStringLiteral("内存不足，无法处理图片。");
    const size_t plane = size_t(width) * height;
    std::vector<float> pixels(plane * 3);
    for (int y = 0; y < height; ++y) {
        const auto row = reinterpret_cast<const QRgb *>(resized.constScanLine(y));
        for (int x = 0; x < width; ++x) {
            const auto index = size_t(y) * width + x;
            pixels[index] = qBlue(row[x]) / 127.5f - 1.0f;
            pixels[plane + index] = qGreen(row[x]) / 127.5f - 1.0f;
            pixels[2 * plane + index] = qRed(row[x]) / 127.5f - 1.0f;
        }
    }
    const std::array<int64_t, 4> shape = {1, 3, height, width};
    const auto memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    auto input = Ort::Value::CreateTensor<float>(memory, pixels.data(), pixels.size(), shape.data(), shape.size());
    const char *inputs[] = {inputName.c_str()}, *outputs[] = {outputName.c_str()};
    auto values = session.Run(Ort::RunOptions{nullptr}, inputs, &input, 1, outputs, 1);
    return std::move(values.front());
}

std::vector<QRect> detect(Engine &model, const QImage &image, const std::atomic_bool &cancel,
                          const QElapsedTimer &timer)
{
    const double scale = qMin(2.0, 1536.0 / qMax(image.width(), image.height()));
    const int width = qMax(32, qRound(image.width() * scale / 32) * 32);
    const int height = qMax(32, qRound(image.height() * scale / 32) * 32);
    auto output = infer(model.detector, model.detInput, model.detOutput, image, width, height);
    const auto shape = output.GetTensorTypeAndShapeInfo().GetShape();
    if (shape.size() != 4 || shape[0] != 1 || shape[1] != 1 || shape[2] != height || shape[3] != width)
        throw QStringLiteral("文字检测模型输出不匹配。");
    const auto data = output.GetTensorData<float>();
    std::vector<unsigned char> mask(size_t(width) * height);
    for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
        const int index = y * width + x;
        // One-pixel dilation connects small gaps in a detected text line.
        mask[index] = data[index] > 0.3f || (x && data[index - 1] > 0.3f)
            || (y && data[index - width] > 0.3f) || (x && y && data[index - width - 1] > 0.3f);
    }
    std::vector<QRect> boxes;
    std::vector<int> queue;
    for (int seed = 0; seed < width * height; ++seed) {
        if (!mask[seed]) continue;
        checkCanceled(cancel, timer);
        queue.clear(); queue.push_back(seed); mask[seed] = 0;
        int left = seed % width, right = left, top = seed / width, bottom = top;
        for (size_t j = 0; j < queue.size(); ++j) {
            const int index = queue[j], x = index % width, y = index / width;
            left = qMin(left, x); right = qMax(right, x); top = qMin(top, y); bottom = qMax(bottom, y);
            for (int dy = -1; dy <= 1; ++dy) for (int dx = -1; dx <= 1; ++dx) {
                const int nx = x + dx, ny = y + dy;
                if (nx < 0 || nx >= width || ny < 0 || ny >= height) continue;
                const int neighbor = ny * width + nx;
                if (mask[neighbor]) { mask[neighbor] = 0; queue.push_back(neighbor); }
            }
        }
        const int w = right - left + 1, h = bottom - top + 1;
        if (w < 3 || h < 3) continue;
        double score = 0;
        for (int y = top; y <= bottom; ++y) for (int x = left; x <= right; ++x) score += data[y * width + x];
        if (score / (w * h) < 0.5) continue;
        // shortcut: axis-aligned screenshot text; use rotated polygons if document/photo OCR is added.
        const double expand = w * double(h) * 1.6 / (2 * (w + h));
        const QRect box(QPoint(qFloor((left - expand) * image.width() / width),
                               qFloor((top - expand) * image.height() / height)),
                        QPoint(qCeil((right + expand) * image.width() / width),
                               qCeil((bottom + expand) * image.height() / height)));
        const auto clipped = box.intersected(image.rect());
        if (clipped.width() >= 3 && clipped.height() >= 3) boxes.push_back(clipped);
        if (boxes.size() > 1000) throw QStringLiteral("文字区域过多，请分区域识别。");
    }
    std::sort(boxes.begin(), boxes.end(), [](const QRect &a, const QRect &b) {
        return a.top() == b.top() ? a.left() < b.left() : a.top() < b.top();
    });
    // Stable row grouping avoids a non-transitive fuzzy comparator in std::sort.
    for (size_t begin = 0; begin < boxes.size();) {
        size_t end = begin + 1;
        while (end < boxes.size() && boxes[end].top() - boxes[begin].top()
               < qMin(boxes[begin].height(), boxes[end].height()) / 2) ++end;
        std::sort(boxes.begin() + begin, boxes.begin() + end,
                  [](const QRect &a, const QRect &b) { return a.left() < b.left(); });
        begin = end;
    }
    return boxes;
}
}

Ocr::Result Ocr::recognizeModel(const QImage &image, const std::atomic_bool &cancel)
{
    Result result;
    result.language = QStringLiteral("PP-OCRv5 · 中英混合");
    QElapsedTimer timer;
    timer.start();
    try {
        if (image.isNull()) throw QStringLiteral("没有可识别的图片，请先框选区域。");
        if (image.width() > 4096 || image.height() > 4096)
            throw QStringLiteral("本版模型 OCR 支持单边不超过 4096 像素，请裁剪后识别。");
        std::unique_lock<std::mutex> lock(engineMutex, std::defer_lock);
        while (!lock.try_lock()) { checkCanceled(cancel, timer); QThread::msleep(20); }
        checkCanceled(cancel, timer);
        if (!engine) engine.reset(new Engine);
        checkCanceled(cancel, timer);
        const auto boxes = detect(*engine, image, cancel, timer);
        QRect previous;
        for (const auto &box : boxes) {
            checkCanceled(cancel, timer);
            const int width = qMax(32, qCeil(48.0 * box.width() / box.height()));
            if (width > 4096) throw QStringLiteral("单行文字过长，请分区域识别。");
            auto output = infer(engine->recognizer, engine->recInput, engine->recOutput, image.copy(box), width, 48);
            const auto shape = output.GetTensorTypeAndShapeInfo().GetShape();
            if (shape.size() != 3 || shape[0] != 1 || shape[2] != engine->characters.size())
                throw QStringLiteral("识别模型与字典不匹配。");
            const auto data = output.GetTensorData<float>();
            QString text;
            int previousIndex = -1, count = 0;
            double score = 0;
            for (int64_t t = 0; t < shape[1]; ++t) {
                const auto row = data + t * shape[2];
                const int index = int(std::max_element(row, row + shape[2]) - row);
                if (index && index != previousIndex) { text += engine->characters[index]; score += row[index]; ++count; }
                previousIndex = index;
            }
            if (!count || score / count < 0.5 || text.trimmed().isEmpty()) continue;
            if (!result.text.isEmpty()) {
                const bool sameRow = qAbs(box.center().y() - previous.center().y())
                    < qMin(box.height(), previous.height()) / 2;
                result.text += sameRow ? QLatin1Char(' ') : QLatin1Char('\n');
            }
            result.text += text.trimmed();
            previous = box;
        }
        checkCanceled(cancel, timer);
    } catch (const QString &message) {
        result.error = message;
        result.text.clear();
    } catch (const Ort::Exception &error) {
        result.error = QStringLiteral("离线 OCR 运行失败：") + QString::fromUtf8(error.what());
        result.text.clear();
    } catch (const std::bad_alloc &) {
        result.error = QStringLiteral("内存不足，请缩小识别区域。");
        result.text.clear();
    }
    return result;
}
