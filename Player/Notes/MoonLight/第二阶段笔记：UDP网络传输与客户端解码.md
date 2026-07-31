# 第二阶段笔记：UDP 网络传输 + 客户端解码显示

> 本阶段目标：将第一阶段编码出的 H.264 数据通过 UDP 从服务端发送到客户端，客户端解码并显示。
> 验收方式：本机跑两个 Player 实例（一个发一个收），端到端延迟 < 30ms。

---

## 整体架构

```
服务端（被控制端）                                客户端（控制端）
┌───────────────────────────────┐            ┌───────────────────────────────┐
│                               │            │                               │
│  MonitorCapture               │            │  VideoReceiver                │
│  (DXGI/WGC/GDI → GPU 纹理)    │            │  (UDP recv → NalReassembler   │
│         │                     │            │   → AVPacket)                 │
│         ▼                     │            │         │                     │
│  ObsNvencEncoder              │    UDP     │         ▼                     │
│  (GPU→NV12→NVENC→H.264)       │ ─────────→ │  VideoDecoder (D3D11VA 硬解)  │
│         │                     │  47998     │         │                     │
│         ▼                     │            │         ▼                     │
│  VideoSender                  │            │  FFmpegPlayer::DecodeLoop     │
│  (FragmentFrame → sendto)     │            │  (低延迟模式：收到帧立刻渲染)  │
│                               │            │         │                     │
│         │                     │            │         ▼                     │
│  StreamServer (主循环)         │            │  StreamWindow → D3D11 Present │
│                               │            │                               │
└───────────────────────────────┘            └───────────────────────────────┘
```

### 数据流：从屏幕像素到客户端显示

```
[服务端]                                    [客户端]
显示器画面
  │
  ▼ DXGI Desktop Duplication
ID3D11Texture2D (GPU, BGRA)
  │
  ▼ CopyResource → staging texture → Map
CPU BGRA 数据
  │
  ▼ sws_scale (BGRA → NV12)
CPU NV12 数据
  │
  ▼ avcodec_send_frame + avcodec_receive_packet
H.264 Annex B 码流 (一帧)
  │
  ▼ FragmentFrame (按 1400 字节分片)
多个 UDP 包 (NalPacketHeader + payload)
  │
  ▼ sendto ──────── 网络 ────────→  recvfrom
                                          │
                                          ▼ NalReassembler.AddPacket
                                    完整 H.264 帧 (std::vector<uint8_t>)
                                          │
                                          ▼ av_new_packet + memcpy
                                    AVPacket*
                                          │
                                          ▼ packet_queue_.Push
                                    SafeQueue<AVPacket*>
                                          │
                                          ▼ VideoDecoder::VideoDecodeLoop
                                    avcodec_send_packet → avcodec_receive_frame
                                          │
                                          ▼ frame_queue_.PushMax
                                    SafeQueue<AVFrame*>
                                          │
                                          ▼ FFmpegPlayer::DecodeLoop (串流路径)
                                    video_renderer_->Render(frame)
                                          │
                                          ▼ D3D11 交换链 Present
                                    客户端屏幕显示
```

---

## 新增文件结构

```
Player/
├── Common/
│   └── NetWork/                          ← 新增：网络传输层
│       ├── NalUnit.h                     ← NAL 分片/组帧协议（纯头文件，header-only）
│       ├── VideoSender.h/.cpp            ← UDP 发送器（服务端用）
│       └── VideoReceiver.h/.cpp          ← UDP 接收器（客户端用）
│
├── Server/
│   ├── OBS_Capture/                      ← 第一阶段已有
│   │   ├── MonitorCapture.h/.cpp
│   │   ├── ObsNvencEncoder.h/.cpp
│   │   └── ...
│   └── StreamServer.h/.cpp               ← 新增：服务器模式封装（采集→编码→发送）
│
├── Client/
│   ├── Core/
│   │   └── FFmpegPlayer.h/.cpp           ← 修改：新增 OpenStream() 串流模式
│   ├── MainUI/                           ← 文件播放器 UI（不动）
│   │   ├── MainWindow.h/.cpp
│   │   ├── VideoWidget.h/.cpp
│   │   └── ...
│   └── StreamUI/                         ← 新增：串流客户端 UI（独立于播放器）
│       ├── StreamWindow.h/.cpp           ← 串流主窗口（TitleBar + 视频 + 状态栏）
│       └── StreamVideoWidget.h/.cpp      ← 串流视频显示组件
│
├── App/
│   ├── main.cpp                          ← 修改：三模式分流
│   └── PlayerApp.h/.cpp                  ← 修改：拆分文件模式 / 串流模式
│
└── Player.vcxproj                        ← 修改：添加新文件条目
```

---

## 模块一：NAL 分片协议 (NalUnit.h)

### 为什么需要分片

H.264 编码器输出的一帧数据（AVPacket）通常远大于 UDP 的 MTU（1500 字节）。一个 1080p@60fps 的关键帧可能有 50KB~200KB，直接 sendto 会触发 IP 层分片，丢一个分片整帧就废了。

**解决方案**：在应用层手动分片，每个 UDP 包 ≤ 1400 字节（留余量给 IP/UDP 头），接收端组装还原。

### 包头设计

```
UDP 包结构：
┌────────────────────────────┬─────────────────────────┐
│  NalPacketHeader (9 字节)   │  Payload (≤ 1400 字节)  │
└────────────────────────────┴─────────────────────────┘

NalPacketHeader 布局（#pragma pack(1) 紧凑排列）：
┌────────┬──────────────┬───────────────┬──────────────┐
│ flags  │ frame_index  │ packet_index  │  timestamp   │
│ 1 字节  │   2 字节      │   2 字节       │   4 字节     │
└────────┴──────────────┴───────────────┴──────────────┘

flags 位定义：
  bit 0 (FLAG_SOF)      : 帧起始分片标记
  bit 1 (FLAG_EOF)      : 帧结束分片标记
  bit 2 (FLAG_KEYFRAME) : 关键帧（IDR）标记
  bit 3~7               : 保留
```

**与 Moonlight 原版设计的差异**：

| 设计点 | Moonlight 原版 | 本项目 | 原因 |
|--------|---------------|--------|------|
| 包头大小 | 24+ 字节（含 RTP 头） | 9 字节 | 去掉 RTP 头，局域网不需要 RTSP 协商 |
| 序列号 | 全局递增 seq | frame_index + packet_index | 双层索引，组帧时更直观 |
| 流类型 | 音频/视频/控制复用 | 纯视频 | 简化协议，第二阶段只传视频 |
| 乱序处理 | 滑动窗口 + ACK | std::map 自动排序 | 局域网丢包率极低，不需要重传 |

### 分片逻辑 (FragmentFrame)

```
输入：一帧 H.264 数据 (frame_data, frame_size)
输出：std::vector<std::vector<uint8_t>>（每个元素是一个完整 UDP 包）

算法：
  packet_count = ceil(frame_size / 1400)
  如果 frame_size == 0，packet_count = 1（空帧也发一个包，保证 SOF+EOF）

  遍历每个分片 i：
    offset = i * 1400
    payload_size = min(1400, frame_size - offset)

    构建包头：
      flags = 0
      if i == 0:         flags |= FLAG_SOF       // 首片
      if i == last:      flags |= FLAG_EOF       // 末片
      if is_keyframe:    flags |= FLAG_KEYFRAME  // 关键帧所有分片都带标记

    组装：[header(9B)] + [payload(≤1400B)]
```

### 组帧逻辑 (NalReassembler)

**核心设计**：用 `std::map<uint16_t, std::vector<uint8_t>>` 存储分片，map 按 key 自动升序排列，收齐后遍历 map 拼接即可得到有序数据。

```
AddPacket(packet_data, packet_size):
  1. 解析 NalPacketHeader
  2. 判断是否需要切换到新帧：
     - frame_index 不同 → 新帧（上一帧可能丢包了，丢弃）
     - 同 frame_index 但收到 SOF 且上一帧已 EOF → 新一轮
     - 同 frame_index 且上一帧已 EOF → 重复包，丢弃
  3. 如果是新帧：清空 packet_map_，记录 frame_index / timestamp / keyframe
  4. packet_map_[packet_index] = payload   // 按 index 存储，支持乱序到达
  5. 如果 flags & FLAG_EOF：
     - 遍历 packet_map_（已按 key 升序），拼接所有 payload
     - 返回 true（帧完整）

TakeFrame():
  - 返回拼接后的 frame_buffer_，清空内部状态
```

**为什么要用 map 而不是 vector**：

UDP 包到达顺序不保证。如果用 vector 按"到达顺序"存储，拼接时会乱序。用 `map<packet_index, payload>`，利用 map 的自动排序特性，收到 EOF 后遍历 map 即可按正确顺序拼接。

### 自测函数 (NalUnitSelfTest)

NalUnit.h 内嵌了 4 个测试用例，可在 main 中调用验证：

| 测试 | 场景 | 验证点 |
|------|------|--------|
| 测试 1 | 3000 字节帧 → 3 个分片 → 重组 | 分片数量、SOF/EOF 标志位、数据完整性 |
| 测试 2 | 100 字节小帧 → 1 个分片 | SOF+EOF 同时设置、单包立即完成 |
| 测试 3 | 旧帧未完成时收到新帧 | 帧切换逻辑、旧帧残留被丢弃 |
| 测试 4 | 包乱序到达（1,0,2 顺序） | map 排序拼接、数据正确性 |

---

## 模块二：UDP 发送器 (VideoSender)

### 设计

```cpp
class VideoSender
{
    bool Init(const char* dest_ip, uint16_t dest_port);
    void SendFrame(const uint8_t* frame_data, int frame_size,
                   uint16_t frame_index, uint32_t timestamp, bool is_keyframe);
    void Close();
};
```

### 关键实现

```
Init:
  1. WSAStartup(MAKEWORD(2,2))              // 初始化 Winsock
  2. socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)  // 创建 UDP socket
  3. inet_pton 填充 dest_addr_
  4. setsockopt SO_SNDBUF = 4MB             // 加大发送缓冲区，避免大帧丢包

SendFrame:
  1. FragmentFrame() 分片
  2. for each packet: sendto()              // 逐片发送，不阻塞
```

**为什么不用 Qt 的 QUdpSocket**：
Qt 网络层（QAbstractSocket）内部有信号槽队列和事件循环开销，对于逐包 sendto 的场景，原生 Winsock2 的 `sendto()` 延迟更低、更可控。服务器模式甚至不走 Qt 事件循环（见后文）。

**为什么发送缓冲区设 4MB**：
默认 Windows UDP 发送缓冲区只有 64KB。一个 1080p 关键帧约 100KB，分 70+ 个 UDP 包，瞬间 burst 可能超过默认缓冲区导致丢包。4MB 足够缓存一秒的帧数据。

---

## 模块三：UDP 接收器 (VideoReceiver)

### 设计

```cpp
class VideoReceiver
{
    bool Init(uint16_t listen_port, SafeQueue<AVPacket*>* packet_queue);
    void Start();                           // 启动接收线程
    void Stop();                            // 停止接收线程
};
```

### 接收线程主循环

```
ReceiveLoop:
  recv_buf[2048]                            // 接收缓冲区（最大 UDP 包 + 余量）

  while running:
    1. recvfrom(sock, recv_buf)             // 阻塞等待
       如果返回 ≤ 0 → socket 关闭或出错，退出

    2. reassembler_.AddPacket(recv_buf)     // 交给组帧器
       如果返回 false → 还没收齐，继续等

    3. frame_data = reassembler_.TakeFrame() // 取走完整帧

    4. 包装成 AVPacket：
       pkt = av_packet_alloc()
       av_new_packet(pkt, frame_data.size())
       memcpy(pkt->data, frame_data.data(), size)

       // 关键帧标记
       if reassembler_.IsKeyFrame():
         pkt->flags |= AV_PKT_FLAG_KEY

       // PTS：毫秒时间戳 → 90kHz 时基（与 H.264 约定一致）
       pkt->pts = timestamp * 90
       pkt->dts = pkt->pts

    5. packet_queue_->Push(pkt)             // 推入队列给 VideoDecoder 消费
```

**PTS 为什么要乘 90**：
H.264 的标准时基是 90kHz（即 90000 ticks/秒）。服务端 timestamp 单位是毫秒，乘以 90 就转换为 90kHz 时基下的 ticks。这样解码器输出的 AVFrame->pts 和 OpenStream 时设置的 `video_tb = {1, 90000}` 配合，可以正确计算显示时间。

### 线程安全与退出机制

```
Stop():
  1. running_ = false
  2. closesocket(sock_)                     // 关键：让阻塞的 recvfrom 立即返回
  3. recv_thread_.join()                    // 等待线程退出
```

**为什么先 closesocket 再 join**：
`recvfrom` 是阻塞调用，线程卡在里面。如果不关 socket，join 会死等。`closesocket` 会让 `recvfrom` 返回 `SOCKET_ERROR`（返回值 ≤ 0），线程跳出循环退出。

### 与现有架构的集成点

```
原管线（文件模式）：
  Reader → video_packet_queue_ → VideoDecoder → video_frame_queue_ → DecodeLoop → Render

新管线（串流模式）：
  VideoReceiver → video_packet_queue_ → VideoDecoder → video_frame_queue_ → DecodeLoop → Render
                  ↑ 同一个队列                                    ↑ 同一个循环
```

**关键设计**：VideoReceiver 输出的 AVPacket 推入的 `video_packet_queue_` 就是 VideoDecoder 消费的队列。VideoDecoder 不关心数据来源是文件还是网络，它只管从队列 Pop 包然后解码。这种"队列解耦"让网络模块可以无缝插入现有管线。

---

## 模块四：FFmpegPlayer 串流模式

### 新增方法

```cpp
bool FFmpegPlayer::OpenStream(uint16_t port, int width, int height, int fps);
```

### OpenStream 与 OpenFile 的区别

| 对比项 | OpenFile | OpenStream |
|--------|----------|------------|
| 数据来源 | Reader (avformat_open_input) | VideoReceiver (UDP recvfrom) |
| AVCodecParameters | 从 AVFormatContext->streams[i]->codecpar 获取 | 手动构造（codec_id=H264, width, height, NV12） |
| 音频 | 有（AudioDecoder + AudioRenderer） | 无 |
| DecodeLoop 路径 | 音视频同步（frame_timer + audio_clock） | 低延迟路径（收到帧立刻渲染） |
| 时基 | 从 AVStream->time_base 获取 | 固定 {1, 90000} |

### 手动构造 AVCodecParameters

串流没有 AVFormatContext，需要手动告诉解码器编码格式：

```cpp
AVCodecParameters* par = avcodec_parameters_alloc();
par->codec_type = AVMEDIA_TYPE_VIDEO;
par->codec_id   = AV_CODEC_ID_H264;
par->width      = width;
par->height     = height;
par->format     = AV_PIX_FMT_NV12;          // NVENC 输出 NV12

video_decoder_.OpenVideo(par, true);         // 尝试 D3D11VA 硬解
```

**为什么需要命令行传 width/height**：
H.264 的 SPS/PPS 里虽然包含分辨率，但 `OpenStream()` 需要在收到第一帧之前就初始化解码器和 D3D11 渲染管线（创建交换链需要 width/height）。命令行参数是最简单的方案——两端用相同参数启动即可。后续可以优化为协议握手协商。

### DecodeLoop 低延迟路径

```cpp
while (playing_)
{
    if (is_streaming_)
    {
        // ---- 串流模式：收到帧立刻渲染，跳过所有同步逻辑 ----
        AVFrame* video_frame = video_frame_queue_.Pop();
        if (!video_frame) break;

        video_renderer_->Render(video_frame);
        av_frame_free(&video_frame);
        continue;                           // 不走下面的音视频同步
    }

    // ---- 文件模式：音视频同步逻辑（frame_timer + audio_clock） ----
    // ...
}
```

**为什么串流模式跳过音视频同步**：
1. 串流没有音频，没有音频时钟可参考
2. 串流的"正确行为"是尽快显示最新帧，而不是按 PTS 排队等待
3. Moonlight 的 Pacer 模块也是类似思路：收到帧后只做极短的平滑（1~2ms），不做长缓冲同步

### Play/Stop/Close 的串流分支

```cpp
Play():
  if (is_streaming_):
    video_receiver_->Start()                // 启动 UDP 接收线程
    video_decoder_.Start()                  // 启动解码线程
  else:
    reader_.Start()                         // 启动文件读取线程
    video_decoder_.Start()
    audio_decoder_.Start()

Stop():
  if (is_streaming_):
    video_receiver_->Stop()
    video_decoder_.Stop()
  else:
    reader_.Stop()
    video_decoder_.Stop()
    audio_decoder_.Stop()

Close():
  Stop()
  video_renderer_->Release()
  if (video_receiver_): delete video_receiver_
```

---

## 模块五：服务器模式封装 (StreamServer)

### 设计

```cpp
class StreamServer
{
    bool Init(uint16_t port, int monitor_index,
              const char* dest_ip = "127.0.0.1",
              int fps = 60, int bitrate_kbps = 10000);
    void Run();                              // 阻塞主循环
    void Stop();                             // 设置退出标志
};
```

### 主循环

```
Run:
  frame_duration = 1000ms / fps              // 帧间隔（微秒精度）
  frame_index = 0

  while running:
    frame_start = now()

    // 第一步：采集
    capture_->Tick(1.0f / fps)
    capture_->GetFrame(frame)

    // 第二步：获取纹理
    if frame.IsGpu():
      tex = frame.gpu_texture                // DXGI/WGC 路线
    else:
      d3d_ctx->UpdateSubresource(upload_tex, frame.cpu_data)  // GDI 路线上传
      tex = upload_tex

    // 第三步：编码 + 发送
    encoder_->EncodeFrame(tex, frame_index, ...)
    sender_->SendFrame(h264_data, frame_index, timestamp, is_keyframe)

    frame_index++

    // 帧率限制
    elapsed = now() - frame_start
    if elapsed < frame_duration:
      sleep(frame_duration - elapsed)
```

### 生命周期管理

```
Init:
  1. D3D11CreateDevice (VIDEO_SUPPORT)
  2. MonitorCapture::EnumerateMonitors() → 选指定索引
  3. MonitorCapture::Init()
  4. 等首帧到达（最多约 5 秒）
  5. ObsNvencEncoder::Init(width, height, fps, bitrate)
  6. VideoSender::Init(dest_ip, port)
  7. 如果 GDI 路线：CreateTexture2D (上传纹理)

~StreamServer（析构）:
  逆序释放：sender → encoder → capture → upload_tex → d3d_ctx → d3d_device
```

---

## 模块六：应用入口三模式分流

### main.cpp 结构

```
main(argc, argv):
  CoInitializeEx()

  if HasArg("--server"):
    // ---- 服务器模式：纯控制台，不走 Qt ----
    解析 --port / --monitor / --ip / --fps / --bitrate
    SetConsoleCtrlHandler(Ctrl+C → server.Stop())
    StreamServer server
    server.Init(...)
    server.Run()                             // 阻塞直到 Stop
    return 0

  // ---- 播放器 / 串流模式：需要 Qt 事件循环 ----
  QApplication app(argc, argv)
  PlayerApp player_app
  player_app.Init(argc, argv)                // 内部检测 --stream
  return app.exec()
```

### PlayerApp 双模式初始化

```
Init(argc, argv):
  LoadStyle()

  if 命令行含 --stream:
    // ---- 串流客户端模式 ----
    is_streaming_ = true
    CreateStreamUI()                         // 创建 StreamWindow
    CreateStreamPlayer()                     // 创建 FFmpegPlayer，绑定到 StreamVideoWidget
    BindStreamSignals()                      // 绑定关闭信号
    OpenStream(port, width, height, fps)     // 初始化 UDP 接收 + 自动播放
    return

  // ---- 文件播放器模式 ----
  CreatePlayerUI()                           // 创建 MainWindow
  CreatePlayer()                             // 创建 FFmpegPlayer，绑定到 VideoWidget
  BindPlayerSignals()                        // 绑定播放/暂停/进度条/文件浏览器
  return
```

### 三种启动命令

```bash
# 1. 文件播放器（默认）
Player.exe

# 2. 服务器模式（控制台）
Player.exe --server --port 47998 --monitor 1 --ip 127.0.0.1 --fps 60 --bitrate 10000

# 3. 客户端串流模式（Qt 窗口）
Player.exe --stream --port 47998 --width 1920 --height 1080 --fps 60
```

---

## 模块七：串流 UI (StreamUI)

### 为什么不复用 MainWindow

MainWindow 和播放器"过拟合"了——内嵌了 TitleBar + VideoWidget + ControlBar + FileBrowser，全是播放器专属控件。串流模式不需要 ControlBar（无暂停/拖动/音量）、不需要 FileBrowser、不需要 SeekBar。

### StreamWindow 布局

```
┌──────────────────────────────────────┐
│  TitleBar ("Stream")                  │  ← 复用播放器的 TitleBar
├──────────────────────────────────────┤
│                                      │
│                                      │
│       StreamVideoWidget              │  ← 独立组件，提供 HWND + QLabel 软解回退
│       (D3D11 交换链渲染)              │
│                                      │
│                                      │
├──────────────────────────────────────┤
│  状态栏: "已连接  1920x1080@60fps"    │  ← QLabel，显示连接状态/分辨率/帧率
└──────────────────────────────────────┘
```

### StreamVideoWidget

与播放器的 VideoWidget 功能几乎相同（提供 HWND + QLabel 软解回退），但独立存在于 `StreamUI/` 目录。原因：后续串流 UI 可能加入鼠标/键盘事件转发（远程控制），这些是播放器的 VideoWidget 不该有的职责。

### 全屏模式

- 按 F 键切换全屏/窗口
- 全屏时隐藏 TitleBar 和状态栏
- 鼠标移动恢复 TitleBar，闲置 2 秒后自动隐藏

---

## 技术决策说明

### 1. 为什么服务器模式不走 Qt 事件循环

**为什么是这个方案**：
服务器只需要"采集→编码→发送"的固定循环，没有 UI 交互。跳过 `app.exec()` 省去了 QThread 信号槽的开销，帧率控制更精确。

**底层是什么**：
Win32 控制台程序模型——`main()` 直接跑业务循环，用 `std::this_thread::sleep_for` 做帧率限制。比 Qt 事件驱动更适合"固定帧率推送"场景，因为 Qt 事件循环的定时器精度受系统消息队列影响，在高负载时可能抖动。

**如果是你你会怎么学**：
看 MSDN 的 Console Applications 文档，对比 Win32 消息循环（GetMessage/DispatchMessage）和 Qt 事件循环（QEventLoop::exec）的差异。理解为什么游戏引擎和媒体服务器通常不用 Qt 事件循环。

### 2. 为什么用原生 Winsock2 而不是 QUdpSocket

**为什么是这个方案**：
Qt 网络层（QAbstractSocket）内部有信号槽队列和事件循环开销。对于逐包 `sendto`/`recvfrom` 的场景，原生 Winsock2 延迟更低。

**底层是什么**：
`sendto()` 是 Winsock2 的直接系统调用，数据从用户态 buffer 直接到内核网络栈。QUdpSocket 的 `writeDatagram()` 内部也是调 `sendto`，但中间经过 Qt 的 socket notifier 和信号槽机制，多了一层拷贝和事件投递。

**如果是你你会怎么学**：
对比 Qt 源码中 `QUdpSocket::writeDatagram` 的实现路径和直接 `sendto` 的调用路径。用 Wireshark 抓包对比两种方式的发送间隔抖动。

### 3. 为什么组帧器用 std::map 而不是 vector

**为什么是这个方案**：
UDP 包到达顺序不保证。用 `map<packet_index, payload>` 存储，map 按 key 自动升序排列，收齐后遍历即可得到正确顺序。

**底层是什么**：
`std::map` 底层是红黑树，插入 O(log n)，遍历是中序遍历（天然有序）。相比 vector 需要排序后再拼接，map 在插入时即维护顺序，代码更简洁。对于一帧最多几十个分片的场景，红黑树的常数开销可以忽略。

**如果是你你会怎么学**：
看 cppreference 的 `std::map` 词条，理解红黑树的有序性保证。对比 `std::unordered_map`（哈希表，无序）+ 手动排序的方案，权衡时间复杂度。

### 4. 为什么串流模式跳过音视频同步

**为什么是这个方案**：
串流的"正确行为"是尽快显示最新帧，而不是按 PTS 排队等待。文件播放需要同步是因为音视频两条管线并行，不同步会出现"嘴型对不上"。串流没有音频，且延迟是第一优先级。

**底层是什么**：
文件模式的同步算法是"音频主时钟"——以音频播放进度为基准，视频帧提前则等待、落后则丢帧。串流模式直接 `Pop() → Render() → av_frame_free()`，帧在队列里不停留。

**如果是你你会怎么学**：
看 ffplay 源码的 `video_refresh` 函数，理解 `frame_timer` 和 `remaining_time` 的计算逻辑。对比 Moonlight-Qt 的 Pacer 模块（`Pacer.cpp`），它只做 1~2ms 的极短平滑，不做长缓冲同步。

---

## 踩过的坑

### 坑 1：std::min 被 Windows 宏污染

**现象**：NalUnit.h 编译报 C2589 `"("` illegal token on right side of "::"`

**原因**：Windows.h 定义了 `min`/`max` 宏，与 `std::min` 冲突。

**修复**：给 `std::min` 加括号避免宏展开：`(std::min)(MAX_PAYLOAD_SIZE, frame_size - offset)`

### 坑 2：组帧器不处理乱序包

**现象**：测试 4（乱序包）失败，重组数据错误。

**原因**：初版用 vector 按到达顺序存储，拼接时数据顺序错乱。

**修复**：改用 `std::map<uint16_t, std::vector<uint8_t>>`，利用 map 的自动排序特性保证拼接顺序正确。

### 坑 3：SOF 包重置已有帧数据

**现象**：同一帧的 SOF 包重复到达时，会清空已收到的分片数据。

**原因**：初版逻辑看到 SOF 就清空缓冲，没有判断是否是同一帧。

**修复**：修改帧切换条件——只有 `frame_index` 不同或上一帧已完成（got_eof_）时才切换新帧，同帧的 SOF 不重置。

### 坑 4：解码器线程退出时挂起

**现象**：`FFmpegPlayer::Stop()` 调用后程序卡住。

**原因**：`VideoDecoder::Stop()` 只停了 `packet_queue_`（设 stopped_ 标志），但 `frame_queue_` 没停，DecodeLoop 的 `Pop()` 仍在阻塞等待帧。

**修复**：确保 Stop 顺序正确——先停 VideoReceiver（不再生产 packet），再停 VideoDecoder（packet_queue 的 Pop 会返回 nullptr 退出循环），VideoDecoder 退出前会 `frame_queue_.Stop()` 让 DecodeLoop 的 Pop 也退出。

---

## 联调验证

### 步骤

```
1. 编译 Release 版本

2. 开两个终端：
   终端 A（服务器）：
   Player.exe --server --port 47998 --monitor 1 --fps 60

   终端 B（客户端）：
   Player.exe --stream --port 47998 --width 1920 --height 1080 --fps 60

3. 服务器端日志应显示：
   [StreamServer] 首帧: 1920x1080 GPU
   [StreamServer] 初始化完成 -> 127.0.0.1:47998  1920x1080@60fps  10000000bps
   [StreamServer] 开始推流...

4. 客户端窗口应显示服务器端采集的桌面画面

5. 注意：--width --height 要和服务器端采集到的实际分辨率一致
   （看服务器启动日志的"首帧: WxH"）
```

### 评价标准

| 指标 | 目标值 | 测试方法 |
|------|--------|----------|
| 端到端延迟 | < 30ms (1080p@60fps) | 服务端显示毫秒时钟，手机拍照对比两端 |
| 帧率 | = 服务端帧率 (60fps) | 客户端统计每秒解码帧数 |
| 丢包率 | < 1% (无限制局域网) | 统计 frame_index 断点 |
| CPU 占用 | < 15% | 任务管理器 |

---

## 文件清单

| 文件 | 行数 | 作用 |
|------|------|------|
| `Common/NetWork/NalUnit.h` | ~370 | NAL 分片/组帧协议 + 自测 |
| `Common/NetWork/VideoSender.h` | ~45 | UDP 发送器声明 |
| `Common/NetWork/VideoSender.cpp` | ~85 | UDP 发送器实现 |
| `Common/NetWork/VideoReceiver.h` | ~55 | UDP 接收器声明 |
| `Common/NetWork/VideoReceiver.cpp` | ~175 | UDP 接收器实现 |
| `Server/StreamServer.h` | ~55 | 服务器模式声明 |
| `Server/StreamServer.cpp` | ~180 | 服务器模式实现 |
| `Client/StreamUI/StreamWindow.h` | ~55 | 串流主窗口声明 |
| `Client/StreamUI/StreamWindow.cpp` | ~150 | 串流主窗口实现 |
| `Client/StreamUI/StreamVideoWidget.h` | ~30 | 串流视频组件声明 |
| `Client/StreamUI/StreamVideoWidget.cpp` | ~70 | 串流视频组件实现 |
| `Client/Core/FFmpegPlayer.h` | 修改 | 新增 OpenStream + 串流成员 |
| `Client/Core/FFmpegPlayer.cpp` | 修改 | 新增串流初始化 + 低延迟解码路径 |
| `App/main.cpp` | 修改 | 三模式分流 + 命令行解析 |
| `App/PlayerApp.h` | 修改 | 双模式初始化结构 |
| `App/PlayerApp.cpp` | 修改 | 文件模式 / 串流模式拆分 |
