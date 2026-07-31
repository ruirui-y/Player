#include "InputInjector.h"
#include "InputEvent.h"
#include "Common/LogManager.h"

#include <windows.h>
#include <array>
#include <cstdint>
#include <QDebug>

// ================================================================
// ---- VK → Scancode 映射表（美式英文布局 00000409） ----
// 抄自 Sunshine src/platform/windows/keylayout.h
// 游戏通常只认 Scancode，发送 Scancode 比 VK 码兼容性更好
// ================================================================
namespace
{
    constexpr std::array<uint8_t, 256> VK_TO_SCANCODE_MAP =
    {
        0, 0, 0, 70, 0, 0, 0, 0,           // 0x00-0x07
        14, 15, 0, 0, 76, 28, 0, 0,         // 0x08-0x0F
        42, 29, 56, 0, 58, 0, 0, 0,         // 0x10-0x17
        0, 0, 0, 1, 0, 0, 0, 0,             // 0x18-0x1F
        57, 73, 81, 79, 71, 75, 72, 77,     // 0x20-0x27
        80, 0, 0, 0, 84, 82, 83, 99,        // 0x28-0x2F
        11, 2, 3, 4, 5, 6, 7, 8,            // 0x30-0x37
        9, 10, 0, 0, 0, 0, 0, 0,            // 0x38-0x3F
        0, 30, 48, 46, 32, 18, 33, 34,      // 0x40-0x47
        35, 23, 36, 37, 38, 50, 49, 24,     // 0x48-0x4F
        25, 16, 19, 31, 20, 22, 47, 17,     // 0x50-0x57
        45, 21, 44, 91, 92, 93, 0, 95,      // 0x58-0x5F
        82, 79, 80, 81, 75, 76, 77, 71,     // 0x60-0x67
        72, 73, 55, 78, 0, 74, 83, 53,      // 0x68-0x6F
        59, 60, 61, 62, 63, 64, 65, 66,     // 0x70-0x77
        67, 68, 87, 88, 100, 101, 102, 103, // 0x78-0x7F
        104, 105, 106, 107, 108, 109, 110, 118, // 0x80-0x87
        0, 0, 0, 0, 0, 0, 0, 0,             // 0x88-0x8F
        69, 70, 0, 0, 0, 0, 0, 0,           // 0x90-0x97
        0, 0, 0, 0, 0, 0, 0, 0,             // 0x98-0x9F
        42, 54, 29, 29, 56, 56, 106, 105,   // 0xA0-0xA7
        103, 104, 101, 102, 50, 32, 46, 48, // 0xA8-0xAF
        25, 16, 36, 34, 108, 109, 107, 33,  // 0xB0-0xB7
        0, 0, 39, 13, 51, 12, 52, 53,       // 0xB8-0xBF
        41, 115, 126, 0, 0, 0, 0, 0,        // 0xC0-0xC7
        0, 0, 0, 0, 0, 0, 0, 0,             // 0xC8-0xCF
        0, 0, 0, 0, 0, 0, 0, 0,             // 0xD0-0xD7
        0, 0, 0, 26, 43, 27, 40, 0,         // 0xD8-0xDF
        0, 0, 86, 0, 0, 0, 0, 0,            // 0xE0-0xE7
        0, 113, 92, 123, 0, 111, 90, 0,     // 0xE8-0xEF
        0, 91, 0, 95, 0, 94, 0, 0,          // 0xF0-0xF7
        0, 93, 0, 98, 0, 0, 0, 0            // 0xF8-0xFF
    };

    // ---- 线程本地缓存：上次已知的输入桌面句柄 ----
    thread_local HDESK last_known_desktop_ = nullptr;

    // ---- 切换到当前输入桌面（UAC 兼容） ----
    // 当 UAC 弹窗或锁屏时，系统切换到 Winlogon 桌面
    // 此时 SendInput 会失败，需要先 SetThreadDesktop 到当前输入桌面
    // 抄自 Sunshine misc.cpp syncThreadDesktop()
    HDESK SyncThreadDesktop()
    {
        HDESK hdesk = OpenInputDesktop(DF_ALLOWOTHERACCOUNTHOOK, FALSE, GENERIC_ALL);
        if (!hdesk)
        {
            LogManager::Log("ERR", "[InputInjector] OpenInputDesktop 失败: 0x%08X",
                            static_cast<unsigned>(GetLastError()));
            return nullptr;
        }

        if (!SetThreadDesktop(hdesk))
        {
            LogManager::Log("ERR", "[InputInjector] SetThreadDesktop 失败: 0x%08X",
                            static_cast<unsigned>(GetLastError()));
        }

        CloseDesktop(hdesk);
        return hdesk;
    }

    // ---- SendInput 封装（含 UAC 桌面切换重试） ----
    // 抄自 Sunshine input.cpp send_input()
    // 失败时切换桌面后重试一次
    void SendInputWithRetry(INPUT& input)
    {
    retry:
        if (SendInput(1, &input, sizeof(INPUT)) != 1)
        {
            HDESK hdesk = SyncThreadDesktop();
            if (last_known_desktop_ != hdesk)
            {
                last_known_desktop_ = hdesk;
                goto retry;                                          // 桌面已切换，重试
            }
            LogManager::Log("ERR", "[InputInjector] SendInput 失败: GetLastError=%d",
                            static_cast<int>(GetLastError()));
        }
    }
} // anonymous namespace

// ==========
// ---- 构造 / 析构 ----
// ==========
InputInjector::InputInjector()
{
}

InputInjector::~InputInjector()
{
}

// ---- 处理一条输入消息 ----
void InputInjector::Inject(const InputMessage& msg)
{
    switch (static_cast<InputEventType>(msg.type))
    {
    case InputEventType::KeyPress:
        InjectKeyboard(msg.key_code, true);
        break;

    case InputEventType::KeyRelease:
        InjectKeyboard(msg.key_code, false);
        break;

    case InputEventType::MouseMove:
        InjectMouseMove(msg.dx, msg.dy);
        break;

    case InputEventType::MouseMoveAbs:
        // dx/dy 存的是 uint16_t 的位模式（0-65535），reinterpret_cast 读回
        InjectMouseMoveAbs(static_cast<uint16_t>(msg.dx), static_cast<uint16_t>(msg.dy));
        break;

    case InputEventType::MouseBtnDown:
        InjectMouseButton(msg.button, true);
        break;

    case InputEventType::MouseBtnUp:
        InjectMouseButton(msg.button, false);
        break;

    case InputEventType::MouseWheel:
        InjectMouseWheel(msg.wheel_delta);
        break;

    case InputEventType::MouseHWheel:
        InjectMouseHWheel(msg.wheel_delta);
        break;

    default:
        LogManager::Log("WARN", "[InputInjector] 未知事件类型: %d", msg.type);
        break;
    }
}

// ================================================================
// ---- 键盘注入（Scancode 优先，抄 Sunshine keyboard()） ----
// ================================================================
void InputInjector::InjectKeyboard(uint16_t vk_code, bool down)
{
    INPUT input{};
    input.type = INPUT_KEYBOARD;
    auto& ki = input.ki;

    // ---- 第一步：VK → Scancode 查表 ----
    // Scancode 对游戏兼容性更好，很多游戏只认 Scancode 不认 VK
    ki.wScan = VK_TO_SCANCODE_MAP[vk_code & 0xFF];

    if (ki.wScan)
    {
        // ---- 有映射 → 用 Scancode 发送 ----
        ki.dwFlags = KEYEVENTF_SCANCODE;
    }
    else
    {
        // ---- 无映射 → 回退到 VK 码 ----
        ki.wVk = vk_code;
    }

    // ---- 第二步：扩展键标记 ----
    // PS/2 协议中这些键需要 Extended 前缀字节
    // 参考 https://docs.microsoft.com/en-us/windows/win32/inputdev/about-keyboard-input
    switch (vk_code)
    {
    case VK_LWIN:
    case VK_RWIN:
    case VK_RMENU:
    case VK_RCONTROL:
    case VK_INSERT:
    case VK_DELETE:
    case VK_HOME:
    case VK_END:
    case VK_PRIOR:
    case VK_NEXT:
    case VK_UP:
    case VK_DOWN:
    case VK_LEFT:
    case VK_RIGHT:
    case VK_DIVIDE:
    case VK_APPS:
        ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
        break;
    default:
        break;
    }

    // ---- 第三步：释放标志 ----
    if (!down)
    {
        ki.dwFlags |= KEYEVENTF_KEYUP;
    }

    SendInputWithRetry(input);
}

// ================================================================
// ---- 鼠标移动（相对位移，抄 Sunshine move_mouse()） ----
// ================================================================
void InputInjector::InjectMouseMove(int16_t dx, int16_t dy)
{
    if (dx == 0 && dy == 0)
        return;

    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = MOUSEEVENTF_MOVE;
    input.mi.dx = dx;
    input.mi.dy = dy;

    SendInputWithRetry(input);
}

// ================================================================
// ---- 鼠标移动（绝对坐标 0-65535，抄 Sunshine abs_mouse()） ----
// MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK：
//   坐标系覆盖整个虚拟桌面（多显示器拼接），而非仅主显示器
//   Sunshine 使用此组合确保多显示器环境下光标映射正确
// 客户端将光标在视频窗口内的位置归一化到 0-65535 后发送
// 为什么是这个方案：
//   - 绝对坐标消除了相对位移的累积误差
//   - VIRTUALDESK 确保多显示器环境下光标落在正确的显示器上
// 底层是什么：
//   - SendInput(MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK) 通过 Win32k.sys 注入
//   - 内核将 0-65535 映射到整个虚拟桌面，而非仅主显示器
// ================================================================
void InputInjector::InjectMouseMoveAbs(uint16_t x, uint16_t y)
{
    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK | MOUSEEVENTF_MOVE;
    input.mi.dx = x;
    input.mi.dy = y;

    SendInputWithRetry(input);
}

// ================================================================
// ---- 鼠标按键注入（支持 5 键，抄 Sunshine button_mouse()） ----
// ================================================================
void InputInjector::InjectMouseButton(uint8_t button, bool down)
{
    INPUT input{};
    input.type = INPUT_MOUSE;

    switch (static_cast<MouseButton>(button))
    {
    case MouseButton::Left:
        input.mi.dwFlags = down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
        break;

    case MouseButton::Right:
        input.mi.dwFlags = down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP;
        break;

    case MouseButton::Middle:
        input.mi.dwFlags = down ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP;
        break;

    case MouseButton::X1:
        input.mi.dwFlags = down ? MOUSEEVENTF_XDOWN : MOUSEEVENTF_XUP;
        input.mi.mouseData = XBUTTON1;
        break;

    case MouseButton::X2:
        input.mi.dwFlags = down ? MOUSEEVENTF_XDOWN : MOUSEEVENTF_XUP;
        input.mi.mouseData = XBUTTON2;
        break;

    default:
        return;
    }

    SendInputWithRetry(input);
}

// ---- 垂直滚轮（抄 Sunshine scroll()） ----
void InputInjector::InjectMouseWheel(int16_t delta)
{
    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = MOUSEEVENTF_WHEEL;
    input.mi.mouseData = static_cast<DWORD>(delta);

    SendInputWithRetry(input);
}

// ---- 水平滚轮（抄 Sunshine hscroll()） ----
void InputInjector::InjectMouseHWheel(int16_t delta)
{
    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = MOUSEEVENTF_HWHEEL;
    input.mi.mouseData = static_cast<DWORD>(delta);

    SendInputWithRetry(input);
}
