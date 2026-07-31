#include "InputCollector.h"

#include <QDebug>
#include <vector>

// 静态单例指针初始化
InputCollector* InputCollector::instance_ = nullptr;

InputCollector::InputCollector()
{
}

InputCollector::~InputCollector()
{
    Stop();
}

// ================================================================
// ---- 启动采集 ----
// ================================================================
bool InputCollector::Start()
{
    if (active_)
        return true;

    // 清理之前可能残留的线程
    if (collector_thread_.joinable())
        collector_thread_.join();

    instance_ = this;

    // ---- 创建就绪事件，确保线程初始化完成后再返回 ----
    HANDLE ready_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ready_event)
    {
        qDebug() << "[InputCollector] CreateEvent 失败:" << GetLastError();
        return false;
    }

    // ---- 启动采集线程 ----
    collector_thread_ = std::thread(&InputCollector::CollectorThread, this, ready_event);

    // ---- 等待线程初始化完成 ----
    WaitForSingleObject(ready_event, INFINITE);
    CloseHandle(ready_event);

    // 检查线程是否初始化成功
    if (!active_)
    {
        if (collector_thread_.joinable())
            collector_thread_.join();
        instance_ = nullptr;
        qDebug() << "[InputCollector] 采集线程初始化失败";
        return false;
    }

    qDebug() << "[InputCollector] 输入采集已启动（Raw Input + 键盘钩子）";
    return true;
}

// ================================================================
// ---- 停止采集 ----
// ================================================================
void InputCollector::Stop()
{
    if (!active_)
        return;

    // ---- 向采集线程发送 WM_QUIT 让消息循环退出 ----
    if (collector_thread_id_)
        PostThreadMessageW(collector_thread_id_, WM_QUIT, 0, 0);

    if (collector_thread_.joinable())
        collector_thread_.join();

    active_ = false;
    instance_ = nullptr;

    qDebug() << "[InputCollector] 输入采集已停止";
}

// 设置视频窗口句柄
void InputCollector::SetVideoWidget(HWND hwnd)
{
    video_hwnd_ = hwnd;
}

// 设置服务端显示器信息（用于多显示器坐标映射）
void InputCollector::SetMonitorInfo(const ServerMonitorInfo& info)
{
    monitor_info_ = info;
    has_monitor_info_ = true;
    qDebug() << "[InputCollector] 接收显示器信息: capture=%dx%d  offset=(%d,%d)  virtual=%dx%d",
        info.capture_width, info.capture_height,
        info.monitor_x, info.monitor_y,
        info.virtual_width, info.virtual_height;
}

// 派发事件到回调
void InputCollector::DispatchEvent(const InputMessage& msg)
{
    if (OnInputEvent)
        OnInputEvent(msg);
}

// 获取光标在视频窗口内的绝对坐标（0-65535 范围）
// 返回 true 表示光标在视频窗口内，坐标有效
// 返回 false 表示光标不在视频窗口内（用户可能在做其他操作，不应发送事件）
// 坐标钳制到窗口边界内（模仿 Moonlight 的 qMin/qMax 钳制逻辑）
//
// 多显示器映射公式（有 ServerMonitorInfo 时）：
//   1. 客户端窗口坐标 → 服务器捕获分辨率坐标
//      server_x = (client_x / widget_w) * capture_w
//      server_y = (client_y / widget_h) * capture_h
//   2. 加上显示器在虚拟桌面中的偏移
//      server_x += monitor_x
//      server_y += monitor_y
//   3. 归一化到 0-65535（基于虚拟桌面尺寸）
//      out_x = (server_x / virtual_w) * 65535
//      out_y = (server_y / virtual_h) * 65535
//
// 为什么是这个方案：
//   MOUSEEVENTF_ABSOLUTE | VIRTUALDESK 把 0-65535 映射到整个虚拟桌面，
//   不是单个显示器。旧的简单归一化只对单显示器有效。
//   多数显器时，捕获的显示器在虚拟桌面中有偏移（monitor_x/monitor_y），
//   必须加上偏移才能让 SendInput 把光标放到正确的显示器上。
// 底层是什么：
//   SendInput(MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK) 通过 win32k.sys
//   将 0-65535 线性映射到 SM_CXVIRTUALSCREEN × SM_CYVIRTUALSCREEN
bool InputCollector::GetCursorAbsPos(uint16_t& out_x, uint16_t& out_y)
{
    if (!video_hwnd_ || !IsWindow(video_hwnd_))
        return false;

    // ---- 第一步：获取光标屏幕坐标 ----
    POINT pt;
    if (!GetCursorPos(&pt))
        return false;

    // ---- 第二步：转换为视频窗口客户区坐标 ----
    if (!ScreenToClient(video_hwnd_, &pt))
        return false;

    // ---- 第三步：获取视频窗口客户区尺寸 ----
    RECT rect;
    if (!GetClientRect(video_hwnd_, &rect))
        return false;

    int width = rect.right - rect.left;
    int height = rect.bottom - rect.top;

    if (width <= 0 || height <= 0)
        return false;

    // ---- 第四步：检查光标是否在窗口内 ----
    if (pt.x < 0 || pt.y < 0 || pt.x > width || pt.y > height)
        return false;                                                // 光标在窗口外，不发送事件

    // ---- 第五步：钳制到窗口内（模仿 Moonlight qMin/qMax） ----
    // 防止浮点误差导致坐标超出 0-65535 范围
    if (pt.x < 0) pt.x = 0;
    if (pt.y < 0) pt.y = 0;
    if (pt.x >= width) pt.x = width - 1;
    if (pt.y >= height) pt.y = height - 1;

    // ---- 第六步：归一化到 0-65535（Windows MOUSEEVENTF_ABSOLUTE 坐标系） ----
    // 有多显示器信息时：窗口坐标 → 捕获分辨率 → 加偏移 → 虚拟桌面归一化
    // 无显示器信息时：回退到简单归一化（单显示器场景）
    if (has_monitor_info_)
    {
        float cap_w = static_cast<float>(monitor_info_.capture_width);
        float cap_h = static_cast<float>(monitor_info_.capture_height);
        float virt_w = static_cast<float>(monitor_info_.virtual_width);
        float virt_h = static_cast<float>(monitor_info_.virtual_height);

        // 客户端窗口坐标 → 服务器捕获分辨率坐标
        float server_x = static_cast<float>(pt.x) / width * cap_w;
        float server_y = static_cast<float>(pt.y) / height * cap_h;

        // 加上显示器在虚拟桌面中的偏移
        server_x += static_cast<float>(monitor_info_.monitor_x);
        server_y += static_cast<float>(monitor_info_.monitor_y);

        // 归一化到 0-65535（基于虚拟桌面尺寸）
        out_x = static_cast<uint16_t>(server_x / virt_w * 65535.0f);
        out_y = static_cast<uint16_t>(server_y / virt_h * 65535.0f);
    }
    else
    {
        // 无显示器信息 → 回退到简单归一化（单显示器场景）
        out_x = static_cast<uint16_t>((static_cast<float>(pt.x) / width) * 65535.0f);
        out_y = static_cast<uint16_t>((static_cast<float>(pt.y) / height) * 65535.0f);
    }

    return true;
}

// ================================================================
// ---- 采集线程主函数 ----
// 在独立线程中：创建隐藏窗口 → 注册 Raw Input → 安装键盘钩子 → 消息循环
// ================================================================
void InputCollector::CollectorThread(HANDLE ready_event)
{
    collector_thread_id_ = GetCurrentThreadId();

    // ---- 第一步：注册窗口类 ----
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"InputCollectorRawInputWnd";
    RegisterClassExW(&wc);

    // ---- 第二步：创建消息-only 窗口（HWND_MESSAGE = 不可见、不接收用户输入） ----
    // 将 this 指针通过 lpParam 传给 WM_CREATE
    raw_input_hwnd_ = CreateWindowExW(
        0, wc.lpszClassName, L"InputCollector",
        0, 0, 0, 0, 0,
        HWND_MESSAGE, nullptr, wc.hInstance, this);

    if (!raw_input_hwnd_)
    {
        qDebug() << "[InputCollector] 隐藏窗口创建失败:" << GetLastError();
        SetEvent(ready_event);
        return;
    }

    // ---- 第三步：注册 Raw Input 鼠标设备 ----
    // RIDEV_INPUTSINK：即使窗口不在前台也接收输入（远程控制必须）
    RAWINPUTDEVICE rid;
    rid.usUsagePage = 0x01;             // HID_USAGE_PAGE_GENERIC
    rid.usUsage = 0x02;                 // HID_USAGE_GENERIC_MOUSE
    rid.dwFlags = RIDEV_INPUTSINK;
    rid.hwndTarget = raw_input_hwnd_;

    if (!RegisterRawInputDevices(&rid, 1, sizeof(rid)))
    {
        qDebug() << "[InputCollector] Raw Input 注册失败:" << GetLastError();
        DestroyWindow(raw_input_hwnd_);
        raw_input_hwnd_ = nullptr;
        SetEvent(ready_event);
        return;
    }

    // ---- 第四步：安装低级键盘钩子 ----
    // 钩子回调运行在本线程的消息循环中
    keyboard_hook_ = SetWindowsHookExW(WH_KEYBOARD_LL, KeyboardProc,
                                       GetModuleHandleW(nullptr), 0);
    if (!keyboard_hook_)
    {
        qDebug() << "[InputCollector] 键盘钩子安装失败:" << GetLastError();
        // 注销 Raw Input
        RAWINPUTDEVICE rid_off = {};
        rid_off.usUsagePage = 0x01;
        rid_off.usUsage = 0x02;
        rid_off.dwFlags = RIDEV_REMOVE;
        rid_off.hwndTarget = nullptr;
        RegisterRawInputDevices(&rid_off, 1, sizeof(rid_off));
        DestroyWindow(raw_input_hwnd_);
        raw_input_hwnd_ = nullptr;
        SetEvent(ready_event);
        return;
    }

    // ---- 第五步：初始化完成，通知主线程 ----
    active_ = true;
    SetEvent(ready_event);

    qDebug() << "[InputCollector] 采集线程已就绪，进入消息循环";

    // ---- 第六步：消息循环 ----
    // GetMessage 在收到 WM_QUIT 时返回 FALSE，退出循环
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    // ---- 第七步：清理 ----
    if (keyboard_hook_)
    {
        UnhookWindowsHookEx(keyboard_hook_);
        keyboard_hook_ = nullptr;
    }

    // 注销 Raw Input
    RAWINPUTDEVICE rid_off = {};
    rid_off.usUsagePage = 0x01;
    rid_off.usUsage = 0x02;
    rid_off.dwFlags = RIDEV_REMOVE;
    rid_off.hwndTarget = nullptr;
    RegisterRawInputDevices(&rid_off, 1, sizeof(rid_off));

    if (raw_input_hwnd_)
    {
        DestroyWindow(raw_input_hwnd_);
        raw_input_hwnd_ = nullptr;
    }

    qDebug() << "[InputCollector] 采集线程已退出";
}

// ================================================================
// ---- 键盘钩子回调 ----
// 与原实现相同，WH_KEYBOARD_LL 在本线程消息循环中被调用
// ================================================================
LRESULT CALLBACK InputCollector::KeyboardProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode == HC_ACTION && instance_ && instance_->active_)
    {
        KBDLLHOOKSTRUCT* kb = reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam);

        bool down = false;

        // ---- 判断按下还是释放 ----
        switch (wParam)
        {
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            down = true;
            break;
        case WM_KEYUP:
        case WM_SYSKEYUP:
            down = false;
            break;
        default:
            goto pass;                                                  // 其他事件不处理
        }

        // ---- 构造键盘事件并发送 ----
        InputMessage msg = MakeKeyEvent(static_cast<uint16_t>(kb->vkCode), down);
        instance_->DispatchEvent(msg);
    }

pass:
    return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

// ================================================================
// ---- 隐藏窗口过程（处理 WM_INPUT） ----
// ================================================================
LRESULT CALLBACK InputCollector::WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    // ---- WM_CREATE：保存 this 指针到窗口 UserData ----
    if (msg == WM_CREATE)
    {
        auto cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return 0;
    }

    auto* self = reinterpret_cast<InputCollector*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    // ---- WM_INPUT：处理 Raw Input 数据 ----
    if (msg == WM_INPUT && self && self->active_)
    {
        UINT size = 0;

        // 第一次调用获取所需缓冲区大小
        GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam), RID_INPUT,
                        nullptr, &size, sizeof(RAWINPUTHEADER));

        if (size > 0)
        {
            std::vector<BYTE> buf(size);

            // 第二次调用获取实际数据
            UINT written = GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam), RID_INPUT,
                                           buf.data(), &size, sizeof(RAWINPUTHEADER));

            if (written == size)
            {
                auto* raw = reinterpret_cast<RAWINPUT*>(buf.data());

                // ---- 只处理鼠标类型 ----
                if (raw->header.dwType == RIM_TYPEMOUSE)
                {
                    self->HandleRawMouse(raw->data.mouse);
                }
            }
        }

        // 必须调用 DefWindowProc 让系统清理 Raw Input 数据
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ================================================================
// ---- 处理 Raw Input 鼠标数据 ----
// Raw Input 用于检测"鼠标硬件在动"，但实际发送的是绝对坐标
// 从 GetCursorPos + ScreenToClient 获取光标在视频窗口内的位置
// 归一化到 0-65535 后发送（Windows MOUSEEVENTF_ABSOLUTE 坐标系）
// 光标在视频窗口外时，不发送任何事件（用户在做其他操作，不应干扰）
// 参考 Moonlight 客户端的绝对坐标映射方案
// ================================================================
void InputCollector::HandleRawMouse(const RAWMOUSE& rm)
{
    // ---- 第一步：获取光标在视频窗口内的绝对坐标 ----
    // 光标在窗口外 → 跳过所有事件（移动、按键、滚轮都不发送）
    uint16_t abs_x = 0, abs_y = 0;
    bool inside = GetCursorAbsPos(abs_x, abs_y);

    // ---- 第二步：移动（仅光标在窗口内时发送） ----
    // 使用绝对坐标而非 Raw Input 相对增量，消除累积误差
    if (inside && (rm.usFlags & MOUSE_MOVE_ABSOLUTE) == 0)
    {
        // 只检查 Raw Input 是否报告了移动（lLastX/lLastY 非零），
        // 但实际发送的是绝对坐标而非相对增量
        if (rm.lLastX != 0 || rm.lLastY != 0)
        {
            DispatchEvent(MakeMouseMoveAbsEvent(abs_x, abs_y));
        }
    }

    // ---- 第三步：鼠标按键（仅光标在窗口内时发送） ----
    if (!inside)
        return;                                                      // 光标在窗口外，不发送按键/滚轮

    USHORT btn_flags = rm.usButtonFlags;

    if (btn_flags & RI_MOUSE_LEFT_BUTTON_DOWN)
        DispatchEvent(MakeMouseButtonEvent(MouseButton::Left, true));
    if (btn_flags & RI_MOUSE_LEFT_BUTTON_UP)
        DispatchEvent(MakeMouseButtonEvent(MouseButton::Left, false));
    if (btn_flags & RI_MOUSE_RIGHT_BUTTON_DOWN)
        DispatchEvent(MakeMouseButtonEvent(MouseButton::Right, true));
    if (btn_flags & RI_MOUSE_RIGHT_BUTTON_UP)
        DispatchEvent(MakeMouseButtonEvent(MouseButton::Right, false));
    if (btn_flags & RI_MOUSE_MIDDLE_BUTTON_DOWN)
        DispatchEvent(MakeMouseButtonEvent(MouseButton::Middle, true));
    if (btn_flags & RI_MOUSE_MIDDLE_BUTTON_UP)
        DispatchEvent(MakeMouseButtonEvent(MouseButton::Middle, false));
    if (btn_flags & RI_MOUSE_BUTTON_4_DOWN)
        DispatchEvent(MakeMouseButtonEvent(MouseButton::X1, true));
    if (btn_flags & RI_MOUSE_BUTTON_4_UP)
        DispatchEvent(MakeMouseButtonEvent(MouseButton::X1, false));
    if (btn_flags & RI_MOUSE_BUTTON_5_DOWN)
        DispatchEvent(MakeMouseButtonEvent(MouseButton::X2, true));
    if (btn_flags & RI_MOUSE_BUTTON_5_UP)
        DispatchEvent(MakeMouseButtonEvent(MouseButton::X2, false));

    // ---- 第四步：滚轮 ----
    if (btn_flags & RI_MOUSE_WHEEL)
    {
        int16_t delta = static_cast<int16_t>(rm.usButtonData);
        DispatchEvent(MakeMouseWheelEvent(delta));
    }
}
