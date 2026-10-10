#include "version.h"
#include "capturecontroller.h"
#include "editor.h"
#include "windowscapture.h"
#include <QApplication>
#include <QFont>
#include <QMenu>
#include <QMessageBox>
#include <QIcon>
#include <QSystemTrayIcon>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QLabel>
#include <QVBoxLayout>
#include <QSettings>
#include <QTimer>
#include <memory>


class CaptureHotkey : public QAbstractNativeEventFilter
{
public:
    explicit CaptureHotkey(CaptureController *controller) : controller_(controller) {}
    bool nativeEventFilter(const QByteArray &, void *message, long *result) override
    {
        const auto event = static_cast<MSG *>(message);
        if (event->message != WM_HOTKEY || (event->wParam < 0x4c54 || event->wParam > 0x4c56)) return false;
        if (!QApplication::activeModalWidget()) controller_->start();
        if (result) *result = 0;
        return true;
    }
private:
    CaptureController *controller_;
};

int main(int argc, char *argv[])
{
    WindowsCapture::enableDpiAwareness();
    QApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
    QApplication::setAttribute(Qt::AA_UseHighDpiPixmaps);
    QApplication app(argc, argv);
    app.setApplicationVersion(QStringLiteral(LONGSHOT_VERSION));
    app.setApplicationName("LongShot");
    app.setOrganizationName("LongShot");
    app.setFont(QFont(QStringLiteral("Microsoft YaHei"), 10));
    const QIcon icon(QStringLiteral(":/icons/longshot.ico"));
    app.setWindowIcon(icon);
    // Kernel objects disappear on exit, including a crash; no stale lock file is left behind.
    const auto closeHandle = [](void *handle) { if (handle) CloseHandle(handle); };
    std::unique_ptr<void, decltype(closeHandle)> activation(
        CreateEventW(nullptr, FALSE, FALSE, L"Local\\LongShot.Activate.v1"), closeHandle);
    std::unique_ptr<void, decltype(closeHandle)> instance(
        CreateMutexW(nullptr, FALSE, L"Local\\LongShot.Instance.v1"), closeHandle);
    if (!activation || !instance) {
        QMessageBox::critical(nullptr, QStringLiteral("LongShot"),
            QStringLiteral("无法建立单实例启动保护，请检查权限后重试。"));
        return 1;
    }
    const DWORD ownership = WaitForSingleObject(instance.get(), 0);
    if (ownership == WAIT_TIMEOUT) {
        if (!SetEvent(activation.get())) {
            QMessageBox::warning(nullptr, QStringLiteral("LongShot"),
                QStringLiteral("LongShot 已运行，但无法唤起，请使用托盘图标。"));
            return 1;
        }
        if (app.arguments().contains("--edit")) {
            QMessageBox::information(nullptr, QStringLiteral("LongShot"),
                QStringLiteral("已唤起正在运行的 LongShot。为保留当前图片，本次未导入新图片。"));
        }
        return 0;
    }
    if (ownership != WAIT_OBJECT_0 && ownership != WAIT_ABANDONED) {
        QMessageBox::critical(nullptr, QStringLiteral("LongShot"),
            QStringLiteral("无法检查 LongShot 运行状态，请稍后重试。"));
        return 1;
    }
    Editor editor;
    CaptureController controller(&editor);
    QTimer activationTimer;
    QObject::connect(&activationTimer, &QTimer::timeout, &app, [&] {
        if (WaitForSingleObject(activation.get(), 0) != WAIT_OBJECT_0) return;
        if (QWidget *dialog = QApplication::activeModalWidget()) {
            dialog->raise();
            dialog->activateWindow();
        } else if (controller.state() == CaptureController::State::Idle
                   || controller.state() == CaptureController::State::Editing) {
            if (editor.isVisible()) {
                editor.raise();
                editor.activateWindow();
            } else {
                controller.start();
            }
        }
        // An ongoing selection or scrolling capture must not be reset or lose target focus.
    });
    activationTimer.start(150);
    if (app.arguments().size() == 3 && app.arguments()[1] == "--edit") {
        QImage image(app.arguments()[2]);
        if (image.isNull()) return 2;
        editor.show();
        editor.setImage(image);
        return app.exec();
    }
    if (!QSystemTrayIcon::isSystemTrayAvailable()) {
        QMessageBox::critical(nullptr, QStringLiteral("LongShot"), QStringLiteral("系统托盘不可用，无法启动截图工具"));
        return 1;
    }
    app.setQuitOnLastWindowClosed(false);
    QSystemTrayIcon tray{icon};
    QMenu menu;
    auto capture = menu.addAction(QStringLiteral("截图"));
    QObject::connect(capture, &QAction::triggered, &controller, &CaptureController::start);
    auto last = menu.addAction(QStringLiteral("继续编辑上一张"));
    last->setEnabled(false);
    QObject::connect(last, &QAction::triggered, &editor, [&] {
        editor.show(); editor.raise(); editor.activateWindow();
    });
    QObject::connect(&controller, &CaptureController::captureFinished, &tray,
        [&](const QImage &image, const QString &reason) {
            if (!image.isNull()) last->setEnabled(true);
            else tray.showMessage(QStringLiteral("截图未完成"), reason, QSystemTrayIcon::Warning);
        });
    menu.addSeparator();
    QSettings settings;
    const QStringList shortcuts = { "Ctrl+Alt+A", "Ctrl+Alt+S", "Ctrl+Shift+S" };
    const UINT modifiers[] = { MOD_CONTROL | MOD_ALT, MOD_CONTROL | MOD_ALT, MOD_CONTROL | MOD_SHIFT };
    const UINT keys[] = { 'A', 'S', 'S' };
    int activeShortcut = -1;
    auto selectShortcut = [&](int index) {
        if (index != activeShortcut && index >= 0 &&
            !RegisterHotKey(nullptr, 0x4c54 + index, modifiers[index] | MOD_NOREPEAT, keys[index])) {
            return false;
        }
        if (activeShortcut >= 0 && index != activeShortcut)
            UnregisterHotKey(nullptr, 0x4c54 + activeShortcut);
        activeShortcut = index;
        settings.setValue("captureShortcut", index);

        capture->setText(index < 0 ? QStringLiteral("截图") : QStringLiteral("截图\t%1").arg(shortcuts[index]));
        tray.setToolTip(index < 0 ? QStringLiteral("LongShot · 单击截图 · 未启用快捷键")
                                : QStringLiteral("LongShot · 单击截图 · %1").arg(shortcuts[index]));
        return true;
    };
    QObject::connect(menu.addAction(QStringLiteral("设置…")), &QAction::triggered, &tray, [&] {
        QDialog dialog(nullptr, Qt::WindowTitleHint | Qt::WindowCloseButtonHint | Qt::WindowStaysOnTopHint);
        dialog.setWindowTitle(QStringLiteral("LongShot · 设置"));
        dialog.setMinimumWidth(420);
        auto layout = new QVBoxLayout(&dialog);
        layout->setContentsMargins(24, 24, 24, 24);
        layout->setSpacing(16);
        layout->addWidget(new QLabel(QStringLiteral("截图快捷键")));
        auto choice = new QComboBox;
        choice->addItems(shortcuts);
        choice->addItem(QStringLiteral("不使用快捷键"));
        choice->setCurrentIndex(activeShortcut < 0 ? 3 : activeShortcut);
        layout->addWidget(choice);
        auto status = new QLabel(activeShortcut < 0 ? QStringLiteral("当前未启用快捷键，仍可单击托盘截图。")
            : QStringLiteral("当前快捷键：%1").arg(shortcuts[activeShortcut]));
        status->setWordWrap(true);
        layout->addWidget(status);
        auto buttons = new QDialogButtonBox;
        buttons->addButton(QStringLiteral("保存"), QDialogButtonBox::AcceptRole);
        buttons->addButton(QStringLiteral("取消"), QDialogButtonBox::RejectRole);
        layout->addWidget(buttons);
        QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
            const int index = choice->currentIndex() == 3 ? -1 : choice->currentIndex();
            if (selectShortcut(index)) dialog.accept();
            else {
                status->setText(QStringLiteral("该快捷键注册失败，请换一个组合。原设置未修改。"));
                status->setStyleSheet("color: #b42318;");
            }
        });
        dialog.exec();
    });
    auto help = menu.addAction(QStringLiteral("操作说明"));
    QObject::connect(help, &QAction::triggered, &tray, [&] {
        QMessageBox dialog(QMessageBox::Information, QStringLiteral("LongShot · 操作说明"),
            QStringLiteral("1. 单击托盘或截图快捷键，悬停自动预选窗口、控件或像素区域。\n"
                           "滚轮上选父级、下选子级；单击确认，直接拖动手工框选。\n"
                           "预选时 Tab 切换自动／手工模式；识别不准时可手工框选。\n"
                           "2. 在单个显示器内拉框；选区工具下框内移动，拖动蓝框边角调整大小。\n"
                           "放大镜显示原始屏幕像素；C 复制颜色，Shift 切换 RGB/HEX。\n"
                           "3. 框选后直接点矩形、箭头、文字、马赛克标注；撤销／重做可回退。\n"
                           "点“完成”复制并收起；点“保存”导出 PNG。\n\n"
                           "长截图\n点“长截图”，多窗口时选择目标；点“范围”可调整绿色滚动区。\n"
                           "点“开始”后，移出绿色区域暂停，回到区域继续。\n"
                           "Q、完成或点击蓝框外停止并编辑；Esc／取消放弃本次截图。\n"
                           "蓝框保留整体内容，绿色区延长，新增静止区留白。\n\n"
                           "编辑\n选择模式直接拖动查看，其他工具按空格拖动；滚轮缩放。\n"
                           "100% 查看原始尺寸；裁剪、矩形、箭头、马赛克拖动绘制，文字点击输入。\n"
                           "查看长图时普通标注可移动，文字可双击修改；马赛克固定位置。Ctrl+Z/Y 撤销/重做，Ctrl+S 保存，Ctrl+C 复制。\n"
                           "完成后复制并收起；Esc 收起且不主动修改剪贴板。\n"
                           "托盘可继续编辑上一张；退出前请保存，历史不跨启动保留。\n"
                           "导出保持原始像素；识别效果取决于目标程序与画面，不保证所有区域都能自动识别。"), QMessageBox::Ok);
        dialog.setWindowFlag(Qt::WindowStaysOnTopHint);
        dialog.setButtonText(QMessageBox::Ok, QStringLiteral("关闭"));
        dialog.exec();
    });
    QObject::connect(menu.addAction(QStringLiteral("关于 LongShot")), &QAction::triggered, &tray, [&] {
        QMessageBox dialog(QMessageBox::Information, QStringLiteral("关于 LongShot"),
            QStringLiteral("LongShot " LONGSHOT_VERSION "\nWindows 截图工具\n\n"
                           "区域截图、滚动长截图与标注编辑。\n"
                           "基于 Qt %1 · C++ · Win32\n\n"
                           "许可证：GNU GPL v3.0 only（GPL-3.0-only）\n"
                           "完整条款见随附 LICENSE；本软件不提供担保。\n"
                           "第三方组件保留各自许可。\n\n"
                           "图片在本机处理，不上传。\n"
                           "匹配失败的诊断图片保存在本机临时目录。")
                .arg(QString::fromLatin1(qVersion())), QMessageBox::Ok);
        dialog.setWindowFlag(Qt::WindowStaysOnTopHint);
        dialog.setButtonText(QMessageBox::Ok, QStringLiteral("关闭"));
        dialog.exec();
    });
    menu.addSeparator();
    QObject::connect(menu.addAction(QStringLiteral("退出")), &QAction::triggered, &app, &QApplication::quit);
    tray.setContextMenu(&menu);
    tray.setToolTip(QStringLiteral("LongShot · 单击截图 · 未启用快捷键"));
    QObject::connect(&tray, &QSystemTrayIcon::activated, &controller, [&](QSystemTrayIcon::ActivationReason reason) {
        if (reason == QSystemTrayIcon::Trigger && !QApplication::activeModalWidget()) controller.start();
    });
    CaptureHotkey hotkey(&controller);
    app.installNativeEventFilter(&hotkey);
    tray.show();
    if (!selectShortcut(qBound(-1, settings.value("captureShortcut", 0).toInt(), 2)))
        tray.showMessage(QStringLiteral("快捷键不可用"),
            QStringLiteral("请右键托盘打开“设置”更换快捷键，仍可单击托盘截图。"), QSystemTrayIcon::Warning);
    const int result = app.exec();
    if (activeShortcut >= 0) UnregisterHotKey(nullptr, 0x4c54 + activeShortcut);
    app.removeNativeEventFilter(&hotkey);
    return result;
}
