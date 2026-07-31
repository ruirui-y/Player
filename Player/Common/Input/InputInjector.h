#ifndef INPUTINJECTOR_H
#define INPUTINJECTOR_H

#include <cstdint>

struct InputMessage;

// 服务端输入注入器
// 接收 InputMessage，通过 SendInput() API 注入到系统输入流
// 参考 Sunshine src/platform/windows/input.cpp 实现：
// - 键盘使用 Scancode 注入（游戏兼容性更好）
// - SendInput 失败时自动切换输入桌面（UAC 兼容）
// - 支持扩展键、侧键、水平滚轮
class InputInjector
{
public:
    InputInjector();                                                    // 构造
    ~InputInjector();                                                   // 析构

    // 处理一条输入消息（根据 type 字段分发到对应注入方法）
    void Inject(const InputMessage& msg);                               // 注入

private:
    void InjectKeyboard(uint16_t vk_code, bool down);                   // 键盘注入（Scancode）
    void InjectMouseMove(int16_t dx, int16_t dy);                       // 鼠标移动（相对位移）
    void InjectMouseMoveAbs(uint16_t x, uint16_t y);                    // 鼠标移动（绝对坐标 0-65535）
    void InjectMouseButton(uint8_t button, bool down);                  // 鼠标按键注入
    void InjectMouseWheel(int16_t delta);                               // 垂直滚轮
    void InjectMouseHWheel(int16_t delta);                              // 水平滚轮
};

#endif // INPUTINJECTOR_H
