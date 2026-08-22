#ifndef INPUTCOLLECTOR_H
#define INPUTCOLLECTOR_H

#include <functional>
#include <atomic>
#include <thread>
#include <windows.h>
#include "InputEvent.h"

// 客户端输入采集器
//
// 鼠标：使用 Raw Input API 检测鼠标硬件活动，但从光标相对于视频窗口的位置计算绝对坐标
//   - Raw Input 用于检测"鼠标在动"（包括按键/滚轮），不直接使用其增量
//   - 实际发送的坐标：GetCursorPos → ScreenToClient(视频窗口) → 归一化到 0-65535
//   - 绝对坐标映射参考 Moonlight 客户端方案，消除相对位移累积误差
//   - 多显示器：使用服务端发送的 ServerMonitorInfo 将窗口坐标映射到捕获的显示器
//     公式：窗口坐标 → 捕获分辨率 → 加上显示器偏移 → 映射到虚拟桌面 0-65535
//   - 光标仅在视频窗口内时发送事件；窗口外时释放本地光标，不限制用户操作其他软件
//
// 键盘：保持 WH_KEYBOARD_LL 低级键盘钩子（原有方案工作正常）
//
// 内部创建独立线程 + 隐藏消息窗口（HWND_MESSAGE）运行消息循环
// Raw Input 的 WM_INPUT 消息和键盘钩子回调都在该线程中处理
class InputCollector
{
public:
    InputCollector();                                                                               // 构造
    ~InputCollector();                                                                              // 析构

    bool Start();                                                                                   // 启动采集
    void Stop();                                                                                    // 停止采集

    bool IsActive() const { return active_; }                                                       // 是否正在采集

    // 设置视频窗口句柄（用于计算光标在窗口内的相对位置）
    void SetVideoWidget(HWND hwnd);                                                                 // 设置视频窗口

    // 设置服务端显示器信息（用于多显示器坐标映射）
    void SetMonitorInfo(const ServerMonitorInfo& info);                                             // 设置显示器信息

    // 获取光标在视频窗口内的绝对坐标（0-65535），返回 true 表示光标在窗口内
    // StreamWindow 用于重连时发送初始光标位置
    bool GetCursorAbsPos(uint16_t& out_x, uint16_t& out_y);                                         // 获取光标绝对坐标

    // 采集到事件时的回调（在采集线程中调用，注意线程安全）
    std::function<void(const InputMessage&)> OnInputEvent;                                          // 事件回调

private:
    // 采集线程主函数
    void CollectorThread(HANDLE ready_event);                                                       // 采集线程

    // 键盘钩子回调（static，通过 instance_ 访问实例）
    static LRESULT CALLBACK KeyboardProc(int nCode, WPARAM wParam, LPARAM lParam);                  // 键盘钩子

    // 隐藏窗口过程（处理 WM_INPUT）
    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);          // 窗口过程

    // 处理 Raw Input 鼠标数据
    void HandleRawMouse(const RAWMOUSE& rm);                                                        // Raw Input 鼠标处理

    // 派发事件到回调
    void DispatchEvent(const InputMessage& msg);                                                    // 派发事件

    // ---- 线程 ----
    std::thread collector_thread_;                                                                  // 采集线程
    DWORD collector_thread_id_{0};                                                                  // 采集线程 ID（用于 PostThreadMessage）

    // ---- 钩子 ----
    HHOOK keyboard_hook_{nullptr};                                                                  // 键盘钩子

    // ---- 隐藏窗口 ----
    HWND raw_input_hwnd_{nullptr};                                                                  // Raw Input 接收窗口

    // ---- 视频窗口 ----
    HWND video_hwnd_{nullptr};                                                                      // 视频窗口句柄（用于坐标映射）

    // ---- 服务端显示器信息 ----
    ServerMonitorInfo monitor_info_{};                                                              // 服务端显示器信息
    bool has_monitor_info_{false};                                                                  // 是否已收到显示器信息

    // ---- 状态 ----
    std::atomic<bool> active_{false};                                                               // 是否正在采集
    static InputCollector* instance_;                                                               // 单例指针（钩子回调需要访问实例）
};

#endif // INPUTCOLLECTOR_H
