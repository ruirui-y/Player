## 整体架构：两条流水线 + 一个裁判

ffplay 内部有三条线程同时跑：

```
┌──────────────┐
│  read_thread │  读取文件，分发 AVPacket
└──────┬───────┘
       │
       ├────────→ audioq (PacketQueue) ─→ audio_thread ─→ sampq (FrameQueue) ─→ SDL音频回调
       │
       └────────→ videoq (PacketQueue) ─→ video_thread ─→ pictq (FrameQueue) ─→ video_refresh → SDL渲染
                                                                    ↑
                                                          音频时钟作为主时钟，决定视频什么时候显示
```

**音频是主时钟，视频是跟班**——视频什么时候显示一帧，要看音频播到哪里了。

---

## 第一步：打开文件和分发包

`stream_open()` 做一些初始化，然后启动 `read_thread`。这个线程做的事很简单：

1. 调用 `avformat_open_input()` 打开文件，`avformat_find_stream_info()` 获取流信息
2. 找到音频流和视频流分别调用 `stream_component_open()`
3. 然后在循环里不断调用 `av_read_frame()` 读到一个 `AVPacket`
4. 判断这个包属于哪个流：
   ```
   包的 stream_index == 音频流 → 丢进 audioq
   包的 stream_index == 视频流 → 丢进 videoq
   ```
5. 队列用**序列号（serial）**管理——seek 之后序列号自增，旧队列中的包自然作废

---

## 第二步：两个队列体系

ffplay 有**两级队列**：

**第一级：PacketQueue（存压缩包）**

```
audioq ← 存 AVPacket（H.264/AAC 等压缩数据）
videoq ← 存 AVPacket
```

每个 PacketQueue 内部是一个 FIFO 环形缓冲区，包入队、解码线程出队。

**第二级：FrameQueue（存解码后的帧）**

```
sampq ← 存 AVFrame（PCM 音频样本数据）  大小：9 帧
pictq ← 存 AVFrame（YUV/RGB 视频帧数据）大小：3 帧
```

FrameQueue 为啥这么小（视频才 3 帧）？因为解码后的帧很占内存，而且 3 帧足够流水线平滑，多了浪费。

---

## 第三步：解码

**音频解码** — `audio_thread`

从 `audioq` 拿 AVPacket，调用 `avcodec_decode_audio4()`（框架里实际是 `decoder_decode_frame`），解码出 AVFrame，放入 `sampq`。

**视频解码** — `video_thread`

从 `videoq` 拿 AVPacket，调用 `avcodec_decode_video2()`，解码出 AVFrame，放入 `pictq`。

两个解码线程是**并行**跑的，互不等待。

这里有第一个关键点——**视频在解码前就可能被丢掉**：

```
get_video_frame() 中：
  如果 视频帧的 PTS 已经比主时钟（音频时钟）小（说明这帧已经晚了）
  并且丢帧功能开启
  → 直接 av_frame_unref() 丢掉，不进 pictq
这叫 "early drop"（提前丢帧）
```

---

## 第四步：音频渲染（SDL 回调驱动）

SDL 打开音频设备后，会定期回调 `sdl_audio_callback()`。这个回调是**中断级别**的，必须尽快返回。

流程：

```
sdl_audio_callback(stream, len)
  └→ audio_decode_frame()
       ├─ 从 sampq 取一帧音频 AVFrame
       ├─ synchronize_audio()     ← 同步点：计算应该输出多少样本
       │    如果音频和主时钟（也就是音频自己）有偏差
       │    → 通过重采样器增减样本数（±10% 以内）
       ├─ swr_convert() 重采样（把原始格式转成 SDL 需要的 S16 格式）
       └─ 更新 audio_clock = pts + 样本数/采样率
  └→ 把 is->audio_buf 中的音频数据 memcpy 到 SDL 的 stream 中
  └→ set_clock_at(&is->audclk, ...)  ← 更新音频时钟
```

**音频时钟的精确计算方法**：

```
audio_clock = 当前帧的 pts + (已经输出的样本数 / 采样率)
audclk = audio_clock - (还在硬件缓冲里没播完的数据时长)
```

减掉"还没播完的数据时长"是为了得到"此时此刻喇叭里正在响的那个声音的 PTS"。

---

## 第五步：视频渲染（主循环驱动）

主循环 `refresh_loop_wait_event()` 大约每 10ms 跑一次，调用 `video_refresh()`。

```
video_refresh()
  ├─ 从 pictq 取出当前要显示的视频帧 vp 和上一帧 lastvp
  │
  ├─ 计算 last_duration = vp 和 nextvp 的 PTS 差值（帧的天然时长）
  │
  ├─ compute_target_delay(last_duration)   ← 核心同步点
  │     diff = vidclk - master_clock（视频时钟 - 音频时钟）
  │     diff > 0 → 视频快了 → 增大 delay（等一等）
  │     diff < 0 → 视频慢了 → 减小 delay（追一追）
  │     如果 diff 在阈值内（< 0.04s）→ 不做调整
  │
  ├─ 检查：time >= frame_timer + delay ?
  │    NO  → 还没到显示时间，跳过继续等
  │    YES → 该显示了
  │
  ├─ frame_timer += delay（记录"应该显示的时间"）
  │
  ├─ update_video_pts() → set_clock(&vidclk, vp->pts)
  │
  ├─ 第二次判断：pictq 里还有帧，time > frame_timer + duration?
  │    YES → 这一帧太晚了，丢掉（late drop），看下一帧
  │
  └─ video_display() → SDL_RenderCopy → SDL_RenderPresent
```

---

## 第六步：同步的关键直觉

把同步问题简化成一句话：

> **视频比音频快了？那就多等一会儿再显示下一帧。视频比音频慢了？那就少等一会儿，赶紧追上。实在差太多？直接把当前帧丢掉，跟下一帧同步。**

具体的数值关系：

```
音频时钟 = 当前正在播放的音频的 PTS（精确到亚毫秒级）
视频时钟 = 当前准备显示的帧的 PTS

diff = 视频时钟 - 音频时钟

diff ≈ 0          → 完美同步
diff < -0.04s     → 视频落后了，加速显示（减少 delay）
diff > +0.04s     → 视频超前了，减速显示（增加 delay）
diff > +0.1s 且帧很短 → delay 翻倍
diff 巨大（> 10s）→ 认为失同步，什么都不做，等系统自己恢复
```

音频那边也是类似逻辑——只不过它的调整方式不是"等"或"跳"，而是**通过重采样器微调样本数**（比如本来该输出 1024 个样本，改成 1000 或 1048 个），人耳几乎感觉不到。

---

## 一条完整的时间线（以音频主时钟为例）

```
t=0.000   read_thread 打开文件，开始读包
          audioq 收到第 1 个音频包，videoq 收到第 1 个视频包

t=0.010   audio_thread 开始解码第 1 个音频包 → sampq
          video_thread 开始解码第 1 个视频包 → pictq

t=0.020   SDL 音频回调启动，从 sampq 取帧，同步检查，重采样，送到声卡
          set_clock_at(&audclk, 0.020)  ← 音频时钟设为 0.020s

t=0.030   video_refresh 运行，从 pictq 取帧
          diff = vidclk(0.000) - audclk(0.020) = -0.02 → 在阈值内，不管
          time(0.030) < frame_timer(0.000) + delay(≈0.040)? → 是，继续等

t=0.040   video_refresh 再次运行
          time(0.040) >= frame_timer(0.000) + delay(≈0.040)? → 是，显示
          frame_timer += delay → frame_timer = 0.040
          set_clock(&vidclk, 0.033)  ← 视频时钟更新

t=0.050   video_refresh 运行，处理下一帧
          diff = 0.066 - 0.055 = +0.011 → 视频稍快，增加一点 delay
          继续...

整个过程中，音频时钟匀速前进，视频反复计算 diff 来微调自己的节奏。
```

---

## 总结

| 环节       | 你关心的问题   | 答案                                                         |
| ---------- | -------------- | ------------------------------------------------------------ |
| 文件打开   | 包怎么分的     | `read_thread` 读包，按 stream_index 分到 audioq/videoq       |
| 队列体系   | 有哪些队列     | PacketQueue（压缩包）→ FrameQueue（解码帧）：audioq→sampq, videoq→pictq |
| 解码       | 怎么解码的     | 两个独立线程各自从 PacketQueue 取包，`avcodec_decode_*` 到 AVFrame，放进 FrameQueue |
| 音视频同步 | 渲染时怎么同步 | 视频侧：计算 `vidclk - audclk`，微调显示 delay，严重则丢帧；音频侧：增减采样数 |
| 渲染       | 怎么渲染的     | 音频：SDL 回调驱动；视频：主循环每 10ms 检查一次，到时间就 `SDL_RenderCopy` |



---

## 先定好参数

```
音频：48000Hz, 16bit, 双声道
  bytes_per_sec = 48000 × 2 × 2 = 192000
  每帧 1024 个样本 → 时长 = 1024/48000 = 21.33ms ≈ 0.021s
  每帧数据量 = 1024 × 2 × 2 = 4096 字节

音频硬件缓冲区：audio_hw_buf_size = 8192 字节（SDL 每次回调要这么多）
  时长 = 8192/192000 = 42.67ms ≈ 0.043s

视频：25fps，每帧间隔 40ms
  帧 PTS = 0.000, 0.040, 0.080, 0.120, ...
```

---

## 第一步：音频回调发生，算 audclk

**第一次回调 — 在系统时间 t=0.018s 触发**

SDL 说："给我 8192 字节的数据，我要送声卡。"

```
audio_decode_frame() 取帧 1：PTS = 0.000，1024 个样本
  audio_clock = 0.000 + 0.021 = 0.021   ← 这帧播完时的文件进度
  把 4096 字节 PCM 数据拷贝给 SDL

但 SDL 要 8192 字节，才给了一半 → 再来一帧
audio_decode_frame() 取帧 2：PTS = 0.021，1024 个样本
  audio_clock = 0.021 + 0.021 = 0.042   ← 帧 2 播完时的文件进度
  再拷 4096 字节

SDL 拿满 8192 字节，回调结束
```

回调结束，更新 audclk：

```
audio_clock = 0.042（帧 2 播完时的进度）
已经在 SDL 内部排队但还没播的数据量 = 2 × 8192 = 16384 字节
                                 （SDL 双缓冲，两个 buffer 都满了）
这些数据能播多久 = 16384 / 192000 = 0.085s

所以现在喇叭里正在响的，是比 0.042 早 0.085 秒的声音：
  audclk.pts = 0.042 - 0.085 = -0.043
  audclk.last_updated = 0.018（回调发生的系统时间）
  audclk.pts_drift = -0.043 - 0.018 = -0.061
```

---

## 第二步：get_clock 怎么持续往前走

现在其他线程在任何时间读 audclk，都用这个公式：

```
get_clock(&audclk) = pts_drift + 当前系统时间

在 t=0.020 读取：-0.061 + 0.020 = -0.041
在 t=0.030 读取：-0.061 + 0.030 = -0.031
在 t=0.040 读取：-0.061 + 0.040 = -0.021
在 t=0.050 读取：-0.061 + 0.050 = -0.011
在 t=0.061 读取：-0.061 + 0.061 = 0.000
```

**audclk 的值在两次回调之间不是死的，它会随着系统时间自动往前走。视频线程读到的 audclk 一直在递增。**

---

## 第三步：第二次回调，更新 audclk

**第二次回调 — 在系统时间 t≈0.061s 触发**

（0.018 + 0.043 = 0.061，正好一个缓冲区播完）

```
audio_decode_frame() 取帧 3：PTS = 0.043 → audio_clock = 0.043+0.021=0.064
audio_decode_frame() 取帧 4：PTS = 0.064 → audio_clock = 0.064+0.021=0.085
又填满 8192 字节给 SDL

更新 audclk：
  audclk.pts = 0.085 - 0.085 = 0.000
  audclk.last_updated = 0.061
  audclk.pts_drift = 0.000 - 0.061 = -0.061

get_clock(&audclk)：
  t=0.061 = -0.061 + 0.061 = 0.000
  t=0.070 = -0.061 + 0.070 = 0.009
  t=0.080 = -0.061 + 0.080 = 0.019
  t=0.090 = -0.061 + 0.090 = 0.029
```

---

## 第四步：第三次回调

**在 t≈0.104s（0.061+0.043）触发**

```
帧 5：PTS=0.085 → audio_clock = 0.085+0.021=0.106
帧 6：PTS=0.106 → audio_clock = 0.106+0.021=0.127

audclk.pts = 0.127 - 0.085 = 0.042
audclk.pts_drift = 0.042 - 0.104 = -0.062

get_clock(&audclk)：
  t=0.104 = -0.062 + 0.104 = 0.042
  t=0.120 = -0.062 + 0.120 = 0.058
  t=0.140 = -0.062 + 0.140 = 0.078
  t=0.160 = -0.062 + 0.160 = 0.098
```

---

## 第五步：video_refresh 怎么用 audclk

视频线程不知道"音频播到哪了"，它只能来读 audclk。

**t=0.040 — 第一次显示视频帧 1（PTS=0.000）**

```
video_refresh 执行：

step 1 — 算 delay
  get_clock(&vidclk) = NAN（还没显示过任何帧，没设过）
  get_master_clock() = get_clock(&audclk) = -0.021
  diff = NAN - (-0.021) = NAN
  跳过同步调整，delay = 0.040（直接按帧率来）

step 2 — 到时间了吗？
  frame_timer = 0（初始值）
  frame_timer + delay = 0 + 0.040 = 0.040
  现在时间 0.040 >= 0.040 → 到了！显示！

step 3 — 显示，更新 vidclk
  set_clock(&vidclk, 0.000, t=0.040):
    vidclk.pts = 0.000
    vidclk.last_updated = 0.040
    vidclk.pts_drift = 0.000 - 0.040 = -0.040

  frame_timer = 0 + 0.040 = 0.040
```

**t=0.080 — 显示帧 2（PTS=0.040）**

```
step 1 — 算 delay
  get_clock(&vidclk) = -0.040 + 0.080 = 0.040
  get_clock(&audclk) = -0.061 + 0.080 = 0.019
  diff = 0.040 - 0.019 = +0.021
  同步阈值 = max(0.04, min(0.1, 0.04)) = 0.04
  0.021 < 0.04 → 不做调整
  delay = 0.040

step 2 — 到时间了吗？
  frame_timer(0.040) + delay(0.040) = 0.080
  现在时间 0.080 >= 0.080 → 到了！显示！

step 3 — 更新
  vidclk.pts = 0.040, last_updated=0.080, pts_drift=-0.040
  frame_timer = 0.080
```

**t=0.120 — 显示帧 3（PTS=0.080）**

```
  diff = get_clock(&vidclk) - get_clock(&audclk)
       = (-0.040+0.120) - (-0.062+0.120)
       = 0.080 - 0.058
       = +0.022
  0.022 < 0.04 → 不做调整
```

---

## 第六步：出现同步校正的场景

假设帧 4（PTS=0.120）解码慢了 100ms，到 t=0.220 才进入 pictq。

**t=0.220 — video_refresh 发现队列里有帧了**

```
diff = get_clock(&vidclk) - get_clock(&audclk)
     = (-0.040+0.220) - (-0.062+0.220)     ← pts_drift 没变
     = 0.180 - 0.158
     = +0.022
```

等等，这样看 diff 还是正的，没触发校正——**但 vidclk 只是"估算"，不是真正的内容进度！**

真正的关键在这里：**vidclk 是根据上次显示的帧（帧 3 的 PTS=0.080）+ 流逝时间算出来的，但实际要显示的帧是帧 4（PTS=0.120），它的内容比 vidclk 估算的晚了**。

真正的同步靠的是 **frame_timer**。看：

```
frame_timer = 0.120（上次设的，对应帧 3 显示后 + delay）
            = 应该是帧 4 的预期显示时间

但现在 t=0.220，而 frame_timer + delay = 0.120 + 0.040 = 0.160
t(0.220) >= 0.160 → 晚了 60ms

还要检查晚期丢帧：
  pictq 里还有帧 5（PTS=0.160）
  duration = vp_duration(帧4, 帧5) = 0.160 - 0.120 = 0.040
  t(0.220) > frame_timer(0.160) + duration(0.040) = 0.200?
  0.220 > 0.200 → YES → 帧 4 太晚了，丢掉！直接显示帧 5
```

**如果丢帧没开启呢？那 compute_target_delay 会调整：**

```
延迟了，但没丢帧：
  diff = 0.180 - 0.158 = +0.022 → 还不到阈值
  注意这里 diff 是正的（视频估算超前），但实际内容落后了
```

这个场景有点特殊，让我换一个**确实会触发校正**的例子：

**场景：视频解码器突然变慢，帧 4 解码完放入 pictq 时是 t=0.380**

此时 audclk 已经走到：
```
audclk 在第四次回调（t≈0.147）更新：
  pts_drift ≈ -0.062（稳定了）

t=0.380: get_clock(&audclk) = -0.062 + 0.380 = 0.318
```

video_refresh 在 t=0.380：
```
sampq 终于有帧了（帧 4, PTS=0.120）
lastvp = 帧 3 (PTS=0.080)
vp = 帧 4 (PTS=0.120)

last_duration = 0.120 - 0.080 = 0.040

compute_target_delay(0.040):
  get_clock(&vidclk) = -0.040 + 0.380 = 0.340
  get_clock(&audclk) = 0.318
  diff = 0.340 - 0.318 = +0.022
  ← 还是没到阈值？？？
```

我意识到问题出在 **vidclk 的 pts_drift 是在帧 3 显示时设的，从那之后一直在线性增长**。如果视频停了很久，vidclk 已经往前跑了很多，和 audclk 的差值可能反而变小了。

**真正导致校正的信号是：帧该显示的时候没显示。**

看看 `video_refresh` 的代码流程——`frame_timer` 才是那个真正暴露出问题的变量：

```
t=0.220 时 video_refresh：
  frame_timer + delay = 0.120 + 0.040 = 0.160
  time(0.220) < 0.160? → NO → 显示帧 4（虽然晚了）
  frame_timer = 0.120 + 0.040 = 0.160
  time - frame_timer = 0.220 - 0.160 = 0.060

  检查晚期丢帧：
  duration = nextvp->pts - vp->pts
  if time > frame_timer + duration → 丢帧
  
  但如果没丢帧，继续显示：

显示后更新：vidclk.pts = 0.120, last_updated=0.220, pts_drift=0.120-0.220=-0.100

t=0.260 video_refresh（帧 5, PTS=0.160）：
  diff = vidclk - audclk
  get_clock(&vidclk) = -0.100 + 0.260 = 0.160
  get_clock(&audclk) = -0.062 + 0.260 = 0.198
  diff = 0.160 - 0.198 = -0.038
  
  阈值 = 0.04
  -0.038 > -0.04 → |diff| < 0.04 → 不做调整

t=0.300（帧 6, PTS=0.200）：
  vidclk pts_drift = -0.100（还是上次）
  get_clock(&vidclk) = -0.100 + 0.300 = 0.200
  get_clock(&audclk) = -0.062 + 0.300 = 0.238
  diff = 0.200 - 0.238 = -0.038
  
  还是没到阈值
```

你看，因为 audclk 和 vidclk 都在同步往前走（pts_drift 都是负的常数），加上流逝的系统时间，两者之差一直稳定在 ±0.02~0.04 之间，很少会主动突破阈值。

**所以 compute_target_delay 的校正更多是一个安全网——大部分情况下，frame_timer 机制自己就能维持同步。** 真正的校正发生在大跳跃的场景（seek、长时间卡顿、网络流抖动）。

---

## 总结：audclk 到底是怎么驱动视频同步的

```
              ┌────────────────────┐
              │   声卡硬件         │
              │   每 43ms 播完     │
              │   一个缓冲区       │
              └────────┬───────────┘
                       │ "我要数据"
                       ▼
              ┌────────────────────┐
              │   readData 回调    │ ← 音频解码 + 填数据
              │   更新 audclk      │
              └────────┬───────────┘
                       │ audclk = 0.123
                       │（声卡正在播文件第 0.123 秒的内容）
                       ▼
              ┌────────────────────┐
              │   video_refresh    │ ← 每 10ms 检查一次
              │   diff = vidclk    │
              │        - audclk    │
              │                    │
              │   diff < -0.04?    │ → 视频晚了，加速显示
              │   diff > +0.04?    │ → 视频快了，减速等待
              │   其他情况         │ → 按帧率正常显示
              └────────────────────┘
```

audio_clock = "我刚刚解码的这包数据播完时的文件进度"

audclk = "刚才那个值，减去还在 SDL 里排队没播的数据量 = 喇叭现在正在响的声音进度"

video_refresh 每次要显示一帧前，先读 audclk 看喇叭到哪了，再决定自己要不要等一下或者快一点。