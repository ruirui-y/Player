# FFplayPlayer 音视频同步改造完整方案

基于 ffplay 音频主时钟策略，实现一个完整的音视频同步播放器。

---

## 一、整体架构

### 1.1 五线程流水线

```
┌───────────┐
│  Reader   │  读包线程（生产者）
└───┬───┬───┘
    │   │
    │   │  按 stream_index 分拣
    │   │
    │   └──── audio_packet_queue_ ──→ ┌──────────────┐
    │            （无上限，压缩包）      │ AudioDecoder │  音频解码线程
    │                                 └──────┬───────┘
    │                                        │
    │                                        │ DecodePacket() → PushMax(9)
    │                                        │
    │                                        └──→ audio_frame_queue_ (上限9帧)
    │
    └────────── video_packet_queue_ ──→ ┌──────────────┐
                 （无上限，压缩包）        │ VideoDecoder │  视频解码线程（支持D3D11VA硬解）
                                        └──────┬───────┘
                                               │
                                               │ DecodePacket() → PushMax(3)
                                               │
                                               └──→ video_frame_queue_ (上限3帧)
                                                        │
                                                        │  Pop() 阻塞取帧
                                                        ▼
                                                  ┌───────────┐
                                                  │ DecodeLoop│  渲染线程（消费者）
                                                  └───┬───┬───┘
                                                      │   │
                                                      │   │  FeedFrame（非阻塞，bytesFree保护）
                                                      │   │
                                                      │   └→ AudioRenderer::FeedFrame → QAudioOutput → 声卡
                                                      │
                                                      │   Render（frame_timer同步后）
                                                      │
                                                      └→ VideoRenderer::Render → 屏幕
```

### 1.2 四队列体系

| 队列名                | 类型     | 上限   | 生产者       | 消费者       |
| --------------------- | -------- | ------ | ------------ | ------------ |
| `video_packet_queue_` | AVPacket | 无上限 | Reader       | VideoDecoder |
| `audio_packet_queue_` | AVPacket | 无上限 | Reader       | AudioDecoder |
| `video_frame_queue_`  | AVFrame  | 3 帧   | VideoDecoder | DecodeLoop   |
| `audio_frame_queue_`  | AVFrame  | 9 帧   | AudioDecoder | DecodeLoop   |

**设计理由：**
- 压缩包队列无上限：压缩数据体积很小（一个包通常几 KB），不会撑爆内存
- 视频帧队列上限 3：参考 ffplay 的 `VIDEO_PICTURE_QUEUE_SIZE`，1080p 一帧约 6MB，防止内存溢出
- 音频帧队列上限 9：参考 ffplay 的 `SAMPLE_QUEUE_SIZE`，约 200ms 音频数据

### 1.3 背压机制（Back Pressure）

帧队列满时，解码线程阻塞在 `PushMax` 中等待。渲染线程 `Pop` 消费一帧后通过 `cv_.notify_one()` 唤醒解码线程。

```
解码线程 PushMax(frame, 3):
  队列有 3 帧 → 阻塞等待
  渲染线程 Pop() → 腾出空位 → 唤醒 → 解码线程放入新帧
```

### 1.4 线程启停顺序

**启动**（先消费者后生产者）：

```
audio_renderer_->Start()  →  渲染线程  →  reader_.Start()  →  video_decoder_.Start()  →  audio_decoder_.Start()
```

**停止**（先生产者后消费者）：

```
reader_.Stop()  →  video_decoder_.Stop()  →  audio_decoder_.Stop()  →  decode_thread_.join()  →  audio_renderer_->Stop()
```

---

## 二、音视频同步

### 2.1 设计思想：音频主时钟

以声卡的播放进度（audclk）作为参考时钟。视频每显示一帧前，先计算该帧 PTS 与 audclk 的差值 diff，根据 diff 决定等待、加速或丢帧。

```
diff = video_pts - audio_clock（秒）

  diff ≈ 0（±0.04s 内）   → 不做调整，按帧率正常显示
  diff < -0.04（视频慢了） → 缩短等待时间加速追赶
  diff > +0.04（视频快了） → 延长等待时间减速等待
  diff < -0.5（严重落后）  → 直接丢帧
```

### 2.2 核心变量

| 变量                   | 类型   | 含义                                     | 参考 ffplay         |
| ---------------------- | ------ | ---------------------------------------- | ------------------- |
| `frame_timer_ms_`      | double | 上一帧"应该"显示的时间点（毫秒）         | `frame_timer`       |
| `video_clock_ms_`      | double | 当前视频帧 PTS（毫秒）                   | `vidclk.pts`        |
| `last_frame_pts_ms_`   | double | 上一帧 PTS（毫秒），用于相邻帧差值计算   | —                   |
| `pause_start_time_ms_` | double | 暂停时的系统时间，恢复时补偿 frame_timer | —                   |
| `frame_drops_early_`   | int    | 早期丢帧计数                             | `frame_drops_early` |
| `frame_drops_late_`    | int    | 晚期丢帧计数                             | `frame_drops_late`  |

### 2.3 时钟系统

**音频时钟（audclk）：** `AudioRenderer::GetClock()` 返回 `QAudioOutput::processedUSecs() / 1000`，单位毫秒。

Qt 底层维护的值——基于声卡实际消费的数据量实时更新，精度约 1~5ms。

**视频时钟（vidclk）：** `video_clock_ms_`，每次渲染一帧后更新为该帧的 PTS。两次更新之间不增长（和 ffplay 的 `get_clock` 线性推算不同，简化实现）。

### 2.4 DecodeLoop 完整同步流程

```
1. 等待音频帧 ≥2 且视频帧 ≥1

2. 初始化 frame_timer = 当前系统时间

3. 主循环（每帧一次）：
   a. 喂音频（非阻塞，声卡有空位才取帧）
   b. 从 video_frame_queue_ 取一帧（阻塞）
   c. 计算 pts_sec = frame->pts × time_base
   d. diff = pts_sec - GetClock()/1000
   e. 早期丢帧：diff < -0.5 且有后续帧 → 丢帧
   f. 计算基准 delay：
      - 优先用相邻帧 PTS 差值（更精确）
      - 算不出来则用帧率倒数保底
   g. 同步校正：delay = ComputeTargetDelay(delay, diff)
   h. 等待至 frame_timer + delay
   i. 更新 frame_timer += delay
   j. 容错：实际时间落后 frame_timer > 100ms 时拉回
   k. 晚期丢帧：diff 落后 > 阈值且有后续帧 → 丢帧
   l. 更新 video_clock、last_frame_pts
   m. Render 当前帧

4. 清理退出
```

### 2.5 ComputeTargetDelay 算法

```cpp
sync_threshold = clamp(delay, 0.04, 0.1)

条件 A：diff <= -sync_threshold（视频落后）
  → delay = max(0, delay + diff)        // 缩短 delay，加速追赶

条件 B：diff >= +sync_threshold（视频超前）
  且 delay > 0.1s（帧时长大）→ delay = delay + diff    // 温和等待
  且 delay ≤ 0.1s（帧时长小）→ delay = delay × 2       // 激进减速

条件 C：|diff| < sync_threshold
  → delay 不变                                  // 静默区
```

### 2.6 丢帧策略

| 层级     | 触发条件                                        | 结果                           |
| -------- | ----------------------------------------------- | ------------------------------ |
| 早期丢帧 | `diff < -0.5s` 且 `video_frame_queue_` 有后续帧 | 丢掉当前帧，看下一帧           |
| 晚期丢帧 | `diff < -delay×2` 且队列有后续帧                | 同上（条件更宽松，已等过一轮） |

### 2.7 暂停补偿

暂停时记录 `pause_start_time_ms_`。恢复时计算暂停时长，加到 `frame_timer_ms_` 上：

```
恢复时：
  pause_duration = now - pause_start_time_ms_
  frame_timer_ms_ += pause_duration
```

防止暂停期间 frame_timer 不动，恢复后视频比音频超前。

---

## 三、音频渲染

### 3.1 Push 模式

使用 `QAudioOutput` 的 push 模式（非 callback）。DecodeLoop 主动调用 `audio_io_->write(pcm_data)` 写入 PCM 数据。

### 3.2 bytesFree 保护

声卡内部缓冲区满时不能写入，否则会导致数据被截断、声音刺耳或加速。

```cpp
// 只在声卡能接受数据时才取帧
while (audio_renderer_->CanAcceptFrame() &&
    audio_frame_queue_.TryPop(af) && n < 5)
{
    audio_renderer_->FeedFrame(af);
    av_frame_free(&af);
    n++;
}
```

### 3.3 帧大小缓存

一帧 PCM 的大小由音频编码格式决定（AAC=4096 字节，MP3=4608 字节等），文件内固定不变。只在第一帧计算一次并缓存。

```cpp
// FeedFrame 中首次计算并缓存
if (frame_bytes_ == 0)
{
    int dst_nb_samples = av_rescale_rnd(
        swr_get_delay(swr_ctx_, frame->sample_rate) + frame->nb_samples,
        sample_rate_, frame->sample_rate, AV_ROUND_UP);
    frame_bytes_ = av_samples_get_buffer_size(
        nullptr, 2, dst_nb_samples, AV_SAMPLE_FMT_S16, 1);
}

// CanAcceptFrame 中用于判断声卡是否有足够空间
bool AudioRenderer::CanAcceptFrame() const
{
    if (!audio_sink_) return false;
    return audio_sink_->bytesFree() >= frame_bytes_;
}
```

DecodeLoop 先调 `CanAcceptFrame()` 再取帧，避免"取出来了但声卡写不进去"的问题。

### 3.4 音频时钟

`GetClock()` 返回 `processedUSecs() / 1000`。该值来自 Qt 底层 WASAPI/PulseAudio 统计的"声卡已消费数据量"，误差约 1~5ms，远低于同步阈值 ±40ms，足够使用。

---

## 四、文件结构

```
Player/Core/
  │
  ├── Reader.h/cpp           读包线程，按 stream_index 分拣到两个包队列
  ├── VideoDecoder.h/cpp     视频解码线程（支持 D3D11VA 硬解）
  ├── AudioDecoder.h/cpp     音频解码线程
  ├── SafeQueue.h            线程安全队列（PushMax 背压支持）
  ├── ClockUtil.h            同步阈值常量 + ComputeTargetDelay
  ├── FFmpegPlayer.h/cpp     渲染线程 + 音视频同步
  │
  ├── AudioRenderer.h/cpp    音频渲染（解码→重采样→QAudioOutput）
  ├── VideoRenderer.h/cpp    视频渲染总入口（GPU 优先，CPU 回退）
  ├── D3D11Pipeline.h/cpp    GPU 渲染管线（交换链 + HLSL 着色器）
  ├── Nv12GpuUploader.h/cpp  GPU 帧上传（硬解帧→NV12 纹理）
  └── SoftwareRenderer.h/cpp CPU 软解回退（sws_scale → QImage）
```

### 类职责

| 类                       | 线程 | 职责                                       |
| ------------------------ | ---- | ------------------------------------------ |
| Reader                   | 1    | 打开文件，读 AVPacket，分拣到两个包队列    |
| VideoDecoder             | 1    | 从视频包队列取包→解码→PushMax 到视频帧队列 |
| AudioDecoder             | 1    | 从音频包队列取包→解码→PushMax 到音频帧队列 |
| FFmpegPlayer::DecodeLoop | 1    | 音频喂声卡 + 视频帧同步 + 渲染             |
| 主线程（Qt）             | 1    | UI 事件循环，QAudioOutput 初始化           |

---

## 五、ffplay 对照表

| ffplay 组件              | 本项目对应                                    | 差异                                                         |
| ------------------------ | --------------------------------------------- | ------------------------------------------------------------ |
| `read_thread`            | `Reader`                                      | 基本一致                                                     |
| `audioq` / `videoq`      | `audio_packet_queue_` / `video_packet_queue_` | 无上限                                                       |
| `sampq` / `pictq`        | `audio_frame_queue_` / `video_frame_queue_`   | 上限 9/3                                                     |
| `audio_thread`           | `AudioDecoder`                                | 独立线程                                                     |
| `video_thread`           | `VideoDecoder`                                | 独立线程                                                     |
| `get_clock(&audclk)`     | `AudioRenderer::GetClock() / 1000`            | ffplay 用 set_clock_at 手动计算，本项目用 Qt 的 processedUSecs |
| `get_clock(&vidclk)`     | `video_clock_ms_ / 1000`                      | 简化，不线性外推                                             |
| `compute_target_delay()` | `ClockUtil::ComputeTargetDelay()`             | 一致                                                         |
| `frame_timer`            | `frame_timer_ms_ / 1000`                      | 一致                                                         |
| `frame_drops_early/late` | `frame_drops_early_` / `frame_drops_late_`    | 阈值从 ffplay 的"diff < 0"放宽到 -0.5s                       |
| `sdl_audio_callback()`   | `FeedPcmData` + `bytesFree` 保护              | push 模式 vs callback 模式                                   |
| `serial`                 | 无                                            | 本项目暂不支持 seek                                          |
| `extclk`                 | 无                                            | 降级时用帧率间隔保底                                         |

---

## 六、验证标准

| 测试项                      | 预期结果                               |
| --------------------------- | -------------------------------------- |
| 打开常见格式（MP4/MKV/AVI） | 正常播放，画面流畅                     |
| 音视频同步                  | diff 稳定在 ±30ms 以内                 |
| 暂停/恢复                   | 同步不跑偏                             |
| 长时间播放（10 分钟以上）   | 无累积漂移                             |
| 不同帧率视频（24/30/60fps） | 均正常工作                             |
| 硬解/软解                   | 自动切换                               |
| 丢帧计数                    | 正常情况下极少（前几秒追赶期可能少量） |

---

## 七、已知局限与后续方向

| 功能       | 状态     | 建议方案                                                   |
| ---------- | -------- | ---------------------------------------------------------- |
| seek 跳转  | 未实现   | 需要 serial 序列号 + 解码器 flush + 帧队列清空             |
| 倍速播放   | 未实现   | 通过 swr 变速 + frame_timer 自适应                         |
| 网络流     | 未实现   | 需要缓冲管理 + 丢包处理                                    |
| 纯视频文件 | 未实现   | 降级为帧率间隔同步                                         |
| 字幕       | 未实现   | 新增 SubtitleDecoder + SubtitleRenderer                    |
| 精确时钟   | 可选优化 | 基于 `total_written_ - buffered` 推算，替代 processedUSecs |