#ifndef INPUTEVENT_H
#define INPUTEVENT_H

#include <cstdint>

// ========== 服务器显示器信息（TCP 连接建立后服务端发送给客户端） ==========
// 客户端用于将视频窗口内的光标位置正确映射到服务器虚拟桌面上的绝对坐标
// 单显示器场景：capture = virtual, monitor_x/y = 0，公式退化为简单归一化
// 多显示器场景：客户端坐标 → 捕获分辨率 → 加上偏移 → 映射到虚拟桌面
#pragma pack(push, 1)
struct ServerMonitorInfo
{
    uint16_t capture_width;                                             // 捕获的显示器分辨率宽度
    uint16_t capture_height;                                            // 捕获的显示器分辨率高度
    int16_t  monitor_x;                                                 // 显示器在虚拟桌面中的 X 偏移
    int16_t  monitor_y;                                                 // 显示器在虚拟桌面中的 Y 偏移
    uint32_t virtual_width;                                             // 虚拟桌面总宽度（所有显示器拼接）
    uint32_t virtual_height;                                            // 虚拟桌面总高度
};
#pragma pack(pop)

static_assert(sizeof(ServerMonitorInfo) == 16, "ServerMonitorInfo 必须为 16 字节");

// ========== 输入事件类型 ==========
enum class InputEventType : uint8_t
{
    KeyPress      = 0x01,                                               // 键盘按下
    KeyRelease    = 0x02,                                               // 键盘释放
    MouseMove     = 0x03,                                               // 鼠标移动（相对位移）
    MouseMoveAbs  = 0x08,                                               // 鼠标移动（绝对坐标 0-65535）
    MouseBtnDown  = 0x04,                                               // 鼠标按键按下
    MouseBtnUp    = 0x05,                                               // 鼠标按键释放
    MouseWheel    = 0x06,                                               // 鼠标垂直滚轮
    MouseHWheel   = 0x07,                                               // 鼠标水平滚轮
};

// ========== 鼠标按键 ==========
enum class MouseButton : uint8_t
{
    Left   = 0,                                                         // 左键
    Right  = 1,                                                         // 右键
    Middle = 2,                                                         // 中键
    X1     = 3,                                                         // 侧键 1（后退）
    X2     = 4,                                                         // 侧键 2（前进）
};

// ========== 输入消息结构（固定 12 字节，TCP 传输用） ==========
// 所有输入事件复用同一个结构体，用 type 字段区分含义
// 未使用的字段由发送端填 0，接收端根据 type 读取对应字段
#pragma pack(push, 1)
struct InputMessage
{
    uint8_t  type;          // InputEventType                            // 事件类型
    uint8_t  button;        // MouseButton（仅鼠标按键事件有效）          // 鼠标按键
    uint16_t key_code;      // 虚拟键码（仅键盘事件有效）                  // VK_xxx
    int16_t  dx;            // 鼠标 X 相对位移（仅 MouseMove 有效）        // 像素
    int16_t  dy;            // 鼠标 Y 相对位移（仅 MouseMove 有效）        // 像素
    int16_t  wheel_delta;   // 滚轮滚动量（仅 MouseWheel/HWheel 有效）    // 标准 120 = 一格
    uint8_t  reserved;      // 保留对齐                                  // 填 0
    uint8_t  reserved2;     // 补齐到 12 字节（4 字节对齐）               // 填 0
};
#pragma pack(pop)

static_assert(sizeof(InputMessage) == 12, "InputMessage 必须为 12 字节");

// ========== 便捷构造函数 ==========

inline InputMessage MakeKeyEvent(uint16_t vk_code, bool down)
{
    InputMessage msg{};
    msg.type = static_cast<uint8_t>(down ? InputEventType::KeyPress : InputEventType::KeyRelease);
    msg.key_code = vk_code;
    return msg;
}

inline InputMessage MakeMouseMoveEvent(int16_t dx, int16_t dy)
{
    InputMessage msg{};
    msg.type = static_cast<uint8_t>(InputEventType::MouseMove);
    msg.dx = dx;
    msg.dy = dy;
    return msg;
}

// 绝对坐标构造：x/y 为 0-65535 范围（Windows MOUSEEVENTF_ABSOLUTE 坐标系）
// 客户端获取光标在视频窗口内的位置，归一化后乘以 65535
inline InputMessage MakeMouseMoveAbsEvent(uint16_t x, uint16_t y)
{
    InputMessage msg{};
    msg.type = static_cast<uint8_t>(InputEventType::MouseMoveAbs);
    // dx/dy 是 int16_t，这里存 uint16_t 的位模式（接收端 reinterpret_cast 读回）
    msg.dx = static_cast<int16_t>(x);
    msg.dy = static_cast<int16_t>(y);
    return msg;
}

inline InputMessage MakeMouseButtonEvent(MouseButton btn, bool down)
{
    InputMessage msg{};
    msg.type = static_cast<uint8_t>(down ? InputEventType::MouseBtnDown : InputEventType::MouseBtnUp);
    msg.button = static_cast<uint8_t>(btn);
    return msg;
}

inline InputMessage MakeMouseWheelEvent(int16_t delta)
{
    InputMessage msg{};
    msg.type = static_cast<uint8_t>(InputEventType::MouseWheel);
    msg.wheel_delta = delta;
    return msg;
}

inline InputMessage MakeMouseHWheelEvent(int16_t delta)
{
    InputMessage msg{};
    msg.type = static_cast<uint8_t>(InputEventType::MouseHWheel);
    msg.wheel_delta = delta;
    return msg;
}

#endif // INPUTEVENT_H
