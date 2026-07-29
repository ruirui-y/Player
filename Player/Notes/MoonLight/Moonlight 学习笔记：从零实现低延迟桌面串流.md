# Moonlight 学习笔记：从零实现低延迟桌面串流（源码阅读后更新版）

> 基于实际阅读 Sunshine v0.23.1 + Moonlight-Qt v4.3.1 源码后的修正路线图

## 整体架构

```
服务端（被控制端）                             客户端（控制端）
┌─────────────────────────┐           ┌─────────────────────────┐
│                         │           │                          │
│  D3D11 桌面捕获          │           │  网络接收器               │
│  (IDXGIOutputDuplication)│          │  (UDP 接收 → NAL 组帧)   │
│         │                │          │         │                │
│         ▼                │   UDP    │         ▼                │
│  NVENC 编码               │ ────────→ │  H.264/H.265 解码        │
│  (nvEncodeAPI 直接调用)   │   RTP    │  (avcodec_send_packet)   │
│         │                │          │         │                │
│         ▼                │          │         ▼                │
│  UDP 发送器               │          │  D3D11 渲染显示          │
│  (分片+序列号+重传)        │          │  (Player 已有的全 GPU    │
│                          │          │    HLSL 管线)            │
│  输入事件注入              │←─────────│  输入事件采集             │
│  (SendInput)             │    TCP   │  (SetWindowsHookEx       │
│                          │          │   + RAWINPUT)           │
└─────────────────────────┘          └─────────────────────────┘
```

### 两种通信信道

| 信道         | 协议    | 用途                            | 性能要求               |
| ------------ | ------- | ------------------------------- | ---------------------- |
| **视频信道** | **UDP** | 传输编码后的 H.264/H.265 数据包 | 延迟敏感，允许少量丢包 |
| **控制信道** | **TCP** | 配对、会话管理、输入事件传递    | 可靠传输，延迟不敏感   |

---

## Player 现有能力的复用评估

| 模块           | Player 现状                    | Moonlight-Qt 对照         | 能否复用                             |
| -------------- | ------------------------------ | ------------------------- | ------------------------------------ |
| D3D11 渲染管线 | ✅ D3D11Pipeline + HLSL         | 对应 `D3D11VA` renderer   | **直接复用**                         |
| 硬解           | ✅ VideoDecoder (D3D11VA)       | 对应 `FFmpegVideoDecoder` | **直接复用**                         |
| 多线程流水线   | ✅ Reader→Decoder→Render        | 对应 decoderThreadProc    | **需要微调**（数据来源从文件改网络） |
| 音视频同步     | ✅ 音频主时钟同步               | 串流场景不需要            | 串流时**直接跳过**                   |
| 音频播放       | ✅ AudioRenderer (QAudioOutput) | 对应 SDL/SoundIo renderer | **可复用**                           |
| TcpServer      | ⬜ 骨架                         | 输入控制信道              | **需要填充**                         |
| SafeQueue      | ✅ PushMax/Pop/TryPop/Flush     | 对应 LiQueue              | **直接复用**                         |
| UI 无边框窗口  | ✅                              | QML 应用（不同方案）      | 不参考                               |

**一句话结论**：Player 的解码+渲染能力比 moonlight-qt 原生实现**更强**（HLSL 着色器 + 硬解直通 Present 不经 CPU），客户端 80% 工作已经完成。

---

## Player 新增模块结构

```
Player/
├── Capture/                          # 新增：服务端捕获与编码
│   ├── ScreenCapture.h/.cpp          # D3D11 IDXGIOutputDuplication 封装
│   └── VideoEncoder.h/.cpp           # NVENC 编码器封装
├── Network/                          # 新增：网络传输
│   ├── NalUnit.h                     # NAL 分片/组帧定义
│   ├── VideoSender.h/.cpp            # UDP 发送器（服务端用）
│   └── VideoReceiver.h/.cpp          # UDP 接收器（客户端用）
├── Input/                            # 新增：输入事件
│   ├── InputCollector.h/.cpp         # 键盘鼠标采集（客户端用）
│   └── InputInjector.h/.cpp          # 输入注入（服务端用）
├── Core/                             # 已有，不改
├── MainUI/                           # 已有，小改
└── Notes/                            # 笔记文档
```

---

## 第一阶段：桌面捕获 + NVENC 编码 + 环回显示

### 目标

在本机上完成"桌面 → NVENC 编码"的全链路，验证编码器正常工作。可以用两种模式验收：环回显示（直接把抓屏画面显示在播放器窗口），或写 MP4 文件。

### 参考源码

| 功能             | 参考 Sunshine 文件                  | 关键代码位置                                      |
| ---------------- | ----------------------------------- | ------------------------------------------------- |
| 屏幕捕获         | `platform/windows/display_base.cpp` | `duplication_t::next_frame()` 第 30~60 行         |
| D3D11 设备初始化 | `platform/windows/display_base.cpp` | `display_base_t::init()` 第 550~560 行            |
| NVENC 编码       | `nvenc/nvenc_d3d11.cpp`             | `create_and_register_input_buffer()` 第 68~103 行 |
| NVENC 编码参数   | `nvenc/nvenc_base.cpp`              | `create_encoder()` 第 200~398 行                  |
| NVENC 分派       | `video.cpp`                         | `capture()` 函数（整合抓屏+编码+发送的主循环）    |

### 需要学的新知识

| 技术点         | 怎么学                       | 参考                                    |
| -------------- | ---------------------------- | --------------------------------------- |
| D3D11 屏幕捕获 | `IDXGIOutputDuplication` API | Sunshine `duplication_t`                |
| NVENC 硬件编码 | `nvEncodeAPI64.dll` 直接加载 | Sunshine `nvenc_d3d11::init_library()`  |
| 编码参数控制   | CBR、GOP、preset、tuning     | Sunshine `nvenc_base::create_encoder()` |

### 详细步骤

**1.1 ScreenCapture 类**

参考 Sunshine 的 `duplication_t` + `display_base_t`，两个类的核心职责：

```cpp
class ScreenCapture
{
public:
    // 初始化。device 从 D3D11Pipeline 传入（复用 Player 已有的 D3D11 设备）
    bool Init(ID3D11Device* device);

    // 每帧调用，捕获当前桌面，填入 AVFrame
    // 输出格式：AV_PIX_FMT_NV12（GPU 显存纹理，data[0]=Texture2D*, data[1]=index）
    //          或 AV_PIX_FMT_D3D11
    bool CaptureNextFrame(AVFrame* frame);

    void Release();

    int Width() const;
    int Height() const;
    int FrameRate() const;        // 显示器实际刷新率

private:
    IDXGIOutputDuplication* dup_{ nullptr };     // Sunshine 的 dup_t
    ID3D11Texture2D* capture_tex_{ nullptr };    // 自建纹理，CopyResource 用
    ID3D11Device* device_{ nullptr };
};
```

**Init 流程**（参考 Sunshine `test_dxgi_duplication()` 第 361~423 行 + `display_base_t::init()` 第 550~560 行）：

```
① D3D11CreateDevice → 创建设备
   注意：flags 必须带 D3D11_CREATE_DEVICE_VIDEO_SUPPORT

② CreateDXGIFactory1 → 枚举适配器

③ EnumAdapters1 → 枚举显卡

④ adapter.EnumOutputs → 枚举显示器输出

⑤ output1->DuplicateOutput → 创建 IDXGIOutputDuplication
```

**CaptureNextFrame 流程**（参考 Sunshine `duplication_t::next_frame()` 第 30~60 行）：

```
① dup->AcquireNextFrame(timeout, &frame_info, &resource)
     → S_OK: 成功
     → DXGI_ERROR_WAIT_TIMEOUT: 超时（桌面无变化）
     → DXGI_ERROR_ACCESS_LOST: 需要重新初始化

② resource->QueryInterface(ID3D11Texture2D) → 取到桌面纹理

③ d3d11_ctx_->CopyResource(capture_tex_, desktop_tex) → 拷贝到自建纹理
   （为什么需要自建：AcquireNextFrame 拿到的纹理释放后就不能再读了，
    必须自己保留一份）

④ dup->ReleaseFrame() → 释放帧（关键，不释放下一次 Acquire 会失败）
    参考 Sunshine `release_frame()` 第 72~94 行

⑤ frame->data[0] = capture_tex_;  // 填到 AVFrame
   frame->format = AV_PIX_FMT_D3D11;
```

**duplication_t 生命周期**（Sunshine `duplication_t` 第 156~170 行）：

```
next_frame 和 release_frame 必须配对调用：
  AcquireNextFrame → 使用 → ReleaseFrame → 下一次 AcquireNextFrame

如果 AcquireNextFrame 成功后不调用 ReleaseFrame：
  下一次 AcquireNextFrame 立即返回 DXGI_ERROR_ACCESS_LOST
```

**1.2 VideoEncoder 类（FFmpeg 编码器方案，推荐）**

用 FFmpeg 封装 NVENC，不走 Sunshine 那种直接调用 `nvEncodeAPI64.dll` 的复杂路径。你用 `avcodec_find_encoder_by_name("h264_nvenc")`，已经集成的 FFmpeg 一步到位。

```cpp
class VideoEncoder
{
public:
    // 初始化编码器
    bool Init(int width, int height, int fps, int bitrate_kbps);

    // 编码一帧，输入 AVFrame（NV12/D3D11），输出编码后的 AVPacket
    bool EncodeFrame(AVFrame* frame, AVPacket* packet);

    void Release();

private:
    AVCodecContext* encoder_ctx_{ nullptr };
};
```

**编码参数配置**（参考 Sunshine `nvenc_base::create_encoder()` 第 204~351 行，转为 FFmpeg AVOption）：

| Sunshine 参数                          | 含义                    | FFmpeg AVOption    |
| -------------------------------------- | ----------------------- | ------------------ |
| `presetGUID = P1`                      | 最快编码速度            | `"preset" = "p1"`  |
| `tuningInfo = ULTRA_LOW_LATENCY`       | 超低延迟模式            | `"tune" = "ll"`    |
| `rateControlMode = CBR`                | 恒定码率                | `"rc" = "cbr"`     |
| `averageBitRate`                       | 目标码率                | `"b" = 数值`       |
| `gopLength = INFINITE`                 | 只插 IDR 不插 P 帧      | `"gop_size" = 0`   |
| `zeroReorderDelay = 1`                 | 禁止 B 帧（无重排延迟） | `"bf" = 0`         |
| `frameIntervalP = 1`                   | 无 B 帧                 | `"b_strategy" = 0` |
| `enablePTD = 1`                        | 分片编码                | `"slices" = 4`     |
| `NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY` | 超低延迟调优            | `"tune" = "ll"`    |

**关键参数汇总（直接写在 Init 里）：**

```cpp
codec = avcodec_find_encoder_by_name("h264_nvenc");     // 或 hevc_nvenc

ctx->width = width;
ctx->height = height;
ctx->time_base = AVRational{1, fps};
ctx->framerate = AVRational{fps, 1};
ctx->pix_fmt = AV_PIX_FMT_D3D11;                         // 直接输入 D3D11 纹理

av_opt_set(ctx->priv_data, "preset", "p1", 0);           // 最快
av_opt_set(ctx->priv_data, "tune", "ll", 0);             // 超低延迟
av_opt_set(ctx->priv_data, "rc", "cbr", 0);              // 恒定码率
av_opt_set(ctx->priv_data, "b", "10000k", 0);            // 10Mbps
av_opt_set(ctx->priv_data, "gop_size", "0", 0);          // 不插 P 帧
av_opt_set(ctx->priv_data, "bf", "0", 0);                // 不要 B 帧
ctx->slices = 4;                                          // 分片编码
```

**Sunshine 为什么不这样用**：因为它需要比 FFmpeg 封装的 NVENC 更细粒度的控制（`ref_frame_invalidation`、`AQ`、`two-pass`、自定义 VBV）。第一阶段不需要这些，FFmpeg 方案够了。第四阶段需要码率自适应时再考虑换。

**1.3 管线串联（环回显示模式）**

```cpp
// PlayerApp 新增：
void StartCapture()
{
    ScreenCapture capture;
    capture.Init(d3d11_device);

    while (capturing)
    {
        AVFrame* frame = av_frame_alloc();
        if (capture.CaptureNextFrame(frame))
        {
            // 直接喂给 VideoRenderer → 显示在播放器窗口
            video_renderer_->Render(frame);
        }
        av_frame_free(&frame);
    }
}
```

**1.4 管线串联（编码写文件模式）**

```cpp
void StartCaptureAndEncode()
{
    ScreenCapture capture;
    VideoEncoder encoder;
    capture.Init(d3d11_device);
    encoder.Init(width, height, fps, 10000);

    // 初始化 MP4 复用器（avformat_open + write_header）
    AVFormatContext* mux = InitMuxer("output.mp4");

    while (capturing)
    {
        AVFrame* frame = av_frame_alloc();
        AVPacket* packet = av_packet_alloc();

        if (capture.CaptureNextFrame(frame))
        {
            if (encoder.EncodeFrame(frame, packet))
            {
                av_interleaved_write_frame(mux, packet);
            }
        }
        av_frame_free(&frame);
        av_packet_free(&packet);
    }

    av_write_trailer(mux);
}
```

### 评价标准

| 指标       | 目标值                        | 测试方法            |
| ---------- | ----------------------------- | ------------------- |
| 捕获帧率   | 显示器刷新率（60/120/144fps） | 日志统计每秒帧数    |
| 编码帧率   | ≥60fps @ 1080p                | 录制的 MP4 文件帧率 |
| CPU 占用   | <10%                          | 任务管理器          |
| 文件可播放 | VLC / Player 可播放           | 用你的播放器打开    |

### 遇到过的坑

（等开发时遇到再记）

---

## 第二阶段：UDP 网络传输 + 客户端解码显示

### 目标

第一阶段编码后的 H.264 数据，通过 UDP 从服务端发送到客户端（本机，后续再改两台机器），客户端解码并显示。

### 参考源码

| 功能                | 参考来源           | 文件                                           |
| ------------------- | ------------------ | ---------------------------------------------- |
| UDP 发送框架        | Sunshine           | `network.cpp` + `stream.cpp`                   |
| RTSP 会话协商       | Sunshine           | `rtsp.cpp` + `rtsp.h`                          |
| 客户端解码+渲染     | Moonlight-Qt       | `streaming/video/ffmpeg.cpp`                   |
| NAL 分片接收        | Moonlight-Common-C | `VideoDepacketizer.cpp`                        |
| 客户端收帧→送显流程 | Moonlight-Qt       | `streaming/video/ffmpeg-renderers/d3d11va.cpp` |

### 详细步骤

**2.1 NAL 分片（服务端）**

H.264 编码器输出的 `AVPacket` 可能包含多个 NAL 单元，并且大小可能超过 MTU（1500 字节）。

参考 Moonlight-Common-C 的 `VideoDepacketizer`，分片方案：

```
每个 UDP 包格式：
┌────────┬────────┬────────┬────────┬─────────────────┐
│  Flags │ 序列号  │ 时间戳  │ NAL数据 │     NAL 负载     │
│  1字节  │  2字节  │  4字节  │  1字节  │   ≤ 1400 字节    │
└────────┴────────┴────────┴────────┴─────────────────┘

Flags 位：
  bit 0: 0=非关键帧, 1=关键帧（IDR）
  bit 1: 0=完整帧, 1=分片第一包
  bit 2: 0=完整帧, 1=分片最后一包
  bit 3~7: 保留
```

**分片逻辑（发送端）：**

```
对于一帧 H.264 数据（AVPacket）：
  如果大小 ≤ 1400 → 直接发，Flags 和 NAL数据都在同一个包
  如果大小 > 1400 → 切成多个包
      第一个包：Flags bit1=1（起始标记）
      中间包：  Flags 无标记
      最后一个包：Flags bit2=1（结束标记）
```

**2.2 发送器（服务端）**

参考 Sunshine `network.cpp`。使用标准 Windows socket，不依赖 Qt 的网络模块（避免 QAbstractSocket 的额外延迟）：

```cpp
class VideoSender
{
public:
    bool Init(const char* dst_ip, int dst_port);     // 目标地址+端口
    bool SendFrame(AVPacket* packet, int64_t pts);    // 编码一帧后调用
    void Release();
};
```

Sunshine 的端口约定：

- 视频流端口：**47998**
- 音频流端口：**48000**
- 控制信道端口：**47989**（TCP）

**2.3 接收器（客户端）**

```cpp
class VideoReceiver
{
public:
    bool Init(int listen_port = 47998);               // 监听端口
    void Start();                                      // 启动接收线程
    void Stop();                                       // 停止

    // 接收线程循环内部分片重组逻辑
    // 完整的帧放入 SafeQueue<AVPacket*>

private:
    void ReceiveLoop();
    SafeQueue<AVPacket*> frame_queue_;
};
```

**接收线程逻辑：**

```
while (running)
    recvfrom(buffer, sizeof(buffer))
    if (Flags 为完整帧)
        frame = alloc AVPacket
        memcpy NAL 数据到 frame->data
        frame_queue_.Push(frame)
    else if (Flags 为分片起始)
        start new frame → 分配 buffer
    else if (Flags 为分片中间或最后一个)
        append 到当前 frame buffer
        if (Flags 结束标记)
            frame_queue_.Push(组装好的 AVPacket)
```

**2.4 解码客户端（Player 端改动）**

Moonlight-Qt 的数据流向是：

```
drSubmitDecodeUnit() → 解码线程队列 → decoderThreadProc()
    → avcodec_send_packet → avcodec_receive_frame → renderer->renderFrame()
```

你的 Player 直接改数据来源即可：

```
原：Reader → packet_queue → VideoDecoder → frame_queue → DecodeLoop → Render
新：VideoReceiver → packet_queue → VideoDecoder → frame_queue → DecodeLoop → Render
```

需要改动：

| 改动点                    | 文件                  | 说明                                               |
| ------------------------- | --------------------- | -------------------------------------------------- |
| `OpenFile` → `OpenStream` | `FFmpegPlayer.h/.cpp` | 新增方法，不走 avformat_open_input，直接收网络包   |
| 低延迟模式开关            | `FFmpegPlayer.h/.cpp` | 新增 `SetLowLatencyMode(bool)`，串流时跳过同步等待 |

**低延迟模式**（参考 Moonlight-Qt 的 Pacer 模块）：

```cpp
void FFmpegPlayer::SetLowLatencyMode(bool enable)
{
    low_latency_mode_ = enable;
}

// DecodeLoop 中：
double delay_sec = frame_interval_ms / 1000.0;
if (low_latency_mode_)
{
    delay_sec = 0;                                   // 不等，立刻渲染
    video_frame_queue_.PushMax(frame, 1);            // 队列上限从 3 降到 1
}
else
{
    // 原有同步逻辑不变
}
```

### 验收演示

本机跑通：

1. 开第一个实例：Player 服务端模式 → 抓屏 → 编码 → UDP 发到 `127.0.0.1:47998`
2. 开第二个实例：Player 客户端模式 → UDP 接收 → 解码 → 显示
3. 观察延迟：毫秒时钟对比，<30ms

### 评价标准

| 指标       | 目标值                 | 测试方法                           |
| ---------- | ---------------------- | ---------------------------------- |
| 端到端延迟 | <30ms（1080p @ 60fps） | 在服务端显示毫秒时钟，手机拍照对比 |
| 帧率       | =服务端帧率（60fps）   | 客户端统计每秒解码帧数             |
| 丢包率     | <1%（无限制局域网）    | 统计序列号断点                     |

---

## 第三阶段：输入事件转发

### 目标

客户端采集键盘鼠标事件，通过 TCP 发送到服务端，服务端注入到系统输入流中。

### 参考源码

| 功能                     | 参考来源     | 文件                                       |
| ------------------------ | ------------ | ------------------------------------------ |
| 键盘鼠标采集             | Moonlight-Qt | `app/streaming/input/input.cpp`            |
| 鼠标原始输入             | Moonlight-Qt | `app/streaming/input/mouse.cpp`            |
| 输入注入（服务端接收端） | Sunshine     | `input.cpp` + `platform/windows/input.cpp` |

### 详细步骤

**3.1 输入事件采集（客户端）**

参考 Moonlight-Qt 的 `SdlInputHandler`（`input/input.cpp`），但它依赖 SDL。用纯 Windows API 实现：

```cpp
class InputCollector
{
public:
    bool Start();             // 启动监听
    void Stop();

    // 回调：采集到事件时调用
    std::function<void(const InputEvent&)> OnInputEvent;

private:
    // 低级键盘钩子
    HHOOK keyboard_hook_{ nullptr };
    static LRESULT CALLBACK KeyboardProc(int nCode, WPARAM wParam, LPARAM lParam);

    // 原始鼠标输入
    void RegisterRawInput();
};
```

**键盘事件采集**（参考 Moonlight-Qt `keyboard.cpp`）：

```cpp
SetWindowsHookEx(WH_KEYBOARD_LL, KeyboardProc, GetModuleHandle(NULL), 0);

// KeyboardProc 回调中：
case WM_KEYDOWN:
    event.type = INPUT_KEYBOARD;
    event.keycode = ((KBDLLHOOKSTRUCT*)lParam)->vkCode;  // 虚拟键码
    event.pressed = true;
    break;
case WM_KEYUP:
    event.pressed = false;
    break;
```

**鼠标事件采集**（参考 Moonlight-Qt `mouse.cpp` + `input.cpp`）：

```cpp
// 注册原始输入设备
RAWINPUTDEVICE rid;
rid.usUsagePage = 0x01;       // HID 用法页：通用桌面
rid.usUsage = 0x02;           // HID 用法：鼠标
rid.dwFlags = RIDEV_INPUTSINK;  // 即使窗口不活动也接收
rid.hwndTarget = hwnd;

RegisterRawInputDevices(&rid, 1, sizeof(rid));

// 处理 WM_INPUT 消息：
case WM_INPUT:
{
    UINT size;
    GetRawInputData((HRAWINPUT)lParam, RID_INPUT, nullptr, &size, sizeof(RAWINPUTHEADER));
    // 读取鼠标相对位移 → 填充 InputEvent
}
```

**3.2 TCP 控制信道**

复用 Player 已有的 `TcpServer` 骨架（`TCP/TcpServer.h/.cpp`）：

- 服务端：监听 47989，收到输入事件后调用 `SendInput()`
- 客户端：连接服务端 47989，发送采集到的输入事件

消息格式（JSON 或简单二进制，第一阶段不需要 Protobuf）：

```
{ "type": "keyboard", "key": 0x41, "down": true }
{ "type": "mousemove", "dx": 12, "dy": -5 }
{ "type": "mousebtn", "btn": 0, "down": true }    // 0=左键, 1=右键
```

**3.3 输入事件注入（服务端）**

```cpp
class InputInjector
{
public:
    void InjectKeyboard(int vk_code, bool down);
    void InjectMouseMove(int dx, int dy);         // 相对位移
    void InjectMouseButton(int btn, bool down);
};

void InputInjector::InjectKeyboard(int vk_code, bool down)
{
    INPUT input = {};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = vk_code;
    input.ki.dwFlags = down ? 0 : KEYEVENTF_KEYUP;
    SendInput(1, &input, sizeof(INPUT));
}

void InputInjector::InjectMouseMove(int dx, int dy)
{
    INPUT input = {};
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = MOUSEEVENTF_MOVE;
    input.mi.dx = dx;
    input.mi.dy = dy;
    SendInput(1, &input, sizeof(INPUT));
}
```

注意：**发送端用相对位移**（`MOUSEEVENTF_MOVE`，对应于 Moonlight 的"optimize for games"鼠标模式），**接收端用 `SendInput` 直接注入**（Sunshine 的 `InputInjector` 也是这么做的）。

### 评价标准

| 指标       | 目标值             | 测试方法                   |
| ---------- | ------------------ | -------------------------- |
| 输入延迟   | <50ms              | 在服务端开鼠标轨迹测试工具 |
| 按键无丢失 | 连续按键无丢失     | 高频按键测试 + 计数对比    |
| 鼠标精度   | 1080p 内无明显偏差 | 圈定目标点击测试           |

---

## 第四阶段：配对 + 会话管理 + 码率自适应

### 目标

实现 Moonlight 兼容的配对流程，支持多客户端，加入码率自适应。

### 参考源码

| 功能          | 参考来源     | 文件                            |
| ------------- | ------------ | ------------------------------- |
| HTTPS 控制台  | Sunshine     | `nvhttp.cpp` + `confighttp.cpp` |
| 配对流程      | Sunshine     | `crypto.cpp` + `nvhttp.cpp`     |
| RTSP 会话协商 | Sunshine     | `rtsp.cpp` + `rtsp.h`           |
| 客户端配对    | Moonlight-Qt | `backend/nvpairingmanager.cpp`  |
| 多路编码      | Sunshine     | `video.cpp` 的 `capture()` 函数 |
| 码率自适应    | Sunshine     | `stat_trackers.h`               |

### 详细步骤

**4.1 配对流程**

Sunshine 的配对流程（`nvhttp.cpp`）：

```
客户端 → HTTPS GET /serverinfo          → 获取服务端信息（版本、GCM 支持）
客户端 → HTTPS POST /pair?pin=xxxx      → 发送 PIN 码
服务端 → 返回 204（配对成功）
客户端 → HTTPS POST /pair?uniqueid=xxx  → 客户端发送自己的唯一 ID
服务端 → 返回服务器证书 cert
客户端 → 用服务端公钥加密客户端证书 → POST /pinpair
服务端 → 验证签名，返回配对结果
```

配对完成后，客户端发起 RTSP 连接协商串流参数：

```
客户端 → RTSP OPTIONS → 服务端
服务端 → RTSP DESCRIBE → 返回 SDP（含视频/音频参数）
客户端 → RTSP SETUP → 建立传输通道
客户端 → RTSP PLAY → 开始串流
```

Sunshine 的 `rtsp.cpp` 里实现了这一整套流程。第一阶段可以**跳过配对**，直接用配置文件写死参数。

**4.2 码率自适应**

参考 Sunshine 的 `stat_trackers.h`。最基本算法：

```cpp
// 每秒统计一次丢包率
double loss_rate = (double)lost_packets / total_packets;

if (loss_rate > 0.05)
    bitrate = (int)(bitrate * 0.85);   // 丢包超过 5%，降码率
else if (loss_rate < 0.01)
    bitrate = std::min(max_bitrate, (int)(bitrate * 1.05));  // 丢包少，升码率

// 重新配置编码器
encoder.Reconfigure(bitrate);
```

限制范围：5Mbps ~ 50Mbps，调整间隔至少 1 秒。

### 评价标准

| 指标       | 目标值               | 测试方法               |
| ---------- | -------------------- | ---------------------- |
| 配对成功率 | 100%（正常网络环境） | 反复配对 100 次        |
| 多路并发   | ≥4 路 1080p @ 30fps  | 4 个客户端同时连接     |
| 码率自适应 | 限速后 5 秒内稳定    | 用 Clumsy 模拟网络变化 |

---

## 与 Moonlight/Sunshine 的核心差异

| 特性           | Moonlight / Sunshine      | 我们的方向                                    |
| -------------- | ------------------------- | --------------------------------------------- |
| 客户端渲染     | SDL + D3D11 简单 NV12→RGB | **HLSL 着色器全 GPU 管线，不经 CPU**          |
| 多路并发服务端 | 单路会话                  | **支持多路**（复用 DemandStation 多会话架构） |
| 录像回放       | 无                        | **内置录播功能**                              |
| 桌面远程办公   | 主要为游戏设计            | **桌面 + 游戏双模式**                         |
| 配对系统       | 仅 HTTPS 证书配对         | **可接入用户认证系统**                        |

---

## 文件中提到的类与其对应 Sunshine 源码文件速查表

| 文件                                | 关键类/函数                                                  | 作用                       |
| ----------------------------------- | ------------------------------------------------------------ | -------------------------- |
| `platform/windows/display_base.cpp` | `display_base_t::init()`, `duplication_t::next_frame()`, `capture()` | D3D11 屏幕捕获全流程       |
| `platform/windows/display.h`        | `display_base_t`, `duplication_t`, `display_ram_t`, `display_vram_t` | 捕获相关的类型定义         |
| `platform/windows/display_ram.cpp`  | `snapshot()`, `alloc_img()`                                  | CPU 模式抓屏实现           |
| `platform/windows/display_vram.cpp` | 游标合成、GPU 模式抓屏                                       | GPU 模式抓屏实现           |
| `nvenc/nvenc_d3d11.cpp`             | `nvenc_d3d11::init_library()`, `create_and_register_input_buffer()` | NVENC D3D11 输入缓冲创建   |
| `nvenc/nvenc_base.cpp`              | `create_encoder()`, `encode_frame()`, `invalidate_ref_frames()` | NVENC 编码器全流程         |
| `nvenc/nvenc_config.h`              | `nvenc_config` 结构体                                        | NVENC 编码参数配置         |
| `video.cpp`                         | `capture()`                                                  | 整合抓屏→编码→发送的总入口 |
| `video.h`                           | `config_t`, `encoder_t`, `encode_session_t`                  | 编码配置和相关类型         |
| `stream.cpp`                        | UDP 视频发送                                                 | 网络传输                   |
| `network.cpp`                       | socket 封装                                                  | 网络底层                   |
| `rtsp.cpp`                          | RTSP 会话协商                                                | 流参数协商                 |
| `platform/windows/input.cpp`        | 键盘鼠标注入                                                 | 服务端输入接收             |
| `input.cpp`                         | 输入事件处理                                                 | 输入事件转换               |
| `stat_trackers.h`                   | 丢包统计、码率自适应                                         | 网络统计                   |

以及 Moonlight-Qt 对应文件：

| 文件                                               | 关键类/函数                                                  | 作用                  |
| -------------------------------------------------- | ------------------------------------------------------------ | --------------------- |
| `streaming/video/ffmpeg.cpp`                       | `FFmpegVideoDecoder`, `submitDecodeUnit()`, `decoderThreadProc()` | 客户端解码线程        |
| `streaming/video/ffmpeg-renderers/d3d11va.cpp`     | D3D11VA 渲染器                                               | 客户端 D3D11 硬解渲染 |
| `streaming/video/ffmpeg-renderers/pacer/pacer.cpp` | Pacer                                                        | 客户端帧率控制        |
| `streaming/video/decoder.h`                        | `IVideoDecoder`, `DECODER_PARAMETERS`                        | 解码器接口定义        |
| `streaming/session.cpp`                            | `Session`, `drSetup()`, `drSubmitDecodeUnit()`               | 客户端会话管理        |
| `streaming/input/input.cpp`                        | `SdlInputHandler`                                            | 客户端输入采集        |
| `streaming/input/mouse.cpp`                        | 鼠标事件处理                                                 | 鼠标采集              |
| `streaming/input/keyboard.cpp`                     | 键盘事件处理                                                 | 键盘采集              |