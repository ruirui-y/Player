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

## 第四阶段：音频串流（服务端采集 + 客户端播放）

### 目标

服务端用 WASAPI 环回捕获系统音频，Opus 编码后通过 UDP（端口 48000）发送；客户端解码播放。完成后串流有画面有声音。

### 参考源码

| 功能             | 参考来源     | 文件                                                   |
| ---------------- | ------------ | ------------------------------------------------------ |
| 服务端 WASAPI 环回 | Sunshine     | `platform/windows/audio.cpp` 的 `mic_wasapi_t`         |
| Opus 多流编码     | Sunshine     | `audio.cpp` 的 `audio::encodeThread()`                 |
| 音频 RTP 封装     | Sunshine     | `stream.cpp` 的 `audioBroadcastThread()`               |
| 客户端 Opus 解码  | Moonlight-Qt | `streaming/audio/audio.cpp` 的 `arDecodeAndPlaySample()` |
| 客户端音频渲染    | Moonlight-Qt | `streaming/audio/renderers/sdlaud.cpp`                 |

### 详细步骤

**4.1 服务端 WASAPI 环回采集**

- 初始化：`CoCreateInstance(MMDeviceEnumerator)` → 默认渲染端点 → `IAudioClient::Initialize(SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK)`
- 事件驱动：`SetEventHandle` + 独立线程 `WaitForSingleObjectEx`，线程优先级用 MMCSS "Pro Audio"
- 取数据：`IAudioCaptureClient::GetBuffer` → 读 PCM（默认 48000Hz、float32、声道数随系统）
- 监听默认设备变更（`IMMNotificationClient`），变更时自动重初始化

**4.2 Opus 编码**

- `opus_multistream_encoder_create`，`OPUS_APPLICATION_RESTRICTED_LOWDELAY`，CBR
- 首期只做立体声 48kHz；后续再扩展 5.1/7.1 六档预设
- 帧长选 2.5ms（120 样本）以压到超低延迟

**4.3 UDP 发送（端口 48000）**

- 包格式类比视频：1 字节 flags + 2 字节 frame index + 2 字节 packet index + 4 字节 timestamp + payload
- Opus 一帧通常 <400 字节，一个 UDP 包足够，无需分片
- 新建 `AudioSender`，复用 `VideoSender` 的 socket 封装思路

**4.4 客户端解码播放**

- 改 `AudioDecoder`：数据源从文件包队列改为 `AudioReceiver`（UDP 48000）
- Opus 解码 `opus_multistream_decode` → PCM
- 复用已有 `AudioRenderer`（QAudioOutput），格式 48000/16bit/立体声
- 串流模式下不做音视频同步，音频来即播（音频本身就是天然时钟）

> **技术决策说明**
>
> **为什么是这个方案**：Opus 是为低延迟语音/音频设计的编解码器，2.5ms 帧延迟 + 约 64kbps 立体声即可透明；WASAPI loopback 能直接抓系统混音输出而不需要虚拟声卡。相比 AAC，Opus 在低延迟场景下延迟低一个数量级。
>
> **底层是什么**：WASAPI loopback 用 `AUDCLNT_STREAMFLAGS_LOOPBACK` 让系统在共享模式下把"正在播放的混音流"复制一份给你；它是事件驱动的，数据就在音频引擎的环形缓冲里，没有轮询开销。
>
> **如果是你你会怎么学**：先看 Sunshine `platform/windows/audio.cpp` 的 `mic_wasapi_t::init()`，再读 Opus 官方 API 文档的 Encoder 一节；MSDN "Loopback Recording" 示例可直接跑通。

### 评价标准

| 指标     | 目标值      | 测试方法                      |
| -------- | ----------- | ----------------------------- |
| 音频延迟 | <80ms       | 服务端敲键、客户端录音对比波形 |
| 采样率   | 48000Hz     | 播放正弦波听音质              |
| 无杂音   | 持续不爆音  | 播放音乐 10 分钟              |

---

## 第五阶段：抗丢包机制（FEC + 参考帧失效 + IDR 请求）

### 目标

当前 UDP 视频丢包会导致画面花屏直到下一个关键帧。本阶段加入三档抗丢包：FEC 前向纠错（冗余包恢复少量丢包）、参考帧失效 RFI（失效受影响参考帧修复中量丢包）、IDR 请求（兜底大量丢包）。

### 参考源码

| 功能           | 参考来源          | 文件                                              |
| -------------- | ----------------- | ------------------------------------------------- |
| FEC 编码       | Sunshine          | `stream.cpp` 的 `fec::encode()` + `rs.h`(moonlight-common-c) |
| FEC 解码       | moonlight-common-c | `RtpFec.c`                                        |
| 参考帧失效     | Sunshine          | `nvenc/nvenc_base.cpp` 的 `invalidate_ref_frames()` |
| IDR 请求处理   | Sunshine          | `stream.cpp` 的 `IDX_REQUEST_IDR_FRAME`           |
| 客户端丢包检测 | moonlight-common-c | `VideoDepacketizer.c` + `RtpReorder.c`            |

### 详细步骤

**5.1 FEC 前向纠错（服务端编码 + 客户端恢复）**

- 服务端：一帧切成数据分片后，按 `fec_percentage`（如 20%）用 Reed-Solomon 生成 parity 分片，交织在数据包流里发出
- 包头扩展加 `fecInfo`（shard index、data_shards、percentage）
- 客户端：`NalReassembler` 收包时若发现某帧缺包，先用 parity 分片尝试恢复；恢复失败再上报

**5.2 参考帧失效 RFI（客户端 → 服务端）**

- 客户端记录 FEC 恢复失败的帧序号范围，通过控制信道发 `Invalidate Ref Frames`（first_frame, last_frame）
- 服务端编码线程收到事件 → `nvEncInvalidateRefFrames` 失效 DPB 中对应参考帧 → 下一帧编出"参考帧失效后的 P 帧"（frameType=5），无需整帧 IDR

**5.3 IDR 请求（兜底）**

- 丢失范围超过 DPB 大小或 RFI 不足时，客户端发 `Request IDR Frame`，服务端下一帧强制编 IDR
- 首帧必须 IDR：客户端首帧非 IDR 直接返回 `DR_NEED_IDR`

> **技术决策说明**
>
> **为什么是这个方案**：UDP 不重传，丢包恢复有三档——FEC 靠冗余包无延迟恢复少量丢包；RFI 靠服务端失效参考帧用一个 P 帧修复中量丢包；IDR 靠整帧重编兜底大量丢包。三档组合比单纯重传或单纯 IDR 都低延迟。重传在串流里不可行，因为等一个 RTT 来回，画面已经过去了。
>
> **底层是什么**：Reed-Solomon 是一种纠删码，把 k 个数据分片编码成 k+m 个分片，任意收到 k 个就能还原，本质是有限域 GF(2^8) 上的多项式运算。RFI 依赖 NVENC 的 `nvEncInvalidateRefFrames`，它把指定帧从参考帧缓冲 DPB 里标记为无效，编码器下一帧就不引用它，避免错误传播。
>
> **如果是你你会怎么学**：Reed-Solomon 看 moonlight-common-c 的 `rs.h`（nanors 实现）和维基百科 Reed–Solomon error correction；RFI 看 Sunshine `nvenc/nvenc_base.cpp::invalidate_ref_frames` 和 NVENC 编程指南的 Reference Frame Invalidation 一节。

### 评价标准

| 指标        | 目标值          | 测试方法                  |
| ----------- | --------------- | ------------------------- |
| FEC 恢复率  | 5% 丢包下 >90%  | Clumsy 模拟 5% 丢包看花屏 |
| RFI 生效    | 丢包后 1 帧内恢复 | 人工丢包看画面恢复速度    |
| 无永久花屏  | 任何丢包不残留  | 长时间丢包测试            |

---

## 第六阶段：控制信道改造 + 网络统计与自适应码率

### 目标

当前输入走 TCP，存在队头阻塞风险，且缺少网络质量感知。本阶段评估控制信道方案、统一控制消息、加入 RTT/丢包测量与码率自适应闭环。

### 参考源码

| 功能         | 参考来源     | 文件                                          |
| ------------ | ------------ | --------------------------------------------- |
| ENet 控制流  | Sunshine     | `stream.cpp` 的 `control_server_t`            |
| 控制消息类型 | Sunshine     | `stream.cpp` 的 `packetTypes[]`（15 种）      |
| 网络统计     | Moonlight-Qt | `streaming/video/decoder.h` 的 `VIDEO_STATS`  |
| 丢包上报     | Sunshine     | `stream.cpp` 的 `IDX_LOSS_STATS`              |

### 详细步骤

**6.1 控制信道方案选择（需先定夺）**

- 兼容路线：引入 ENet（UDP 47999），可靠+不可靠混合通道。输入走可靠通道（不能丢），IDR/RFI 请求走不可靠通道（丢了重发即可）
- 自研路线：保留 `InputTransportServer` 的 TCP，但把 IDR/RFI/丢包上报也加进来。代价是 TCP 队头阻塞——一个大输入包卡住会阻塞后续请求

**6.2 控制消息统一**

- 无论 ENet 还是 TCP，定义统一消息格式：2 字节类型 ID + payload
- 消息类型：输入数据、IDR 请求、RFI 请求、丢包统计、震动反馈、连接状态、HDR 模式

**6.3 网络统计**

- RTT：控制信道往返测量（客户端发 ping 带时间戳，服务端立即回）
- 丢包率：视频包序列号缺口统计（现有 NAL 包已有 2 字节 frame index + 2 字节 packet index，直接算）
- 客户端 `VIDEO_STATS`：接收/解码/渲染帧数、网络丢帧、RTT 方差

**6.4 自适应码率闭环**

- 客户端每秒上报丢包率/RTT
- 服务端：丢包 >5% 降码率×0.85，<1% 升码率×1.05（限 5~50Mbps，间隔≥1s）
- 调 NVENC 运行时改码率（`nvEncReconfigureEncoder`，或 FFmpeg 路线在 `avcodec_send_frame` 前改 `rc` 参数）
- 注意：Sunshine 服务端**不做**运行时自适应码率，丢包全靠客户端驱动 IDR/RFI。这是本项目相对 Sunshine 的自研增强点

> **技术决策说明**
>
> **为什么是这个方案**：TCP 的队头阻塞 head-of-line blocking 是低延迟输入的死敌——一个丢包会让后续所有包排队等重传。ENet 在 UDP 上自建可靠/不可靠双通道，输入走可靠通道但不会被一个大包阻塞。但 ENet 引入复杂度，自研路线可先用 TCP 跑通统计闭环，再决定是否换。
>
> **底层是什么**：队头阻塞是 TCP 字节流有序交付的代价——内核收序缓冲里第 1 个包丢了，后面到了的包也得等重传。ENet 用 UDP 自己管序列号和重传，按流分通道，互不阻塞。
>
> **如果是你你会怎么学**：ENet 看 https://github.com/cgutman/enet 和其 Reliability 文档；队头阻塞看 QUIC 的设计动机——QUIC 就是为了解决 TCP HoL 才发明的。

### 评价标准

| 指标        | 目标值           | 测试方法                 |
| ----------- | ---------------- | ------------------------ |
| 输入延迟    | <50ms（含弱网）  | Clumsy 1% 丢包测输入响应 |
| RTT 测量精度 | ±5ms             | 与 ping 对比             |
| 码率自适应  | 限速后 5s 内稳定 | Clumsy 动态限速          |

---

## 第七阶段：会话协商（RTSP/SDP）+ 配对与流加密

### 目标

当前编解码参数硬编码、无安全认证。本阶段加入 RTSP/SDP 参数协商、HTTPS 配对（证书 + 挑战响应）、AES-GCM 流加密。纯内网自用可跳过本阶段，对接标准 Moonlight 客户端则必须实现。

### 参考源码

| 功能       | 参考来源          | 文件                                            |
| ---------- | ----------------- | ----------------------------------------------- |
| RTSP 服务器 | Sunshine          | `rtsp.cpp` 的 `cmd_describe/setup/announce/play` |
| SDP 生成   | moonlight-common-c | `SdpGenerator.c`                                |
| HTTPS 配对 | Sunshine          | `nvhttp.cpp` 的 `pair()` + `crypto.cpp`         |
| 客户端配对 | Moonlight-Qt      | `backend/nvpairingmanager.cpp`                  |
| 流加密     | Sunshine          | `crypto.cpp` 的 `cipher::gcm_t`                 |

### 详细步骤

**7.1 RTSP/SDP 协商（TCP 48010）**

- DESCRIBE：服务端返回能力声明（codec、分辨率档位、帧率、FEC 支持、RFI 能力、音频档位）
- SETUP：客户端为 audio/video/control 各请求端口，服务端返回 server_port + ping payload
- ANNOUNCE：客户端发完整流参数（分辨率/帧率/码率/CSC/FEC/加密标志），服务端据此建 `config_t`
- PLAY：开始串流

**7.2 HTTPS 配对（TCP 47984）**

- 服务端自签 X.509 证书 + 私钥（CN="NVIDIA GameStream Client"）
- 4 步挑战-响应：getservercert → clientchallenge → serverchallengeresp → clientpairingsecret
- 用 `SHA-256(salt+PIN)` 截 16 字节作 AES-128-ECB 密钥加密 challenge
- 配对成功后客户端证书存入信任链，后续连接凭证书认证

**7.3 流加密（AES-GCM）**

- 配对后 launch 传 `rikey`（GCM key）
- 视频/音频/控制流分别 AES-GCM 加密
- IV 用序列号 + 固定字节按 NIST SP 800-38D 8.2.1 构造（'V'/'A'/'C'+'H'+'C' 前缀）

> **技术决策说明**
>
> **为什么是这个方案**：RTSP 是流媒体参数协商的标准协议，用它动态协商 codec/分辨率/码率，不用改代码重编译。配对用挑战-响应而非明文密码，是因为 PIN 码本身很弱（4 位），必须靠非对称+对称组合把弱 PIN 升级成强会话密钥。AES-GCM 是因为串流既要加密防窃听又要认证防篡改，GCM 一次性给两者。
>
> **底层是什么**：RTSP 本质是 HTTP-like 文本协议，每条消息有方法+URL+头+体，状态码语义和 HTTP 一致。AES-GCM 是 AEAD 认证加密，GCM 模式把 CTR 计数器加密和 GHASH 认证同步做，一个 pass 出密文+tag。IV 不能重用是硬约束，所以用序列号构造。
>
> **如果是你你会怎么学**：RTSP 看 RFC 2326 和 Sunshine `rtsp.cpp`；配对流程看 `nvhttp.cpp::pair()` 的 4 步状态机；AES-GCM 看 NIST SP 800-38D 和 OpenSSL 的 `EVP_aes_128_gcm` 示例。

### 评价标准

| 指标       | 目标值            | 测试方法           |
| ---------- | ----------------- | ------------------ |
| 协商成功率 | 100%              | 反复协商 50 次     |
| 配对成功率 | 100%（正常网络）  | 反复配对 20 次     |
| 加密开销   | <5% CPU           | 对比加密前后 CPU   |

---

## 第八阶段：体验打磨（帧步调 + 色彩空间/HDR + OSD + 输入完整性）

### 目标

解决画面抖动、色彩不准、输入不完整。本阶段打磨 VSync 对齐的帧步调 Pacer、色彩空间矩阵与 HDR、OSD 统计叠加、手柄/触屏/震动/快捷键。

### 参考源码

| 功能       | 参考来源     | 文件                                                            |
| ---------- | ------------ | --------------------------------------------------------------- |
| 帧步调 Pacer | Moonlight-Qt | `streaming/video/ffmpeg-renderers/pacer/pacer.cpp`              |
| VSync 源   | Moonlight-Qt | `pacer/dxvsyncsource.cpp`（`D3DKMTWaitForVerticalBlankEvent`）  |
| 色彩空间矩阵 | Moonlight-Qt | `d3d11va.cpp` 的 6 套 CSC 矩阵                                  |
| HDR 元数据 | Sunshine      | `video_colorspace.cpp` + `display_base.cpp::get_hdr_metadata`  |
| 手柄注入   | Sunshine      | `platform/windows/input.cpp`（ViGEm）                           |
| OSD 叠加   | Moonlight-Qt | `streaming/video/overlaymanager.cpp`                            |

### 详细步骤

**8.1 帧步调 Pacer**

- 当前"来一帧渲染一帧"，帧率不稳会抖动
- Pacer 双队列：pacing 队列(缓冲) + render 队列(上限各 4 帧)
- VSync 回调线程（`D3DKMTWaitForVerticalBlankEvent` 阻塞等垂直空白，时间关键优先级）按刷新率触发渲染
- 帧率/刷新率不整除时智能丢帧避免积压

**8.2 色彩空间与 HDR**

- 现有 HLSL 着色器补齐 6 套 CSC 矩阵：BT.601/709/2020 × limited/full range
- HDR：客户端 `dynamicRange=1`(10bit) + HDR 显示器 → BT.2020 + ST2084 + 10bit
- 服务端从 DXGI Output6 取 HDR 元数据（primaries、白点、MaxCLL/MaxFALL），通过控制信道 `IDX_HDR_MODE` 发给客户端

**8.3 OSD 叠加**

- 在视频纹理上叠加统计文字：接收/解码/渲染 FPS、RTT、丢包率、码率
- 用 D2D 或简单 HLSL 文字纹理实现

**8.4 输入完整性**

- 手柄：客户端 SDL 采集，服务端用 ViGEm 模拟 X360/DS4 设备注入
- 触屏：绝对/相对手势
- 震动：服务端 `clRumble` 下发 → 客户端 SDL rumble
- 快捷键：Ctrl+Alt+Shift+字母组合（退出/全屏/统计/鼠标模式等）

> **技术决策说明**
>
> **为什么是这个方案**：VSync 对齐能消除撕裂和帧率不均——渲染线程不等 VSync 就 Present 会撕裂；等 VSync 但来一帧渲一帧会重复/丢帧。Pacer 用 VSync 回调驱动，按"显示刷新率 vs 视频帧率"的比值智能丢帧，是顺滑度的关键。
>
> **底层是什么**：`D3DKMTWaitForVerticalBlankEvent` 是内核态调用，直接阻塞到显示器的垂直同步信号，比 Sleep 精确得多——Sleep 受系统时钟粒度限制，最低约 0.5ms 且抖动大。
>
> **如果是你你会怎么学**：Pacer 看 Moonlight-Qt `pacer.cpp` 的双队列设计；VSync 看 MSDN 的 `D3DKMTWaitForVerticalBlankEvent`；色彩空间看 Sunshine `video_colorspace.cpp` 和 FFmpeg 的 color_range/colorspace 字段说明。

### 评价标准

| 指标     | 目标值           | 测试方法            |
| -------- | ---------------- | ------------------- |
| 帧步调   | 无撕裂、无重复帧 | 高速相机拍画面      |
| HDR      | 正确显示 HDR 内容 | HDR 演示视频对比    |
| 手柄延迟 | <50ms            | 手柄按键到画面响应  |

---

## 第九阶段：广域网远程控制（公网服务器中转）

### 目标

前八阶段在局域网内把串流的全套功能打磨完毕后，本阶段解决"跨网络远程控制"的最后一公里——让不在同一局域网的两台机器也能串流。

核心思路：租一台公网云服务器做中转，服务端和客户端都主动连接到中转服务器，由中转服务器转发流量。不做 NAT 穿透。

### 为什么不做 NAT 穿透

NAT 穿透（STUN/TURN/ICE）是 P2P 领域最复杂的工程之一：要处理对称 NAT、端口预测、UPnP/PCP/NAT-PMP 多种协议、中继回退，工作量堪比重写半个 WebRTC。而且即便实现了，对称 NAT（运营商级 CGN）下仍然打不通，最终还是得靠中转服务器兜底。

直接用公网服务器中转，跳过 NAT 穿透的全部复杂度：两端都是出站连接，任何 NAT 都不阻拦，在所有网络环境下都能连通。代价是多一跳延迟和中转服务器带宽成本，但对远程桌面场景完全可接受。

> **技术决策说明**
>
> **为什么是这个方案**：NAT 穿透的投入产出比极低——花几周写 ICE 协议栈，对称 NAT 下仍然连不上，最终还是得租中转服务器。直接租服务器中转，1 小时跑通，在所有网络环境下都能连通，延迟多一跳约 10-40ms（取决于中转服务器位置），对远程桌面场景完全可接受。
>
> **底层是什么**：NAT 穿透的本质是让两个 NAT 后的设备互相"看见"对方，但 NAT 的设计就是阻止外部主动连入。出站连接（设备主动连外部）则不受任何 NAT 限制——这就是中转方案的根本原理：两端都出站连到公网服务器，公网服务器在中间桥接。
>
> **如果是你你会怎么学**：NAT 类型看 RFC 3489 的四种 NAT 定义；ICE 协议看 RFC 8445；如果想理解为什么放弃自建，看 WebRTC 的 ICE 实现复杂度就够了。

### 前置条件

本阶段要求前八阶段全部完成，特别是：
- **第七阶段的流加密（AES-GCM）必须已完成**：中转服务器能看到所有流量，明文传输等于把桌面和键盘输入裸奔给中转服务器
- **第五阶段的抗丢包（FEC/RFI/IDR）必须已完成**：广域网丢包率远高于局域网，没有抗丢包机制画面会持续花屏
- **第六阶段的自适应码率必须已完成**：广域网带宽波动大，固定码率会卡顿

### 中转架构

```
服务端（被控端）              中转服务器（公网）              客户端（控制端）
┌──────────────┐           ┌──────────────────┐           ┌──────────────┐
│              │  出站连接  │                  │  出站连接  │              │
│  桌面捕获     │ ─────────→│  流量转发         │←───────── │  解码渲染     │
│  NVENC 编码   │           │                  │           │              │
│              │←──────────│                  │──────────→│              │
│  输入注入     │  转发输入  │                  │  转发视频   │  输入采集     │
└──────────────┘           └──────────────────┘           └──────────────┘
     NAT 后                       公网 IP                      NAT 后
```

需要中转的信道：

| 信道 | 协议 | 端口 | 方向 |
| ---- | ---- | ---- | ---- |
| 视频 | UDP | 47998 | 服务端 → 中转 → 客户端 |
| 音频 | UDP | 48000 | 服务端 → 中转 → 客户端 |
| 控制/输入 | TCP | 47989 | 双向 |
| RTSP | TCP | 48010 | 客户端 → 中转 → 服务端 |

### 两种实现路线

**路线 A（快速验证）：frp 中转，0 代码改动**

frp（Fast Reverse Proxy）是一个成熟的反向代理工具，租一台公网服务器跑 frps，服务端机器跑 frpc 把端口暴露出去，客户端直接连公网服务器 IP。

- 公网服务器：下载 frp，运行 `frps -c frps.toml`
- 服务端机器：运行 `frpc`，把 47998/48000/47989/48010 端口暴露到公网
- 客户端：直接连公网服务器 IP 的对应端口

这条路不写一行串流代码，用现有第一到第八阶段的成果直接跑广域网。适合验证广域网可行性和测试延迟基线。

**路线 B（产品级）：自建中转服务器**

在公网服务器上跑一个自研的中转程序，管理多组服务端-客户端配对，按会话 ID 路由流量。相比 frp 的优势：可以做会话管理、带宽限制、多路复用、认证鉴权，最终产品化必须走这条路。

中转服务器的核心逻辑：
- 监听公网端口，接受服务端和客户端的连接
- 服务端连接时注册一个会话 ID
- 客户端连接时凭会话 ID 找到对应服务端
- 建立转发管道：服务端的视频/音频包转发给客户端，客户端的输入/控制包转发给服务端

> **技术决策说明**
>
> **为什么是这个方案**：frp 能 1 小时跑通验证，但它是通用代理工具，不知道串流协议语义，无法做会话管理、带宽控制、认证。自建中转服务器可以把"会话配对 + 流量转发 + 鉴权"一体化，是产品化的必经之路。先 A 验证可行性，再 B 做产品。
>
> **底层是什么**：中转服务器的本质是一个应用层路由器——它收到的每个包都带有会话标识（或连接本身绑定会话），查路由表找到对端连接，把包转发过去。UDP 包直接转发，TCP 流按消息边界转发。这和 TURN 服务器的原理一致，只是不用实现 ICE 的复杂协商。
>
> **如果是你你会怎么学**：frp 看 https://github.com/fatedier/frp 的文档；TURN 协议看 RFC 5766，理解 TURN 是怎么做中转的；自建中转可以参考 TURN 的 Allocate/Permission/Send 消息设计，但大幅简化。

### 详细步骤

**9.1 frp 中转验证（路线 A，0 代码）**

公网服务器配置 `frps.toml`：
```toml
bindPort = 7000
```

服务端机器配置 `frpc.toml`：
```toml
serverAddr = "<公网服务器IP>"
serverPort = 7000

[[proxies]]
name = "video"
type = "udp"
localIP = "127.0.0.1"
localPort = 47998
remotePort = 47998

[[proxies]]
name = "audio"
type = "udp"
localIP = "127.0.0.1"
localPort = 48000
remotePort = 48000

[[proxies]]
name = "control"
type = "tcp"
localIP = "127.0.0.1"
localPort = 47989
remotePort = 47989
```

服务端启动：`Player.exe --server --port 47998 --monitor 1 --ip <公网服务器IP> --ctrl-port 47989`

客户端连接：`Player.exe --client --ip <公网服务器IP> --port 47998`

验证基线指标：
- 端到端延迟：拍照对比毫秒时钟（预期比局域网高 20-60ms，取决于中转服务器到两端的延迟）
- 丢包率：统计 NAL 包序列号缺口
- 中转服务器带宽占用：`iftop` 或云监控查看，20Mbps 视频码率 × 2（进出各一份）

**9.2 自建中转服务器（路线 B）**

在公网服务器上新建 `RelayServer` 类：
- 监听 47998(UDP)/48000(UDP)/47989(TCP)/48010(TCP)
- 维护会话表：`session_id → {server_conn, client_conn}`
- 服务端连接时分配会话 ID，客户端连接时凭会话 ID 匹配
- UDP 转发：收到服务端的视频包 → 查会话表 → 转发给对应客户端
- TCP 转发：双向 pipe，服务端 ↔ 中转 ↔ 客户端

会话配对协议（TCP 控制信道上）：
```
服务端 → 中转：REGISTER server <session_id>
中转 → 服务端：OK
客户端 → 中转：CONNECT <session_id>
中转 → 客户端：OK / NOT_FOUND
配对成功后进入转发模式
```

**9.3 UPnP 端口映射（可选，直连备选）**

如果服务端机器的路由器支持 UPnP 且有公网 IP，可以跳过中转直接直连：
- 引入 miniupnpc：`upnpDiscover` → `GetValidIGD` → `AddPortMapping`
- 映射 47998/48000/47989/48010 端口
- UPnP 成功时客户端直连服务端公网 IP，延迟最低
- 失败时回退中转服务器

参考 Sunshine `upnp.cpp` 的 `map_upnp_port()`。

**9.4 应用管理**

- `apps.json` 定义应用：`{"name":"...", "cmd":"...", "prep-cmd":[...], "detached":[...]}`
- HTTPS `/launch?appid=x` → `proc_t::execute()`：先跑 prep-cmd，再启主 cmd
- 注入环境变量：`SUNSHINE_CLIENT_WIDTH/HEIGHT/FPS`
- 进程组管理：退出时 `request_process_group_exit` → 超时 `group.terminate()`

参考 Sunshine `process.cpp` 的 `proc_t::execute()`。

### 参考源码

| 功能         | 参考来源 | 文件                              |
| ------------ | -------- | --------------------------------- |
| UPnP 端口映射 | Sunshine | `upnp.cpp` 的 `map_upnp_port()`   |
| 应用启动     | Sunshine | `process.cpp` 的 `proc_t::execute()` |
| mDNS 服务发布 | Sunshine | `platform/windows/publish.cpp`    |
| Web 配置 UI  | Sunshine | `confighttp.cpp`                  |
| TURN 中转协议参考 | RFC 5766 | TURN 的 Allocate/Permission/Send 设计 |

### 评价标准

| 指标       | 目标值            | 测试方法               |
| ---------- | ----------------- | ---------------------- |
| 广域网连通 | 中转模式下可连    | 跨网络连接测试         |
| 中转延迟   | 比直连多 <40ms    | 拍照对比毫秒时钟       |
| 传输加密   | 中转服务器抓包不可读 | Wireshark 抓 UDP 看是否密文 |
| 丢包恢复   | IDR 请求后 2 帧内恢复 | Clumsy 模拟 5% 丢包    |
| 应用启动   | 远程一键启动      | 远程点应用看是否启动   |

### 阶段定位

本阶段是整个项目的收尾阶段，前八阶段在局域网内把功能打磨完毕后，本阶段只解决"跨网络连通"这一个问题。不做 NAT 穿透，靠公网服务器中转，在所有网络环境下都能连通。frp 快速验证可行性，自建中转服务器做产品化。

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