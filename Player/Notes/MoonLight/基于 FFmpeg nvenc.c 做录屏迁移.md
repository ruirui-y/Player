# 基于 FFmpeg nvenc.c 录屏迁移完整记录

## 第一阶段：源码分析

### 阅读了 FFmpeg 的三个核心文件

**nvenc.h**
- `NvencContext` 结构体：编码器上下文，包含编码参数、surface 池、设备句柄、队列等
- `NvencSurface` 结构体：每个表面包含 input_surface（输入缓冲区）、output_surface（输出 bitstream 缓冲区）、in_ref、width/height/pitch
- 枚举常量：预设（P1-P7）、码控模式（CBR/LowLatency/Lossless）、Profile
- 公开函数声明：`ff_nvenc_encode_init`、`ff_nvenc_encode_close`、`ff_nvenc_receive_packet`、`ff_nvenc_encode_flush`

**nvenc.c（114KB，分多段读取）**

关键函数及对应行号：
- `nvenc_copy_frame`（第2181行）：将 AVFrame 的像素数据 memcpy 到已经 Lock 的 NVENC input buffer
- `nvenc_upload_frame`（第2325行）：分两条路径——
  - HWACCEL 路径：RegisterResource → MapInputResource（GPU 纹理直传）
  - 软件路径：LockInputBuffer → nvenc_copy_frame（memcpy）→ UnlockInputBuffer
- `nvenc_send_frame`（第3002行）：取空闲 surface → upload → 填充 pic_params → nvEncEncodePicture。返回值处理：SUCCESS 时把所有 pending surface 移到 ready 队列；NEED_MORE_INPUT 时只入 pending 队列
- `process_output_surface`（第2614行）：LockBitstream → memcpy 到 AVPacket → 判断帧类型（IDR/I/P/B）→ 设置 PTS/DTS → UnlockBitstream
- `output_ready`（第2714行）：判断 ready 队列是否有足够帧可输出。正常时要求 `nb_ready + nb_pending >= async_depth`；flush 时仅 `nb_ready > 0`
- `ff_nvenc_receive_packet`（第3175行）：外部接口。从输入取帧 → nvenc_send_frame → output_ready 判断 → process_output_surface 取输出
- `ff_nvenc_encode_flush`（第3226行）：发送 EOS（nvenc_send_frame(NULL)），重置 timestamp 队列
- `ff_nvenc_encode_close`（第2033行）：发送 EOS → 释放所有 surface → 销毁编码器 → 释放 CUDA/D3D 设备 → 卸载 DLL
- `nvenc_alloc_surface`（第1897行）：创建 input buffer（nvEncCreateInputBuffer）或注册 HW 帧 + 创建 bitstream buffer（nvEncCreateBitstreamBuffer），放入 unused 队列
- `nvenc_setup_surfaces`（第1953行）：创建 N 个 surface + 创建 3 个 FIFO 队列（unused/output_surface/output_surface_ready）+ timestamp 队列
- `nvenc_setup_encoder`（第1689行）：获取预设配置 → 覆盖编码参数（码率、GOP、B 帧、VUI 等）→ nvEncInitializeEncoder
- `nvenc_setup_device`（第713行）：选择编码设备（CUDA 或 D3D11），打开编码会话，检查能力
- `ff_nvenc_encode_init`（第2118行）：初始化流程：设置 data_pix_fmt → 分配 frame → nvenc_load_libraries → nvenc_setup_device → nvenc_setup_encoder → nvenc_setup_surfaces → nvenc_setup_extradata

### 阅读了 Capture 目录的四个文件

**ScreenCapture.h/cpp**
- 使用 IDXGIOutputDuplication 捕获桌面
- 优先使用 IDXGIOutput5::DuplicateOutput1 申请 NV12 格式
- 失败时回退到 DuplicateOutput 默认 BGRA 格式
- `CaptureNextFrame` 返回 GPU 显存中的纹理指针（ID3D11Texture2D*）
- 获取方式：AcquireNextFrame → QueryInterface → CopyResource 到内部 gpu_texture_

**NvencEncoder.h/cpp（旧版）**
- 纯软件路径：CopyResource（GPU→staging）→ Map + memcpy → sws_scale（BGRA→NV12）→ LockInputBuffer + memcpy → EncodePicture
- 单 input buffer + 单 output buffer，无队列，纯同步
- 参数：P1 + CBR + frameIntervalP=1 + 1 帧 VBV

---

## 第二阶段：问题定位

### Windows 下 FFmpeg 支持的抓屏方式

**方式一：gdigrab（传统 CPU 路径，已验证可行）**
```
ffmpeg -f gdigrab -offset_x 0 -offset_y 0 -video_size 1920x1080 -i desktop -c:v h264_nvenc -preset p1 -tune ll -b:v 10M -pix_fmt nv12 -r 120 -y capture_gdigrab.h264
```
- **底层 API**：Windows GDI（BitBlt）
- **数据流**：GDI 系统内存 BGRA → sws_scale（BGRA→NV12，CPU）→ av_image_copy（memcpy 到 NVENC input buffer，CPU）→ nvEncEncodePicture（GPU）
- **CPU 开销**：高（每帧 1920x1080 完整 memcpy + 色彩转换）
- **帧率**：受 GDI 渲染性能限制，高分辨率下可能跑不满 120fps

**方式二：ddagrab（GPU 直通路径，对应你的 ScreenCapture）**
```
ffmpeg -f ddagrab -framerate 120 -video_size 1920x1080 -i desktop -c:v h264_nvenc -preset p1 -tune ll -b:v 10M -y capture_ddagrab.h264
```
- **底层 API**：DirectX Desktop Duplication（IDXGIOutputDuplication）—— 和 ScreenCapture 完全一致
- **数据流**：D3D11 纹理（GPU 显存）→ CopyResource → staging → Map CPU → sws_scale（BGRA→NV12，CPU）→ memcpy 到 NVENC input buffer → nvEncEncodePicture（GPU）
- **注意**：这个命令没有加 `-hwaccel d3d11`，所以 FFmpeg 会把 D3D11 纹理回读到 CPU，再走软件路径送进 NVENC。**跟 gdigrab 最终走的是同一条编码路径，只是抓屏方式不同**

**方式三：ddagrab + GPU Direct（零拷贝，性能最优）**
```
ffmpeg -f ddagrab -framerate 120 -video_size 1920x1080 -i desktop -c:v h264_nvenc -preset p1 -tune ll -b:v 10M -hwaccel d3d11 -hwaccel_output_format d3d11 -y capture_ddagrab_gpu.h264
```
- **底层 API**：IDXGIOutputDuplication + D3D11VA hardware acceleration
- **数据流**：D3D11 纹理（GPU 显存）→ nvEncRegisterResource（注册）→ nvEncMapInputResource（映射）→ nvEncEncodePicture（GPU 直接编码，零 CPU 拷贝）
- **CPU 开销**：接近零（全程 GPU 显存内操作，不走 CPU）

### 三种方式对比

| 方式                  | 抓屏底层         | 编码输入路径                        | CPU 拷贝        | 适用场景                    |
| --------------------- | ---------------- | ----------------------------------- | --------------- | --------------------------- |
| gdigrab               | GDI/BitBlt       | 系统内存 → LockInputBuffer          | 每帧完整拷贝    | 通用兼容，低版本 Windows    |
| ddagrab（软件）       | DXGI Duplication | GPU staging → CPU → LockInputBuffer | 每帧回读 + 拷贝 | 有独立显卡，需要高性能抓屏  |
| ddagrab（GPU Direct） | DXGI Duplication | GPU 纹理 → Register → Map → 编码    | 零拷贝          | 高端录屏/串流，追求最低延迟 |

### 对当前项目的意义

你的 `ScreenCapture` 已经实现了 `ddagrab` 的抓屏部分（IDXGIOutputDuplication），输出的是 D3D11 纹理。所以理论上应该走**方式三**的 GPU Direct 路径——这也是之前我在 GPU Direct 版本尝试做的事。问题不在抓屏侧，而在纹理注册/映射的周期管理上。如果只追求先跑通，可以先走**方式二**的软件路径（CopyResource + staging + sws_scale + LockInputBuffer），这是最保险的。

### 我们代码的三个版本迭代

**第一版：GPU Direct + 异步（completion event）**
- 问题：`enableEncodeAsync=1` 时，NV_ENC_SUCCESS 返回的输出没有立即读取，而是等 completion event。event 在同步返回的场景下不触发，所有输出帧全部丢失 → 生成空文件

**第二版：GPU Direct + 同步 + ready 队列**
- 问题：用 `texture_map_` 以纹理指针为键（ScreenCapture 永远返回同一个 gpu_texture_ 指针），每次提交覆盖上次的映射条目。ReleaseSurface 时拿不到正确的 mapped_resource，解映射失败 → nvEncLockBitstream 返回错误 → 写入 0 帧

**第三版（当前）：纯软件路径（回归旧版方式）**
- 数据流：CopyResource（GPU→staging）→ Map + memcpy → sws_scale（BGRA→NV12）→ LockInputBuffer + memcpy → EncodePicture
- 参数保持 P5 + VBR + 2 B 帧（没有采用 ffmpeg 命令的 P1 + CBR）
- 遗留问题：Flush 卡死，写入 0 帧

---

## 第三阶段：当前剩余问题分析

### 问题 1：Flush 卡死

Flush 中调用 `nvEncEncodePicture(EOS)` 后，循环调用 `ProcessOutput` 读 ready 队列。但当前代码的 SubmitFrame 中没有正确处理 `NV_ENC_ERR_NEED_MORE_INPUT`——surface 放入了 pending 队列，但 `pending_queue_` 和 `ready_queue_` 之间的搬移条件没对齐 FFmpeg。Flush 时 pending 队列不为空但里面 surface 的 bitstream 没有有效数据，`nvEncLockBitstream` 拿不到数据产生异常。

### 问题 2：写入 0 帧

`EncodeFrame` 中：提交 → SendFrame（如果 SUCCESS 则移 pending→ready）→ 立即去 ready 取。这里缺少了 FFmpeg 的 `output_ready` 判断（`nb_ready + nb_pending >= async_depth`）。而且 `frameIntervalP=3` 时前 3 帧都是 NEED_MORE_INPUT，不会有 SUCCESS 产生，所以前三帧没有输出。而调用方在 `EncodeFrame` 返回 false 后就直接继续下一轮，没有 retry。结果 10 秒录制期间 `EncodeFrame` 从未返回过 true。

### 问题 3：参数与 ffmpeg 命令不一致

| 项目   | ffmpeg 命令          | 当前代码                      |
| ------ | -------------------- | ----------------------------- |
| preset | p1                   | P5                            |
| tune   | ll（low latency）    | ULTRA_LOW_LATENCY             |
| 码控   | CBR（-b:v 10M）      | VBR                           |
| B 帧   | 无（preset p1 默认） | 2 个 B 帧（frameIntervalP=3） |
| VBV    | 由 CBR 隐式决定      | 3 帧缓冲区                    |

当 frameIntervalP=3 时，NVENC 需要积累 3 帧才会输出第一帧——这要求调用方有对应的 async_depth 逻辑配合（FFmpeg 的 output_ready 函数）。我们的测试函数没有做这个配合。

---

## 第四阶段：要真正跑通需要改的三件事

### 改 1：参数改为 P1 + CBR + 无 B 帧

`NvEncSession.cpp`：
- `presetGUID = NV_ENC_PRESET_P1_GUID`
- `tuningInfo = NV_ENC_TUNING_INFO_LOW_LATENCY`
- `frameIntervalP = 1`
- `rateControlMode = NV_ENC_PARAMS_RC_CBR`

这样每帧提交都会有输出，不需要三层队列管理，Flush 也不会有 pending 残留。

### 改 2：EncodeFrame 简化

去掉 pending/ready 队列的逻辑。因为 frameIntervalP=1 时每个 EncodePicture 都返回 SUCCESS，submit 后直接 LockBitstream 读输出即可。整个流程退化为：

```
CopyResource → Map → sws_scale → LockInputBuffer → memcpy → UnlockInputBuffer
→ EncodePicture → LockBitstream → memcpy → UnlockBitstream
```

### 改 3：移除 sws_scale 中的 BGRA 临时缓冲

当前 CreateSwsContext 分配了 bgra_cpu_ 和 nv12_cpu_ 两个缓冲区，但实际只用了 nv12_cpu_ 一个（BGRA 数据直接从 staging Map 后的 mapped.pData 传给 sws_scale）。可以简化：直接 sws_scale 从 staging mapped.pData → cpu_nv12_，然后 LockInputBuffer 后 memcpy 过去。

---

以上就是从开始到现在，基于 FFmpeg nvenc.c 做录屏迁移的完整过程。