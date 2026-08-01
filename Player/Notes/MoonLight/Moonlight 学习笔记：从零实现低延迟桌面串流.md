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

### 通信信道与公网管控定位

| 信道 | 协议 | 用途 | 性能要求 |
| ---- | ---- | ---- | -------- |
| **视频信道** | **UDP** | 传输编码后的 H.264/H.265 数据包 | 延迟敏感，允许少量丢包 |
| **控制信道** | **TCP / 可靠 UDP** | 输入事件、IDR/RFI 请求、网络统计 | 小包优先，可靠但不能长期阻塞 |
| **信令信道** | **TLS** | 登录、设备发现、权限校验、STUN 协调、打洞地址交换 | 可靠、安全、流量极小 |
| **P2P 数据信道** | **UDP + ECDH 会话密钥** | 屏幕、音频、键鼠、文件、剪贴板 | 两端直连，服务器不可解密业务数据 |

公网版本里，服务器不是视频中继，而是设备发现、权限验证、NAT 穿透协调、审计和质量监控的管控中心。3M 云服务器带宽只用于 KB 级信令和状态上报，不承载桌面视频流；业务数据在控制端和被控端之间 P2P 直连，并用端到端密钥加密。

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

## 第九阶段：广域网远程控制（公网管控服务器 + P2P 穿透）

### 目标

前八阶段已经把局域网内的低延迟串流链路跑通，本阶段把系统扩展到跨互联网远程控制。公网服务器不转发屏幕、音频和输入流，只负责登录、设备发现、权限校验、STUN/NAT 检测、打洞协调、审计日志和连接质量监控。

核心判断：3M 带宽云服务器不适合作为视频中继。20Mbps 视频流经过中继会产生入口和出口两份带宽，单路会话就远超 3M 上限；服务器资源应该全部留给 KB 级信令和管控。

### 三个核心架构取舍

| 取舍 | 决策 | 理由 |
|------|------|------|
| 要不要中继 | **砍掉** | 3M 带宽做中继只能服务 1-2 人，体验比 ToDesk 差 10 倍，不如不做。服务器 100% 资源用于信令和管控 |
| 要不要 WebRTC | **不用** | WebRTC 是为音视频通话设计的，不支持屏幕编码、不支持键鼠优先级、延迟优化方向不对。自研传输协议更可控 |
| 连接失败怎么办 | **劝退用 ToDesk** | 三级连接覆盖 65-75% 够用了，打洞失败直接提示用户切换网络或用 ToDesk，不浪费服务器资源做劣质中继 |

> **项目定位**：服务器不是"转发流量的中继站"，是整个系统的**管控大脑**——设备发现、NAT 穿透协调、权限验证、审计日志、质量监控。3M 带宽 100% 用于信令和管控，理论上可支撑 5000+ 设备在线。

### 服务器角色定位

| 角色 | 做什么 | 流量 | 不可替代性 |
| ---- | ------ | ---- | ---------- |
| 设备发现与配对 | 维护设备在线状态，帮助控制端找到被控端 | KB 级 | NAT 后设备无法直接被发现 |
| STUN 与 NAT 检测 | 反射公网地址，判断 NAT 类型，交换候选地址 | KB 级 | 两端不知道自己的公网映射 |
| 权限与认证 | 登录、Token、连接密码、RBAC、被控端授权 | KB 级 | P2P 系统也需要集中鉴权 |
| 审计与质量监控 | 记录连接、文件传输、RTT、帧率、带宽 | KB 级 | 纯 P2P 无法做集中管控 |
| 数据维护 | SQLite WAL、定时备份、日志轮转、配置热加载 | 本地 IO | 保障服务器长期运行 |

服务器定位为管控中心，不做 TURN 中继。连接失败时提示当前网络不支持直连，建议切换网络或使用成熟远控工具；不要用低带宽服务器硬撑劣质中继。

### 整体架构

```
┌──────────────────────────────────────────────────────────┐
│            公网管控服务器（2核2G 3M，纯管控不中继）        │
│                                                          │
│  ┌──────────┐ ┌──────────┐ ┌──────────────────────────┐ │
│  │ 信令服务  │ │ STUN 服务 │ │ 管控平台                  │ │
│  │ 登录配对  │ │ NAT 检测  │ │ 用户系统 + 设备管理       │ │
│  │ 在线状态  │ │ 地址反射  │ │ RBAC 权限 + 审计日志      │ │
│  │ 打洞协调  │ │          │ │ 质量监控 + 动态降级       │ │
│  └──────────┘ └──────────┘ │ SQLite 备份 + 配置热加载  │ │
│                              │ Web 管理后台              │ │
│                              └──────────────────────────┘ │
│                                                          │
│  3M 带宽全部用于信令、心跳、状态上报，零视频中继           │
└──────────────────────────────────────────────────────────┘
       ↑ TLS 信令（KB 级）          ↑ TLS 信令（KB 级）
       │                            │
┌──────┴───────┐              ┌──────┴───────┐
│   控制端      │              │   被控端      │
│              │              │              │
│ L1 局域网检测 │              │ L1 局域网检测 │
│ L2 UPnP 映射 │              │ L2 UPnP 映射 │
│ L3 UDP 打洞  │←── P2P ─────→│ L3 UDP 打洞  │
│              │ ECDH +       │              │
│ 失败 → 提示  │ ChaCha20     │              │
│ 切换网络/工具 │              │              │
└──────────────┘              └──────────────┘
```

### 连接建立流程

```
控制端输入设备 ID
  │
  ├──→ 服务器：查询在线状态 + 校验用户权限
  │     ├── 离线/无权 → 拒绝
  │     └── 在线+有权 → 继续
  │
  ├──→ 服务器：STUN 反射双方公网地址 + 检测 NAT 类型
  │     ├── NAT 组合不兼容 → 跳过打洞，直接提示
  │     └── NAT 组合兼容 → 继续
  │
  ├──→ L1：局域网直连检测
  │     ├── 成功 → 直连 192.168.x.x，延迟 <5ms
  │     └── 失败 → 继续
  │
  ├──→ L2：UPnP 端口映射
  │     ├── 成功 → 公网直连，延迟 <30ms
  │     └── 路由器不支持 → 继续
  │
  ├──→ L3：UDP 打洞
  │     ├── 成功 → P2P 直连，延迟 <30ms
  │     └── 超时 3 秒 → 失败提示
  │
  └──→ 当前网络无法建立直连：可能是对称型 NAT 或 CGNAT
```

| 级别 | 连接方式 | 预期覆盖 | 延迟 | 说明 |
| ---- | -------- | -------- | ---- | ---- |
| L1 | 局域网直连 | ~10% | <5ms | 同网段设备直接发现 |
| L2 | UPnP 映射 | ~15% | <30ms | 路由器自动开端口映射 |
| L3 | UDP 打洞 | ~40-50% | <30ms | STUN 协调后两端互发 UDP |
| 失败 | 提示切换网络/工具 | ~25-35% | 无 | 对称型 NAT、CGNAT 无法稳定直连 |

### 打洞成功率矩阵

打洞能否成功取决于两端的 NAT 类型组合：

| 控制端 \ 被控端 | Full Cone | IP Restricted | Port Restricted | Symmetric | CGNAT |
|-----------------|-----------|---------------|-----------------|-----------|-------|
| **Full Cone** | ✅ 95% | ✅ 90% | ✅ 85% | ✅ 80% | ❌ 5% |
| **IP Restricted** | ✅ 90% | ✅ 85% | ✅ 80% | ✅ 70% | ❌ 5% |
| **Port Restricted** | ✅ 85% | ✅ 80% | ✅ 75% | ❌ 10% | ❌ 0% |
| **Symmetric** | ✅ 80% | ✅ 70% | ❌ 10% | ❌ **0%** | ❌ 0% |
| **CGNAT** | ❌ 5% | ❌ 5% | ❌ 0% | ❌ 0% | ❌ 0% |

**关键事实**：对称型 + 对称型 = 0% 成功率。中国运营商大量使用对称型 NAT 和 CGNAT（尤其 4G/5G），所以中继兜底理论上不可省略——但 3M 带宽做中继也没意义，所以选择劝退。

### 为什么 UPnP 是关键一级

UPnP 让客户端直接告诉路由器"帮我开端口映射"，相当于自动在路由器上开端口转发。开了 UPnP 的路由器，NAT 类型从"对称型"变成"锥形"——打洞成功率从 ~10% 提升到 ~85%。用 miniupnpc 库，约 50 行代码。

### NAT 类型预判

打洞前用 STUN 双请求检测两端 NAT 类型，不兼容直接跳过：

```
1. 客户端向服务器 IP1:Port1 发 STUN 请求 → 返回反射地址 R1
2. 客户端向服务器 IP2:Port2 发 STUN 请求 → 返回反射地址 R2
3. 对比 R1 和 R2 的端口：
   ├── 端口相同 → 锥形 NAT（能打洞）
   └── 端口不同 → 对称型 NAT（大概率打不通）
```

### 为什么不用 WebRTC

| 维度 | WebRTC | 自研协议 |
|------|--------|---------|
| 设计目标 | 音视频通话 | 远程控制（屏幕+键鼠+文件） |
| 延迟 | 100-200ms（为通话优化） | <30ms（为操控优化） |
| 屏幕编码 | 不支持（只有摄像头编码） | 自选（H.264/H.265 硬件编码） |
| 键鼠传输 | 不支持（只有 DataChannel） | 自设计（优先级+可靠性） |
| 依赖体积 | 几十 MB | 几千行 C++ |

### 数据流优先级

三种数据流对可靠性和延迟的要求完全不同，在 P2P 通道中按优先级分队列：

| 数据流 | 数据量 | 可靠性要求 | 延迟要求 | 传输策略 |
|--------|--------|-----------|---------|---------|
| 屏幕画面 | 大（100-200KB/帧） | 低（丢一帧等下一帧） | 中（33ms 内） | UDP 低优先级，不重传 |
| 键鼠输入 | 极小（几十字节） | **高**（丢一个点击=误操作） | **极高**（>100ms 卡顿） | UDP 高优先级，序列号+ACK+重传 |
| 文件传输 | 大 | **高**（丢一个字节文件损坏） | 低 | UDP 分块+ACK+重传 |

```
应用层：键鼠事件（高优先级可靠）──┐
应用层：屏幕帧（低优先级可丢）  ──┤
应用层：文件传输（可靠）        ──┘
                                   ↓
                          自定义可靠 UDP 协议
                          ├── 序列号 + ACK + 重传
                          ├── 优先级队列（键鼠 > 文件 > 屏幕）
                          └── 拥塞控制（简易版）
                                   ↓
                              UDP 打洞 P2P
```

### 信道与加密

| 通道 | 传什么 | 加密方案 | 谁能解密 |
| ---- | ------ | -------- | -------- |
| 信令通道 | 登录、设备 ID、打洞地址、权限验证 | TLS 1.2+ | 服务器能解密 |
| P2P 数据通道 | 屏幕、音频、键鼠、文件、剪贴板 | ECDH + ChaCha20-Poly1305 | 只有两端能解密 |
| 状态上报 | 在线状态、质量指标、心跳 | 复用 TLS | 服务器能解密 |

P2P 不走 TLS，因为直连双方没有传统意义上的"服务器证书"角色。两端在信令阶段交换临时公钥，打洞成功后用 ECDH 协商会话密钥，之后的 UDP 数据包用 ChaCha20-Poly1305 做认证加密。**选 ChaCha20 而不是 AES，是因为 ChaCha20 在没有 AES 硬件加速的 ARM 设备上更快，且 ChaCha20-Poly1305 是 AEAD 同时保证机密性和完整性。**

> **技术决策说明**
>
> **为什么是这个方案**：公网服务器带宽只有 3M，视频中继会把单路 10-20Mbps 码流放大成入口和出口两份流量，物理上扛不住。把服务器限定为信令、STUN、权限、审计和质量监控，能支撑大量设备在线，也能保留远控系统最有价值的管控能力。
>
> **底层是什么**：NAT 穿透的本质是让两个 NAT 后的端点先通过服务器知道彼此的公网映射，再同时向对方发 UDP 包，在 NAT 表里打出允许对端返回的映射项。对称型 NAT 会针对不同目标分配不同端口，对端无法稳定预测，所以这类组合要快速失败。
>
> **如果是你你会怎么学**：先看 STUN 的 Binding Request/Response 和 NAT 映射概念，再看 miniupnpc 的端口映射流程，最后再读 ICE；不要一开始就啃完整 WebRTC。

### 服务器功能拆分（11 个模块）

**模块 ① 用户系统**

| 功能 | 说明 | 技术实现 |
|------|------|---------|
| 注册 | 用户名+密码，bcrypt 存储 | TCP 协议 + SQLite |
| 登录 | 验证密码，返回 Token | Token 用 UUID |
| 自动登录 | Token 持久化 | 存本地配置文件 |
| 修改密码 | 验证旧密码后改新 | bcrypt |
| 退出登录 | 清除 Token，断开连接 | — |
| 会话保活 | Token 过期 + 心跳续期 | 心跳 30 秒 |
| 多设备在线 | 同一用户多端同时在线 | user → device 1:N |

**模块 ② 设备管理**

| 功能 | 说明 | 技术实现 |
|------|------|---------|
| 设备注册 | 被控端启动自动注册 | UUID 生成 device_id |
| 地址簿 | 设备列表（别名/分组/备注） | SQLite CRUD |
| 添加/删除设备 | 输入 device_id + 别名 | — |
| 设备分组 | 按分组管理 | group 字段 |
| 在线状态 | 实时在线/离线 | 心跳 + 推送 |
| 最近连接 | 按时间排序 | 连接日志查询 |
| 收藏设备 | 常用置顶 | favorite 标记 |
| 配置同步 | 地址簿存服务器，换电脑不重配 | 服务器端存储 |

**模块 ③ 连接管理**

| 功能 | 说明 | 技术实现 |
|------|------|---------|
| 局域网直连 | 同网段自动检测 | UDP 广播发现 |
| UPnP 映射 | 自动开路由器端口 | miniupnpc |
| UDP 打洞 | STUN + P2P 直连 | 自实现 ICE 简化版 |
| NAT 类型预判 | 打洞前检测兼容性 | STUN 双请求对比 |
| 版本兼容检查 | 连接时交换版本号 | 不兼容直接拒绝 |
| 断线重连 | 网络抖动自动恢复 | 指数退避 1s→2s→4s→8s |
| 连接状态显示 | 显示连接模式 | lan/upnp/p2p |

**模块 ④ 远程桌面**

| 功能 | 说明 | 技术实现 |
|------|------|---------|
| 屏幕采集 | 被控端截屏 + 增量更新 | DXGI / D3D11 |
| 屏幕编码 | 压缩屏幕帧 | NVENC H.264/H.265 |
| 屏幕传输 | UDP 低优先级可丢帧 | — |
| 屏幕解码 | 控制端硬解渲染 | D3D11VA + HLSL |
| 键鼠捕获 | 控制端捕获事件 | SetWindowsHookEx + RAWINPUT |
| 键鼠注入 | 被控端模拟输入 | SendInput |
| 键鼠传输 | UDP + 序列号 + ACK | 高优先级可靠通道 |
| 剪贴板同步 | 双向剪贴板 | 信令通道传输 |
| 文件传输 | 分块 + ACK + 重传 | — |
| 快捷键透传 | Ctrl+Alt+Del 等 | — |

**模块 ⑤ 安全加密**

| 功能 | 说明 | 技术实现 |
|------|------|---------|
| TLS 信令加密 | 客户端↔服务器 | OpenSSL |
| ECDH 端到端加密 | 控制端↔被控端 | Curve25519 |
| 对称加密 | 加密 UDP 数据包 | ChaCha20-Poly1305 |
| 密码存储 | 用户密码不明文 | bcrypt |
| 连接密码 | 二次验证 | hash 验证 |
| Token 管理 | 登录态保活 | UUID + 心跳 |

**模块 ⑥ 权限控制**

| 功能 | 说明 | 技术实现 |
|------|------|---------|
| 权限级别 | full / view_only / file_only | RBAC |
| 权限分配 | 管理员分配用户→设备 | — |
| 权限验证 | 连接前服务器拦截 | — |
| 被控端授权 | 弹窗"是否允许控制" | 首次确认+记住 |
| 离线通知 | 被控端上线推送通知 | 服务器推送 |

**模块 ⑦ 审计日志**

| 功能 | 说明 | 技术实现 |
|------|------|---------|
| 连接日志 | 谁/何时/连了谁/多久 | SQLite + spdlog |
| 文件传输日志 | 传了什么/多大 | — |
| 权限变更日志 | 谁改了权限 | — |
| 日志查询 | 按时间/用户/设备 | — |
| 日志轮转 | spdlog 50MB 轮转，保留 10 个 | — |
| 自动清理 | 90 天前审计记录自动删 | 定时任务 |

**模块 ⑧ 质量保障**

| 功能 | 说明 | 技术实现 |
|------|------|---------|
| RTT 测量 | 每 2 秒测往返延迟 | 时间戳差值 |
| 帧率统计 | 实时帧率上报 | 计数器 |
| 带宽统计 | 上下行带宽 | 字节计数 |
| 动态降帧 | RTT>100ms 降帧率 | 策略引擎 |
| 动态降分辨率 | RTT>200ms 降分辨率 | 策略引擎 |
| 心跳超时清理 | 90 秒无心跳标记离线 | 超时机制 |
| 网络断开检测 | 心跳超时判断 | — |

**模块 ⑨ 数据库维护**

| 功能 | 说明 | 技术实现 |
|------|------|---------|
| WAL 模式 | 读写不互斥，崩溃恢复 | SQLite PRAGMA |
| 定时备份 | 每天凌晨 3 点 | crontab + .backup |
| 完整性校验 | 备份后验证 | PRAGMA integrity_check |
| 备份保留 | 只留 7 天 | find -mtime +7 -delete |
| 日志清理 | 审计 90 天自动清 | 定时任务 |
| 数据导出 | dump 为 SQL 文本 | sqlite3 dump |

**模块 ⑩ 运维**

| 功能 | 说明 | 技术实现 |
|------|------|---------|
| 多线程 | 扛 500+ 并发 | epoll + 线程池 |
| 异步日志 | 不阻塞 IO 线程 | spdlog async |
| 配置管理 | 端口/密码/策略可配置 | JSON 配置文件 |
| 配置热加载 | 不重启改配置 | SIGHUP 信号 |
| 进程守护 | 崩溃自动重启 | systemd |
| 开机自启 | 被控端开机自动运行 | 注册表/systemd/LaunchAgent |
| 异常上报 | 客户端崩溃日志上传 | minidump |

**模块 ⑪ 管理后台**

| 功能 | 说明 | 技术实现 |
|------|------|---------|
| 设备列表 | 查看所有设备状态 | HTTP API + HTML |
| 用户管理 | 查看/禁用用户 | — |
| 日志查询 | 浏览器查审计日志 | — |
| 权限管理 | 修改权限规则 | — |
| 在线监控 | 实时在线数/连接数 | — |

### 数据库设计（6 张表 + 完整 DDL）

```
users（用户）──→ address_book（地址簿）──→ devices（设备）
    │                                          │
    ├──→ permissions（权限）                   │
    │                                          │
    └──→ audit_logs（审计日志）←───────────────┘
                    │
    connection_metrics（质量记录）←────────────┘
```

```sql
-- 1. 用户表
CREATE TABLE users (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    username    TEXT UNIQUE NOT NULL,
    password    TEXT NOT NULL,           -- bcrypt hash
    role        TEXT DEFAULT 'user',     -- user / admin
    token       TEXT,
    created_at  DATETIME DEFAULT CURRENT_TIMESTAMP
);

-- 2. 设备表
CREATE TABLE devices (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    device_id   TEXT UNIQUE NOT NULL,
    alias       TEXT,
    group_name  TEXT,
    owner_id    INTEGER REFERENCES users(id),
    password    TEXT,                    -- 连接密码 hash
    permission  TEXT DEFAULT 'full',
    is_online   INTEGER DEFAULT 0,
    last_heartbeat DATETIME,
    client_version TEXT,
    created_at  DATETIME DEFAULT CURRENT_TIMESTAMP
);

-- 3. 地址簿
CREATE TABLE address_book (
    user_id     INTEGER REFERENCES users(id),
    device_id   TEXT REFERENCES devices(device_id),
    alias       TEXT,
    is_favorite INTEGER DEFAULT 0,
    last_connect DATETIME,
    PRIMARY KEY (user_id, device_id)
);

-- 4. 权限表
CREATE TABLE permissions (
    user_id     INTEGER REFERENCES users(id),
    target_type TEXT,                    -- device / group
    target_name TEXT,
    permission  TEXT,                    -- full / view_only / file_only
    PRIMARY KEY (user_id, target_type, target_name)
);

-- 5. 审计日志
CREATE TABLE audit_logs (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    user_id     INTEGER,
    device_id   TEXT,
    action      TEXT,                    -- connect / disconnect / file_transfer
    duration    INTEGER,
    ip_address  TEXT,
    created_at  DATETIME DEFAULT CURRENT_TIMESTAMP
);

-- 6. 连接质量记录
CREATE TABLE connection_metrics (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    conn_id     TEXT,
    controller_id INTEGER,
    target_device TEXT,
    rtt_ms      INTEGER,
    fps         INTEGER,
    bandwidth_kbps INTEGER,
    connect_mode TEXT,                  -- lan / upnp / p2p
    created_at  DATETIME DEFAULT CURRENT_TIMESTAMP
);

-- 索引
CREATE INDEX idx_audit_time ON audit_logs(created_at);
CREATE INDEX idx_metrics_time ON connection_metrics(created_at);
CREATE INDEX idx_devices_online ON devices(is_online);

-- 开启 WAL 模式
PRAGMA journal_mode=WAL;
PRAGMA synchronous=NORMAL;
```

**备份策略**：

```bash
# crontab 每天凌晨 3 点备份 + 完整性校验 + 7 天保留
0 3 * * * sqlite3 /data/remote_control.db ".backup '/backup/rc_$(date +\%Y\%m\%d).db'" && \
           sqlite3 /backup/rc_$(date +\%Y\%m\%d).db "PRAGMA integrity_check;" && \
           find /backup -name "rc_*.db" -mtime +7 -delete
```

### 详细步骤

**9.1 账号与设备注册**

- 服务端提供 TLS 信令入口，控制端和被控端先登录拿 Token
- 被控端启动后注册 `device_id`、别名、版本号、内网地址、心跳时间
- 控制端拉取地址簿和在线状态，连接前由服务器校验用户是否有权限
- 被控端首次被连接时弹窗确认，可选择记住授权

**9.2 STUN 与 NAT 类型检测**

- 客户端向服务器两个不同 IP/端口发 STUN 请求，服务器返回反射地址
- 对比两次反射端口：端口稳定说明更接近锥形 NAT，端口变化说明大概率是对称型 NAT
- 两端 NAT 组合明显不兼容时直接失败，不再浪费 3 秒打洞时间

**9.3 UPnP 端口映射**

- 被控端优先尝试 `upnpDiscover` → `GetValidIGD` → `AddPortMapping`
- 映射视频、音频、控制和未来协商端口
- UPnP 成功时服务器把被控端公网地址返回给控制端，控制端直接连接
- UPnP 失败时进入 UDP 打洞

参考 Sunshine `upnp.cpp` 的 `map_upnp_port()`。

**9.4 UDP 打洞与 P2P 建链**

- 服务器把双方公网反射地址、内网地址、连接 token 和临时公钥发给对方
- 双方同时向对方公网地址和内网地址发送 UDP 探测包
- 任一方向收到有效探测包后确认直连路径，并固定该路径为本次会话地址
- 直连建立后执行 ECDH，生成会话密钥，再启动视频、音频、输入和文件通道

**9.5 复用现有串流链路**

- 视频继续复用 `VideoSender` / `VideoReceiver`，只是目标地址从手填 IP 变为连接管理模块产出的 P2P 地址
- 音频继续复用 `AudioSender` / `AudioReceiver`
- 输入短期可继续走可靠控制信道；如果要彻底绕开 TCP 队头阻塞，再迁移到可靠 UDP 子通道
- 第五、六阶段已有的 FEC、IDR/RFI、RTT、丢包率和自适应码率在公网模式下必须打开

**9.6 质量监控与动态降级**

- 控制端每 2 秒上报 RTT、接收 FPS、解码 FPS、渲染 FPS、丢包率和估算带宽
- RTT >100ms 时优先降帧率，RTT >200ms 或连续丢包时降分辨率/码率
- 服务器只记录指标和下发策略，不直接参与业务流转发
- 90 秒无心跳的设备标记离线，断线重连使用指数退避 1s → 2s → 4s → 8s

### 不做中继的边界

| 场景 | 处理方式 | 原因 |
| ---- | -------- | ---- |
| 对称型 NAT + 对称型 NAT | 直接失败提示 | 端口不可预测，打洞成功率接近 0 |
| CGNAT 双端 | 直接失败提示 | 用户没有真实公网映射 |
| 单端 UPnP 成功 | 优先公网直连 | 延迟低，服务端不耗带宽 |
| 普通锥形 NAT 组合 | UDP 打洞 | 覆盖大部分家庭宽带场景 |
| 需要 100% 连通 | 提示使用成熟远控工具 | 3M 云服务器不适合 TURN 中继 |

frp 或 TURN 只能作为临时验证工具，不作为主路线。若未来换成高带宽服务器，可以单独设计中继服务，但那已经是商业化部署问题，不属于当前 3M 服务器方案。

### 参考源码与项目

| 功能 | 参考来源 | 关注点 |
| ---- | -------- | ------ |
| UPnP 端口映射 | Sunshine `upnp.cpp` | `map_upnp_port()` 的发现与映射流程 |
| 应用启动 | Sunshine `process.cpp` | `proc_t::execute()` 的应用生命周期管理 |
| mDNS 发布 | Sunshine `platform/windows/publish.cpp` | 局域网发现可参考 |
| Web 配置 UI | Sunshine `confighttp.cpp` | 管理后台和配置页面 |
| 整体远控架构 | RustDesk / RustDesk Server | 信令、设备、权限和中继边界 |
| STUN/TURN | coturn | STUN 反射与 TURN 中继概念 |
| UPnP | miniupnpc | 路由器端口映射 |

### 评价标准

| 指标 | 目标值 | 测试方法 |
| ---- | ------ | -------- |
| 直连覆盖率 | 身边样本 65-75% 可连 | 不同家庭宽带、手机热点、公司网络测试 |
| 信令带宽 | 3M 服务器稳定承载大量在线设备 | 云监控看入出带宽和连接数 |
| P2P 延迟 | 局域网 <5ms，UPnP/打洞 <30ms | 拍照对比毫秒时钟 |
| 安全性 | 服务器抓不到业务明文 | Wireshark 抓 P2P UDP 包 |
| 失败速度 | NAT 不兼容时 3 秒内给出提示 | 对称型 NAT/CGNAT 环境测试 |
| 数据可靠性 | 审计日志和设备状态不丢 | SQLite WAL + 备份恢复演练 |

### 阶段定位

本阶段把项目从"局域网串流工具"升级为"跨互联网远程控制系统"。它不推翻前八阶段的串流链路，而是在链路前面补一层公网管控和 P2P 建链能力：服务器负责找人、验权、协调连接和记录质量，真正的桌面数据仍然走两端直连。

### 公网远控开发计划（三阶段）

| 阶段 | 目标 | 累计时间 | 核心产出 |
|------|------|---------|---------|
| P0 能用 | 能跨网远程控制 | 8 周 | 身边 3-5 人测试，65-75% 能连上 |
| P1 像产品 | 有用户体系+权限+审计+备份 | 12.5 周 | 能管理 50+ 设备，有权限控制和审计 |
| P2 有深度 | 有质量监控+并发+管理后台 | 17.5 周 | 系统自动调节连接质量，有管理后台 |

**P0：能用（8 周）**

| 任务 | 技术 | 时间 |
|------|------|------|
| 用户注册登录 + Token + bcrypt | C++ + SQLite | 1 周 |
| 设备注册 + 在线状态 + 心跳 | TCP + epoll | 1 周 |
| TLS 信令加密 | OpenSSL | 0.5 周 |
| 信令服务器 + 打洞协调 | C++ + TCP | 1.5 周 |
| STUN 服务 + NAT 类型检测 | coturn 或自写 | 0.5 周 |
| UPnP 端口映射 | miniupnpc | 0.5 周 |
| UDP 打洞 + ECDH 端到端加密 | UDP + Curve25519 | 2 周 |
| 屏幕传输 + 键鼠控制（改已有客户端） | DXGI + 平台 API | 1 周 |

**P1：像产品（+4.5 周）**

| 任务 | 技术 | 时间 |
|------|------|------|
| 地址簿 + 设备分组 + 收藏 | SQLite CRUD | 1 周 |
| RBAC 权限控制 + 连接密码 | 权限模型 | 1 周 |
| 审计日志 + 异步写入 + 轮转 | spdlog | 0.5 周 |
| 断线重连 | 指数退避 | 0.5 周 |
| 剪贴板同步 | 信令通道 | 0.5 周 |
| 数据库 WAL + 定时备份 | crontab | 0.5 周 |
| 被控端授权确认 + 版本检查 | 信令交互 | 0.5 周 |

**P2：面试有深度（+5 周）**

| 任务 | 技术 | 时间 |
|------|------|------|
| 连接质量监控 + 动态降级策略 | 指标采集 + 策略引擎 | 1.5 周 |
| 文件传输 | 分块 + ACK + 重传 | 1 周 |
| 多线程优化 | epoll + 线程池 | 1 周 |
| Web 管理后台 | HTTP + HTML | 0.5 周 |
| 开机自启 + 后台运行 + 进程守护 | systemd/注册表 | 0.5 周 |
| 配置热加载 + 异常上报 | SIGHUP + minidump | 0.5 周 |

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

---

## 第十阶段：面试准备（远程控制系统项目叙事）

> 以下内容整合自远程控制项目面试方案，面向 2 年 C++ 经验、广深求职场景。核心策略：**面试讲取舍不讲技术**——"为什么砍中继"比"我会用 epoll"有说服力 10 倍。

### 一分钟版本（电梯演讲）

> "我做了一个跨互联网远程控制系统，服务器跑在一台 2 核 2G 腾讯云上，支持 500+ 设备在线管理。三级连接策略覆盖 65-75%：局域网直连 + UPnP + UDP 打洞，失败劝退用 ToDesk。砍掉了中继因为 3M 带宽做中继没意义。信令通道 TLS 加密，P2P 数据通道 ECDH+ChaCha20 端到端加密。没用 WebRTC，自研传输协议区分屏幕帧（可丢）和键鼠（可靠）的优先级。服务器聚焦管控：RBAC 权限、审计日志、连接质量监控+动态降级、数据库 WAL+定时备份。11 个模块，6 张表。"

### 完整版本（面试主讲）

> **项目概述**
>
> "我做了一个远程控制系统，服务器跑在一台 2 核 2G 腾讯云上，支持 500+ 设备在线管理。身边 5 人测试 70% 连接成功率。"
>
> **安全设计**
>
> "三条通信通道两层加密。信令通道用 TLS，因为服务器持证书可以做身份验证。P2P 数据通道用 ECDH+ChaCha20-Poly1305 端到端加密，因为直连不经过服务器，服务器看不到业务数据。选 ChaCha20 而不是 AES，是因为 ChaCha20 在没有硬件加速的 ARM 设备上更快。密码用 bcrypt 存储，连接密码二次验证。"
>
> **架构取舍**
>
> "三个关键取舍：
> 1. 砍掉中继——3M 带宽做中继只能服务 1-2 人，体验比 ToDesk 差 10 倍，服务器 100% 用于管控
> 2. 三级连接——局域网直连 + UPnP + UDP 打洞，覆盖 65-75%，失败直接劝退用 ToDesk
> 3. 没用 WebRTC——它是为音视频通话设计的，不支持屏幕编码和键鼠优先级，自研传输协议更可控"
>
> **管控平台**
>
> "11 个模块：用户系统、设备管理+地址簿、RBAC 权限控制、审计日志、连接质量监控+动态降级、断线重连、剪贴板同步、文件传输、数据库 WAL+定时备份、配置热加载、Web 管理后台。6 张 SQLite 表覆盖全部数据模型。"
>
> **性能优化**
>
> "每 2 秒采集 RTT/帧率/带宽，RTT>100ms 自动降帧率，>200ms 降分辨率。打洞前用 STUN 双请求检测 NAT 类型，对称型直接跳过不浪费时间。多线程 epoll 扛 500+ 并发。心��� 90 秒超时清理僵尸连接。SQLite WAL 模式 + 每天定时备份 + integrity_check + 7 天保留。"
>
> **传输协议**
>
> "屏幕帧走 UDP 低优先级，丢帧直接等下一帧不重传。键鼠走 UDP 高优先级，序列号+ACK+重传，几十字节重传成本极低。文件走 UDP 分块+ACK+重传，类似 TCP 滑动窗口。"

### 可能被追问的问题

| 追问 | 回答要点 |
|------|---------|
| 为什么不用 TCP 打洞 | TCP 打洞成功率 ~64% vs UDP 82%，TCP 状态机复杂大部分 NAT 不支持 simultaneous-open |
| 对称型 NAT 为什么打不通 | 对称型 NAT 对每个目标用不同端口，对方猜不到你用哪个端口 |
| 为什么不用 WebRTC | WebRTC 为音视频设计，不支持屏幕编码，延迟优化方向不对，依赖体积大 |
| ChaCha20 和 AES 区别 | ChaCha20 在无 AES 硬件加速时更快，都是 AEAD，安全性相当 |
| SQLite 挂了怎么办 | WAL 模式崩溃恢复 + 每天备份 + integrity_check + 7 天保留 |
| 1000 人同时在线扛得住吗 | 信令极轻量，3M 带宽够 5000+ 设备。多线程 epoll 扛 500+ 并发 |
| 打洞失败 30% 怎么办 | 直接提示用户"当前网络不支持直连，建议使用 ToDesk"。3M 带宽做中继没意义 |
| 为什么砍中继 | 3M 带宽中继 1-2 人就满，体验比 ToDesk 差 10 倍，用户照样走，不如聚焦管控 |
| RBAC 怎么设计的 | 用户→权限→目标（设备/设备组），权限级别 full/view_only/file_only |
| 断线重连怎么做的 | 指数退避 1s→2s→4s→8s，15 秒内尝试重新打洞 |

### 面试前必须内化的 10 个判断

1. **不存在 100% 打洞成功**——对称型+对称型 = 0%，CGNAT 几乎全军覆没
2. **3M 带宽 = 375 KB/s**——不是 3 MB/s，物理上扛不住中转
3. **服务器价值在管控不在转发**——设备管理+权限+审计+质量监控才是核心
4. **UPnP 是被忽略的关键一级**——能把对称型 NAT 变成锥形，成功率从 10% 到 85%
5. **键鼠不能丢但可以走 UDP**——上层自实现序列号+ACK+重传（QUIC 思路）
6. **P2P 直连不经过服务器**——所以用 ECDH 端到端加密，服务器看不到业务数据
7. **不用 WebRTC**——它是为视频通话设计的，不适合远程控制
8. **砍中继是正确取舍**——3M 做中继体验比 ToDesk 差 10 倍，聚焦管控更有价值
9. **SQLite WAL + 定时备份**——2 核 2G 服务器上的数据安全方案
10. **面试讲取舍不讲技术**——"为什么砍中继"比"我会用 epoll"有说服力 10 倍

### 岗位方向

基于猎聘 2026.07 广深 C++ 岗位数据：

| 最匹配方向 | 匹配度 | 优势 | 劣势 |
|-----------|--------|------|------|
| 通信/网络编程 | ⭐⭐⭐⭐ | P2P 打洞+NAT 穿透是核心技能 | 缺 DPDK/内核经验 |
| 通用 C++ 后端 | ⭐⭐⭐ | 多线程+协议+数据库 | 缺 RPC/微服务 |
| 桌面端 | ⭐⭐⭐ | 屏幕采集+键鼠注入 | 缺 Qt/MFC |

**投简历策略**：优先投通信/网络编程方向（华为、中兴、大疆等），P2P 打洞项目是硬通货。

### 参考项目

| 参考什么 | 项目 | 说明 |
|----------|------|------|
| 整体架构 | RustDesk Server | github.com/rustdesk/rustdesk-server |
| 传输协议设计 | RustDesk | github.com/rustdesk/rustdesk |
| STUN/TURN | coturn | github.com/coturn/coturn |
| P2P 打洞+中转 | linker | github.com/snltty/linker |
| epoll 基础 | WebServer-master | github.com/hello-wxt/WebServer-master |
| UPnP | miniupnp | github.com/miniupnp/miniupnp |
| 加密 | OpenSSL / libsodium | github.com/openssl/openssl |