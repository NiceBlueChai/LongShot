#include "ocr.h"
#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QElapsedTimer>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QThread>
#include <QVBoxLayout>
#include <memory>
#include <roapi.h>
#include <wrl.h>
#include <wrl/wrappers/corewrappers.h>
#include <windows.media.ocr.h>
#include <windows.graphics.imaging.h>
#include <robuffer.h>

using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Wrappers::HString;
using Microsoft::WRL::Wrappers::HStringReference;
namespace {
void check(HRESULT hr) { if (FAILED(hr)) throw hr; }
QString stringValue(const HString &value)
{
    UINT32 size = 0;
    const auto data = value.GetRawBuffer(&size);
    return QString::fromWCharArray(data, int(size));
}
}

Ocr::Result Ocr::recognize(const QImage &input, const QString &language, const std::atomic_bool &cancel)
{
    Result result;
    if (input.isNull()) { result.error = QStringLiteral("没有可识别的图片，请先框选区域。"); return result; }
    if (cancel) return result;
    Microsoft::WRL::Wrappers::RoInitializeWrapper apartment(RO_INIT_MULTITHREADED);
    try {
        check(apartment);
        using namespace ABI::Windows::Media::Ocr;
        using namespace ABI::Windows::Graphics::Imaging;
        using namespace ABI::Windows::Storage::Streams;
        ComPtr<IOcrEngineStatics> factory;
        check(RoGetActivationFactory(HStringReference(RuntimeClass_Windows_Media_Ocr_OcrEngine).Get(),
                                     IID_PPV_ARGS(&factory)));
        UINT32 maximum = 0;
        check(factory->get_MaxImageDimension(&maximum));
        if (UINT32(input.width()) > maximum || UINT32(input.height()) > maximum) {
            result.error = QStringLiteral("Windows OCR 单边最多支持 %1 像素，请裁剪后识别。").arg(maximum);
            return result;
        }
        ComPtr<IOcrEngine> engine;
        if (language.isEmpty()) check(factory->TryCreateFromUserProfileLanguages(&engine));
        else {
            ComPtr<ABI::Windows::Globalization::ILanguageFactory> languages;
            check(RoGetActivationFactory(HStringReference(RuntimeClass_Windows_Globalization_Language).Get(),
                                         IID_PPV_ARGS(&languages)));
            ComPtr<ABI::Windows::Globalization::ILanguage> selected;
            HString tag;
            check(tag.Set(reinterpret_cast<const wchar_t *>(language.utf16()), language.size()));
            check(languages->CreateLanguage(tag.Get(), &selected));
            check(factory->TryCreateFromLanguage(selected.Get(), &engine));
        }
        if (!engine) {
            result.error = QStringLiteral("未安装对应的 Windows OCR 语言组件。请在 Windows 设置的语言选项中安装，或切换识别语言。");
            return result;
        }
        ComPtr<ABI::Windows::Globalization::ILanguage> selected;
        check(engine->get_RecognizerLanguage(&selected));
        HString tag;
        check(selected->get_LanguageTag(tag.GetAddressOf()));
        result.language = stringValue(tag);
        const auto image = input.convertToFormat(QImage::Format_RGB32);
        if (image.isNull()) { result.error = QStringLiteral("图片转换失败，内存不足。"); return result; }
        const auto bytes = image.sizeInBytes();
        if (bytes > UINT_MAX) { result.error = QStringLiteral("图片过大，请缩小选区。"); return result; }
        ComPtr<IBufferFactory> buffers;
        check(RoGetActivationFactory(HStringReference(RuntimeClass_Windows_Storage_Streams_Buffer).Get(),
                                     IID_PPV_ARGS(&buffers)));
        ComPtr<IBuffer> buffer;
        check(buffers->Create(UINT32(bytes), &buffer));
        check(buffer->put_Length(UINT32(bytes)));
        ComPtr<Windows::Storage::Streams::IBufferByteAccess> access;
        check(buffer.As(&access));
        byte *data = nullptr;
        check(access->Buffer(&data));
        memcpy(data, image.constBits(), size_t(bytes));
        ComPtr<ISoftwareBitmapStatics> bitmaps;
        check(RoGetActivationFactory(HStringReference(RuntimeClass_Windows_Graphics_Imaging_SoftwareBitmap).Get(),
                                     IID_PPV_ARGS(&bitmaps)));
        ComPtr<ISoftwareBitmap> bitmap;
        check(bitmaps->CreateCopyWithAlphaFromBuffer(buffer.Get(), BitmapPixelFormat_Bgra8,
              image.width(), image.height(), BitmapAlphaMode_Ignore, &bitmap));
        ComPtr<ABI::Windows::Foundation::IAsyncOperation<OcrResult *>> operation;
        check(engine->RecognizeAsync(bitmap.Get(), &operation));
        ComPtr<ABI::Windows::Foundation::IAsyncInfo> info;
        check(operation.As(&info));
        QElapsedTimer timer;
        timer.start();
        for (;;) {
            if (cancel || timer.elapsed() > 30000) {
                info->Cancel();
                if (!cancel) result.error = QStringLiteral("识别超时，请缩小区域后重试。");
                return result;
            }
            ABI::Windows::Foundation::AsyncStatus status;
            check(info->get_Status(&status));
            if (status != ABI::Windows::Foundation::AsyncStatus::Started) break;
            QThread::msleep(20);
        }
        ComPtr<IOcrResult> recognized;
        check(operation->GetResults(&recognized));
        ComPtr<ABI::Windows::Foundation::Collections::IVectorView<OcrLine *>> lines;
        check(recognized->get_Lines(&lines));
        UINT32 count = 0;
        check(lines->get_Size(&count));
        for (UINT32 i = 0; i < count; ++i) {
            ComPtr<IOcrLine> line;
            check(lines->GetAt(i, &line));
            HString text;
            check(line->get_Text(text.GetAddressOf()));
            if (i) result.text += '\n';
            auto lineText = stringValue(text);
            lineText.replace(QRegularExpression(QStringLiteral("(?<=\\p{Han})[ \\t]+(?=\\p{Han})")), QString());
            result.text += lineText;
        }
    } catch (HRESULT hr) {
        result.error = QStringLiteral("Windows OCR 调用失败（0x%1）。此免安装版本在部分系统上可能不可用，请检查系统和 OCR 语言组件。")
            .arg(quint32(hr), 8, 16, QLatin1Char('0'));
    } catch (const std::bad_alloc &) {
        result.error = QStringLiteral("内存不足，请缩小识别区域。");
    }
    return result;
}

void Ocr::showDialog(QWidget *parent, const QImage &image)
{
    QDialog dialog(parent, Qt::Dialog | Qt::WindowStaysOnTopHint);
    dialog.setWindowTitle(QStringLiteral("识别文字 · Windows 离线 OCR"));
    dialog.resize(600, 420);
    auto layout = new QVBoxLayout(&dialog);
    auto language = new QComboBox(&dialog);
    language->addItem(QStringLiteral("系统首选语言"), QString());
    language->addItem(QStringLiteral("简体中文"), QStringLiteral("zh-Hans"));
    language->addItem(QStringLiteral("English"), QStringLiteral("en-US"));
    layout->addWidget(language);
    auto status = new QLabel(QStringLiteral("正在识别…"), &dialog);
    status->setWordWrap(true);
    layout->addWidget(status);
    auto text = new QPlainTextEdit(&dialog);
    text->setPlaceholderText(QStringLiteral("识别结果可在此修改；点击复制文字写入剪贴板。"));
    layout->addWidget(text);
    auto buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    buttons->button(QDialogButtonBox::Close)->setText(QStringLiteral("关闭"));
    auto retry = buttons->addButton(QStringLiteral("重新识别"), QDialogButtonBox::ActionRole);
    auto copy = buttons->addButton(QStringLiteral("复制文字"), QDialogButtonBox::ActionRole);
    copy->setEnabled(false);
    layout->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    QObject::connect(copy, &QPushButton::clicked, &dialog, [=] {
        QApplication::clipboard()->setText(text->toPlainText());
        status->setText(QStringLiteral("文字已复制；关闭窗口可继续编辑截图。"));
    });
    QObject::connect(text, &QPlainTextEdit::textChanged, &dialog, [=] {
        copy->setEnabled(!text->toPlainText().trimmed().isEmpty());
    });
    auto canceled = std::make_shared<std::atomic_bool>(false);
    auto run = [&] {
        retry->setEnabled(false);
        text->setReadOnly(true);
        language->setEnabled(false);
        status->setText(QStringLiteral("正在离线识别…关闭窗口可取消。"));
        auto result = std::make_shared<Result>();
        const auto tag = language->currentData().toString();
        auto worker = QThread::create([image, tag, canceled, result] {
            *result = recognize(image, tag, *canceled);
        });
        QObject::connect(worker, &QThread::finished, &dialog, [=] {
            retry->setEnabled(true);
            text->setReadOnly(false);
            language->setEnabled(true);
            if (!result->error.isEmpty()) status->setText(result->error);
            else {
                text->setPlainText(result->text);
                status->setText(result->text.trimmed().isEmpty() ? QStringLiteral("未识别到文字，可切换语言后重试。")
                    : QStringLiteral("识别完成（%1），可修改后复制。图片未上传。").arg(result->language));
            }
        });
        QObject::connect(worker, &QThread::finished, worker, &QObject::deleteLater);
        worker->start();
    };
    QObject::connect(retry, &QPushButton::clicked, &dialog, run);
    run();
    dialog.exec();
    *canceled = true;
}
