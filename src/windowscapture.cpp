#include "windowscapture.h"
#include <QFileIconProvider>
#include <QFileInfo>
#include <QElapsedTimer>
#include <QBitArray>
#include <objbase.h>
#include <oleacc.h>
#include <UIAutomation.h>
#include <dwmapi.h>
#include <wrl/client.h>
#include <algorithm>

namespace WindowsCapture {
QVector<WindowCandidate> candidates(const QRect &selection)
{
    struct Search { QRect selection; QRegion covered; QVector<WindowCandidate> found; } search { selection, {}, {} };
    EnumWindows([](HWND window, LPARAM data) -> BOOL {
        auto &search = *reinterpret_cast<Search *>(data);
        DWORD process = 0;
        GetWindowThreadProcessId(window, &process);
        if (process == GetCurrentProcessId() || !IsWindowVisible(window) || IsIconic(window)) return TRUE;
        wchar_t name[128] = {};
        GetClassNameW(window, name, 128);
        const QString type = QString::fromWCharArray(name);
        const bool taskbar = type == "Shell_TrayWnd" || type == "Shell_SecondaryTrayWnd";
        if (type == "Progman" || type == "WorkerW" || (type.startsWith("Shell_") && !taskbar)) return TRUE;
        DWORD cloaked = 0;
        DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
        if (cloaked) return TRUE;
        RECT native = {};
        if (!GetWindowRect(window, &native)) return TRUE;
        const QRect bounds(native.left, native.top, native.right - native.left, native.bottom - native.top);
        const QRect overlap = bounds.intersected(search.selection);
        if (overlap.isEmpty()) return TRUE;
        const QRegion visible = QRegion(overlap).subtracted(search.covered);
        search.covered += overlap;
        if (visible.isEmpty()) return TRUE;
        wchar_t title[512] = {};
        GetWindowTextW(window, title, 512);
        RECT client = {};
        if (!GetClientRect(window, &client)) return TRUE;
        POINT origin = {};
        if (!ClientToScreen(window, &origin)) return TRUE;
        WindowCandidate candidate;
        candidate.window = window;
        candidate.process = process;
        candidate.title = QString::fromWCharArray(title);
        if (candidate.title.isEmpty()) candidate.title = type;
        candidate.bounds = bounds;
        candidate.client = QRect(origin.x, origin.y, client.right, client.bottom);
        candidate.visible = visible;
        search.found.append(candidate);
        return TRUE;
    }, reinterpret_cast<LPARAM>(&search));
    QFileIconProvider provider;
    for (auto &candidate : search.found) {
        candidate.icon = provider.icon(QFileIconProvider::File);
        HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, candidate.process);
        if (!process) continue;
        wchar_t path[32768];
        DWORD size = 32768;
        if (QueryFullProcessImageNameW(process, 0, path, &size))
            candidate.icon = provider.icon(QFileInfo(QString::fromWCharArray(path, int(size))));
        CloseHandle(process);
    }
    return search.found;
}

bool validTarget(HWND window, DWORD process, const QRect &region)
{
    DWORD actual = 0;
    GetWindowThreadProcessId(window, &actual);
    RECT native = {};
    POINT origin = {};
    return IsWindow(window) && IsWindowVisible(window) && !IsIconic(window) && actual == process
        && GetClientRect(window, &native) && ClientToScreen(window, &origin)
        && !region.isEmpty() && QRect(origin.x, origin.y, native.right, native.bottom).contains(region);
}

QRect scrollRegion(HWND window, QPoint point)
{
    using Microsoft::WRL::ComPtr;
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return {};
    QRect result;
    {
        ComPtr<IUIAutomation> automation;
        ComPtr<IUIAutomationElement> element;
        ComPtr<IUIAutomationTreeWalker> walker;
        DWORD process = 0;
        GetWindowThreadProcessId(window, &process);
        if (SUCCEEDED(CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
                                       IID_PPV_ARGS(&automation)))
            && SUCCEEDED(automation->ElementFromHandle(window, &element))
            && SUCCEEDED(automation->get_ControlViewWalker(&walker))) {
            for (int depth = 0; element && depth < 64; ++depth) {
                int owner = 0;
                if (FAILED(element->get_CurrentProcessId(&owner)) || DWORD(owner) != process) break;
                ComPtr<IUIAutomationScrollPattern> scroll;
                BOOL vertical = FALSE;
                if (SUCCEEDED(element->GetCurrentPatternAs(UIA_ScrollPatternId, IID_PPV_ARGS(&scroll)))
                    && SUCCEEDED(scroll->get_CurrentVerticallyScrollable(&vertical)) && vertical) {
                    RECT bounds = {};
                    if (SUCCEEDED(element->get_CurrentBoundingRectangle(&bounds))) {
                        result = QRect(bounds.left, bounds.top, bounds.right - bounds.left, bounds.bottom - bounds.top);
                    }
                }
                ComPtr<IUIAutomationElement> child;
                if (FAILED(walker->GetFirstChildElement(element.Get(), &child))) break;
                ComPtr<IUIAutomationElement> hit;
                for (int sibling = 0; child && sibling < 256; ++sibling) {
                    RECT bounds = {};
                    if (SUCCEEDED(child->get_CurrentBoundingRectangle(&bounds))
                        && PtInRect(&bounds, POINT { point.x(), point.y() })) { hit = child; break; }
                    ComPtr<IUIAutomationElement> next;
                    if (FAILED(walker->GetNextSiblingElement(child.Get(), &next))) break;
                    child = next;
                }
                element = hit;
            }
        }
    }
    CoUninitialize();
    return result;
}

QVector<QRect> refineElementRegions(const QVector<QRect> &candidates, QRect window, QPoint point, QRect current)
{
    QVector<QRect> valid;
    for (const auto &area : candidates)
        if (area.width() >= 2 && area.height() >= 2 && window.contains(area) && area.contains(point)
            && !valid.contains(area)) valid.append(area);
    const bool retain = current.contains(point) && window.contains(current);
    if (retain && !valid.contains(current)) valid.append(current);
    std::sort(valid.begin(), valid.end(), [](const QRect &a, const QRect &b) {
        const qint64 aa = qint64(a.width()) * a.height(), bb = qint64(b.width()) * b.height();
        if (aa != bb) return aa < bb;
        if (a.top() != b.top()) return a.top() < b.top();
        if (a.left() != b.left()) return a.left() < b.left();
        return a.width() < b.width();
    });
    QRect leaf = retain ? current : window;
    for (const auto &area : valid) {
        if (leaf.contains(area)) { leaf = area; break; }
    }
    QVector<QRect> result { leaf };
    for (const auto &area : valid)
        if (area != result.first() && area.contains(result.first())) result.prepend(area);
    if (result.first() != window) result.prepend(window);
    return result;
}

QVector<QRect> pixelRegions(const QImage &image, QPoint point)
{
    QVector<QRect> result;
    if (image.isNull() || !image.rect().contains(point)) return result;
    // Bounded local analysis of the unmodified screen image; never sample the overlay.
    const QRect search = QRect(point - QPoint(700, 400), QSize(1401, 801)).intersected(image.rect());
    const auto patch = image.copy(search).convertToFormat(QImage::Format_RGB32);
    const QPoint local = point - search.topLeft();
    const int width = patch.width(), height = patch.height();
    QBitArray visited(width * height);
    QVector<int> queue;
    queue.reserve(32768);
    QElapsedTimer budget;
    budget.start();
    auto colorAt = [&](int x, int y) { return reinterpret_cast<const QRgb *>(patch.constScanLine(y))[x]; };
    auto distance = [](QRgb a, QRgb b) {
        return qMax(qAbs(qRed(a) - qRed(b)), qMax(qAbs(qGreen(a) - qGreen(b)), qAbs(qBlue(a) - qBlue(b))));
    };
    for (const QPoint offset : { QPoint(0, 0), QPoint(-8, -8), QPoint(8, -8), QPoint(-8, 8),
                                QPoint(8, 8), QPoint(-16, 0), QPoint(16, 0), QPoint(0, -12), QPoint(0, 12) }) {
        const QPoint seed = local + offset;
        if (!patch.rect().contains(seed) || visited.testBit(seed.y() * width + seed.x())) continue;
        if (budget.elapsed() > 100) break;
        const QRgb background = colorAt(seed.x(), seed.y());
        queue.clear();
        queue.append(seed.y() * width + seed.x());
        visited.setBit(queue.first());
        int left = seed.x(), right = left, top = seed.y(), bottom = top;
        bool truncated = false;
        for (int head = 0; head < queue.size(); ++head) {
            if (queue.size() > 400000 || ((head & 4095) == 0 && budget.elapsed() > 100)) {
                truncated = true; break;
            }
            const int x = queue[head] % width, y = queue[head] / width;
            left = qMin(left, x); right = qMax(right, x); top = qMin(top, y); bottom = qMax(bottom, y);
            for (const QPoint delta : { QPoint(-1, 0), QPoint(1, 0), QPoint(0, -1), QPoint(0, 1) }) {
                const int nx = x + delta.x(), ny = y + delta.y();
                if (nx < 0 || ny < 0 || nx >= width || ny >= height) continue;
                const int index = ny * width + nx;
                if (!visited.testBit(index) && distance(colorAt(nx, ny), background) <= 5) {
                    visited.setBit(index); queue.append(index);
                }
            }
        }
        const QRect bounds(QPoint(left, top), QPoint(right, bottom));
        if (truncated || left == 0 || top == 0 || right == width - 1 || bottom == height - 1
            || bounds.width() < 24 || bounds.height() < 12 || !bounds.contains(local)
            || queue.size() < qint64(bounds.width()) * bounds.height() * 65 / 100) continue;
        // Require a sustained contrast boundary on each side, excluding rounded corners.
        bool closed = true;
        for (int side = 0; side < 4; ++side) {
            int contrast = 0;
            for (int sample = 2; sample <= 8; ++sample) {
                const int x = left + (right - left) * sample / 10;
                const int y = top + (bottom - top) * sample / 10;
                const QRgb outside = side == 0 ? colorAt(x, top - 1) : side == 1 ? colorAt(x, bottom + 1)
                    : side == 2 ? colorAt(left - 1, y) : colorAt(right + 1, y);
                if (distance(outside, background) >= 8) ++contrast;
            }
            if (contrast < 5) closed = false;
        }
        const QRect candidate = bounds.translated(search.topLeft());
        if (closed && !result.contains(candidate)) result.append(candidate);
    }
    return result;
}

QVector<QRect> elementRegions(HWND window, QPoint point, bool accessibility)
{
    using Microsoft::WRL::ComPtr;
    QVector<QRect> regions;
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return regions;
    {
        QElapsedTimer deadline;
        deadline.start();
        if (accessibility) {
        // Query the target, never the overlay that currently covers the screen.
        RECT native = {};
        const bool valid = IsWindow(window) && GetWindowRect(window, &native);
        const QRect bounds(native.left, native.top, native.right - native.left, native.bottom - native.top);
        auto appendBounds = [&](IAccessible *object, const VARIANT &id) {
            long x = 0, y = 0, width = 0, height = 0;
            if (!object || FAILED(object->accLocation(&x, &y, &width, &height, id))
                || width <= 0 || height <= 0) return;
            const QRect area = QRect(x, y, width, height).intersected(bounds);
            if (area.contains(point) && !regions.contains(area)) regions.append(area);
        };
        if (valid && bounds.contains(point)) {
            regions.append(bounds);
            HWND childWindow = window;
            for (int depth = 0; depth < 32 && deadline.elapsed() < 100; ++depth) {
                POINT local { point.x(), point.y() };
                if (!ScreenToClient(childWindow, &local)) break;
                const HWND next = RealChildWindowFromPoint(childWindow, local);
                if (!next || next == childWindow || !IsWindowVisible(next)) break;
                childWindow = next;
            }
            ComPtr<IAccessible> accessible;
            HRESULT status = AccessibleObjectFromWindow(childWindow, static_cast<DWORD>(OBJID_CLIENT),
                IID_IAccessible, reinterpret_cast<void **>(accessible.GetAddressOf()));
            if (FAILED(status)) {
                accessible.Reset();
                AccessibleObjectFromWindow(window, OBJID_WINDOW, IID_IAccessible,
                    reinterpret_cast<void **>(accessible.GetAddressOf()));
            }
            VARIANT self;
            VariantInit(&self);
            self.vt = VT_I4;
            self.lVal = CHILDID_SELF;
            QVector<ComPtr<IUnknown>> visited;
            auto firstVisit = [&](IAccessible *object) {
                ComPtr<IUnknown> identity;
                if (FAILED(object->QueryInterface(IID_PPV_ARGS(&identity)))) return false;
                for (const auto &previous : visited)
                    if (previous.Get() == identity.Get()) return false;
                visited.append(identity);
                return true;
            };
            ComPtr<IAccessible> deepest;
            for (int depth = 0; accessible && depth < 60 && deadline.elapsed() < 100; ++depth) {
                if (!firstVisit(accessible.Get())) break;
                deepest = accessible;
                appendBounds(accessible.Get(), self);
                VARIANT hit;
                VariantInit(&hit);
                const HRESULT hitStatus = accessible->accHitTest(point.x(), point.y(), &hit);
                ComPtr<IAccessible> next;
                if (SUCCEEDED(hitStatus)) {
                    if (hit.vt == VT_DISPATCH && hit.pdispVal)
                        hit.pdispVal->QueryInterface(IID_PPV_ARGS(&next));
                    else if (hit.vt == VT_I4 && hit.lVal != CHILDID_SELF) {
                        // A simple child has a rectangle even when get_accChild returns no object.
                        appendBounds(accessible.Get(), hit);
                        ComPtr<IDispatch> dispatch;
                        if (SUCCEEDED(accessible->get_accChild(hit, &dispatch)) && dispatch)
                            dispatch->QueryInterface(IID_PPV_ARGS(&next));
                    }
                }
                VariantClear(&hit);
                if (!next) break;
                accessible = next;
            }
            // Hit testing can skip intermediate containers; recover them for wheel selection.
            visited.clear();
            accessible = deepest;
            for (int depth = 0; accessible && depth < 60 && deadline.elapsed() < 150; ++depth) {
                if (!firstVisit(accessible.Get())) break;
                appendBounds(accessible.Get(), self);
                ComPtr<IDispatch> parent;
                if (FAILED(accessible->get_accParent(&parent)) || !parent) break;
                ComPtr<IAccessible> next;
                if (FAILED(parent.As(&next))) break;
                accessible = next;
            }
        }
        } else {
        ComPtr<IUIAutomation> automation;
        ComPtr<IUIAutomationElement> element;
        ComPtr<IUIAutomationTreeWalker> walker;
        if (SUCCEEDED(CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
                                       IID_PPV_ARGS(&automation)))
            && SUCCEEDED(automation->ElementFromHandle(window, &element))
            && SUCCEEDED(automation->get_RawViewWalker(&walker))) {
            for (int depth = 0; element && depth < 48 && deadline.elapsed() < 700; ++depth) {
                RECT bounds = {};
                if (FAILED(element->get_CurrentBoundingRectangle(&bounds))) break;
                const QRect area(bounds.left, bounds.top, bounds.right - bounds.left, bounds.bottom - bounds.top);
                if (!area.contains(point)) break;
                if (regions.isEmpty() || (regions.last().contains(area) && regions.last() != area))
                    regions.append(area);
                CONTROLTYPEID type = 0;
                // A button's image/label is decoration, not a separate screenshot target.
                if (SUCCEEDED(element->get_CurrentControlType(&type)) && type == UIA_ButtonControlTypeId) break;
                ComPtr<IUIAutomationElement> child, hit;
                if (FAILED(walker->GetFirstChildElement(element.Get(), &child))) break;
                for (int i = 0; child && i < 256 && deadline.elapsed() < 700; ++i) {
                    RECT box = {};
                    if (SUCCEEDED(child->get_CurrentBoundingRectangle(&box))
                        && PtInRect(&box, POINT { point.x(), point.y() })) {
                        RECT previous = {};
                        if (!hit || FAILED(hit->get_CurrentBoundingRectangle(&previous))
                            || qint64(box.right - box.left) * (box.bottom - box.top)
                                < qint64(previous.right - previous.left) * (previous.bottom - previous.top)) hit = child;
                    }
                    ComPtr<IUIAutomationElement> next;
                    if (FAILED(walker->GetNextSiblingElement(child.Get(), &next))) break;
                    child = next;
                }
                element = hit;
            }
        }
        }
    }
    CoUninitialize();
    return regions;
}

QRect toLogical(const QRect &physical, const QSize &logical, const QRect &screen)
{
    if (logical.isEmpty() || screen.isEmpty()) return {};
    const qreal sx = qreal(logical.width()) / screen.width();
    const qreal sy = qreal(logical.height()) / screen.height();
    const int x = qRound((physical.x() - screen.x()) * sx);
    const int y = qRound((physical.y() - screen.y()) * sy);
    return QRect(x, y, qRound((physical.x() + physical.width() - screen.x()) * sx) - x,
                 qRound((physical.y() + physical.height() - screen.y()) * sy) - y);
}

QRect screenRect(const QString &screenName)
{
    struct Search { QString name; QRect rect; } search { screenName, {} };
    EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR monitor, HDC, LPRECT, LPARAM data) -> BOOL {
        auto search = reinterpret_cast<Search *>(data);
        MONITORINFOEXW info = {};
        info.cbSize = sizeof(info);
        if (GetMonitorInfoW(monitor, &info) && search->name == QString::fromWCharArray(info.szDevice)) {
            const auto rect = info.rcMonitor;
            search->rect = QRect(rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top);
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&search));
    return search.rect;
}

QRect toPhysical(const QRect &local, const QSize &logical, const QRect &screen)
{
    if (logical.isEmpty() || screen.isEmpty()) return {};
    const qreal sx = qreal(screen.width()) / logical.width();
    const qreal sy = qreal(screen.height()) / logical.height();
    const int left = screen.x() + qRound(local.x() * sx);
    const int top = screen.y() + qRound(local.y() * sy);
    const int right = screen.x() + qRound((local.x() + local.width()) * sx);
    const int bottom = screen.y() + qRound((local.y() + local.height()) * sy);
    return QRect(left, top, right - left, bottom - top);
}

void enableDpiAwareness()
{
    const auto user32 = GetModuleHandleW(L"user32.dll");
    using SetContext = BOOL (WINAPI *)(HANDLE);
    const auto setContext = reinterpret_cast<SetContext>(GetProcAddress(user32, "SetProcessDpiAwarenessContext"));
    if (setContext && setContext(reinterpret_cast<HANDLE>(-4))) return;
    SetProcessDPIAware();
}

QRect monitorRect(QPoint point)
{
    POINT native = { point.x(), point.y() };
    MONITORINFO info = {};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(MonitorFromPoint(native, MONITOR_DEFAULTTONEAREST), &info)) return {};
    return QRect(info.rcMonitor.left, info.rcMonitor.top,
                 info.rcMonitor.right - info.rcMonitor.left, info.rcMonitor.bottom - info.rcMonitor.top);
}

QImage grab(const QRect &rect, QString *error)
{
    if (rect.isEmpty() || qint64(rect.width()) * rect.height() * 4 > 256LL * 1024 * 1024) {
        if (error) *error = QStringLiteral("选区为空或超过内存上限");
        return {};
    }
    HDC screen = GetDC(nullptr);
    HDC memory = screen ? CreateCompatibleDC(screen) : nullptr;
    BITMAPINFO info = {};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = rect.width();
    info.bmiHeader.biHeight = -rect.height();
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void *pixels = nullptr;
    HBITMAP bitmap = memory ? CreateDIBSection(screen, &info, DIB_RGB_COLORS, &pixels, nullptr, 0) : nullptr;
    HGDIOBJ old = bitmap ? SelectObject(memory, bitmap) : nullptr;
    QImage result;
    if (bitmap && old && BitBlt(memory, 0, 0, rect.width(), rect.height(), screen,
                               rect.x(), rect.y(), SRCCOPY | CAPTUREBLT)) {
        GdiFlush();
        result = QImage(static_cast<uchar *>(pixels), rect.width(), rect.height(),
                        QImage::Format_RGB32).copy();
    }
    if (result.isNull() && error) *error = QStringLiteral("屏幕采集失败（Windows 错误 %1）").arg(GetLastError());
    if (old) SelectObject(memory, old);
    if (bitmap) DeleteObject(bitmap);
    if (memory) DeleteDC(memory);
    if (screen) ReleaseDC(nullptr, screen);
    return result;
}

bool scrollDown(int ticks, QString *error)
{
    INPUT input = {};
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = MOUSEEVENTF_WHEEL;
    input.mi.mouseData = DWORD(-WHEEL_DELTA * ticks);
    if (SendInput(1, &input, sizeof(input)) == 1) return true;
    if (error) *error = QStringLiteral("无法发送滚轮事件，请检查目标窗口权限（Windows 错误 %1）")
                        .arg(GetLastError());
    return false;
}
}
