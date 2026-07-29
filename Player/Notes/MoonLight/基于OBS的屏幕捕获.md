# 基于OBS的屏幕捕获

## 为什么选 OBS 的采集架构

旧的 `Capture/` 目录下有两套手写实现：一套 DXGI Desktop Duplication 采集器（`ScreenCapture`），一套直接调 NVENC SDK 的编码器（`NvencEncoder` + `NvEncLibrary` / `NvEncSession` / `NvEncSurfacePool`）。两套代码都能跑，但各自有盲区：采集端没有 WGC 路线和 GDI 回退，编码端绑死了 NVENC SDK 的 API 细节，换 FFmpeg 就要重写。

OBS 的 `win-capture` 模块经过多年打磨，覆盖了所有 Windows 采集场景。它的核心设计是**多路线自动切换**——DXGI 优先，WGC 补位，GDI 兜底——每条路线都有完整的错误恢复机制。我们把它迁移成 C++ 类，替换掉旧实现，同时把编码器从直接调 NVENC SDK 改为走 FFmpeg `libavcodec`，大幅降低维护成本。

## 采集路线总览

```
┌─────────────────────────────────────────────────────────────────┐
│                      MonitorCapture                              │
│                     (统一入口/自动选择)                           │
├──────────┬──────────────┬───────────────────────────────────────┤
│          │              │                                       │
│  DXGI    │     WGC      │           GDI                         │
│  (GPU)   │    (GPU)     │          (CPU)                        │
│          │              │                                       │
│ DxgiDuplicator  WgcCapture  DcCapture                           │
│ Desktop         Graphics     BitBlt                              │
│ Duplication     Capture                                         │
│ API             API                                             │
└──────────┴──────────────┴───────────────────────────────────────┘
```

| 路线 | API | 输出格式 | 采集对象 | 性能 | 适用场景 |
|------|-----|---------|---------|------|---------|
| DXGI | IDXGIOutputDuplication | GPU BGRA 纹理 | 显示器 | 最高（零拷贝） | 桌面采集首选 |
| WGC | Windows.Graphics.Capture | GPU BGRA 纹理 | 显示器/窗口 | 高 | DXGI 不可用时；窗口采集首选 |
| GDI | BitBlt + GetCursorInfo | CPU BGRA 内存 | 显示器/窗口 | 低 | 无 D3D 设备时的兜底 |

三条路线通过 `MonitorCapture` 统一暴露，调用方不需要关心底层走的哪条：

```cpp
// 调用方视角：三行代码完成采集
MonitorCapture capture;
capture.Init(d3d_device, monitor_id, DisplayCaptureMethod::Auto, true, false);
capture.Tick(1.0f / 60);
```

## 类结构

```
OBS_Capture/
├── CaptureCommon.h           公共类型：CaptureFrame / CursorInfo / MonitorInfo / 枚举
├── CursorCapture.h/.cpp      光标采集与位图缓存
├── DcCapture.h/.cpp          GDI BitBlt 采集器（CPU 路线）
├── DxgiDuplicator.h/.cpp     DXGI Desktop Duplication 采集器（GPU 路线）
├── WgcCapture.h/.cpp         Windows Graphics Capture 采集器（GPU 路线）
├── MonitorCapture.h/.cpp     显示器采集统一入口（自动选择 DXGI/WGC/GDI）
├── WindowCapture.h/.cpp      窗口采集统一入口（自动选择 WGC/BitBlt）
└── ObsNvencEncoder.h/.cpp    FFmpeg NVENC 编码器（H.264 硬件编码）
```

### 类职责

| 类 | 职责 | 对应 OBS 源文件 |
|----|------|----------------|
| `DxgiDuplicator` | 通过 DXGI Desktop Duplication 采集显示器帧 | `duplicator-monitor-capture.c` + `gs-duplicator.c` |
| `WgcCapture` | 通过 Windows Graphics Capture API 采集显示器/窗口 | `wgc-capture.c` + `wgc-source.c` |
| `DcCapture` | 通过 GDI BitBlt 采集桌面/窗口，支持光标叠加 | `dc-capture.c` |
| `CursorCapture` | 采集系统光标状态，解析为 BGRA 位图 | `cursor-capture.c` |
| `MonitorCapture` | 显示器采集外观，封装路线选择和错误恢复 | `duplicator-monitor-capture.c` 的上层逻辑 |
| `WindowCapture` | 窗口采集外观，封装路线选择和 DPI 处理 | `window-capture.c` 的上层逻辑 |
| `ObsNvencEncoder` | 用 FFmpeg libavcodec 封装 NVENC，BGRA→NV12→H.264 | 无（替代旧的 NvencEncoder） |

## DXGI 路线：DxgiDuplicator

### 工作原理

DXGI Desktop Duplication API（Windows 8+）通过 `IDXGIOutputDuplication` 接口直接读取 GPU 上的桌面纹理。整个链路全程在 GPU 内部，不经过 CPU：

```
GPU 桌面纹理 ──AcquireNextFrame──→ IDXGIResource
                                      │
                                   QI 获取 ID3D11Texture2D
                                      │
                                   CopyResource ──→ 私有 target_texture_
                                      │
                                   ReleaseFrame
```

### 关键设计：ReleaseFrame 前必须拷贝

这是整个迁移过程中踩过的最关键的坑。`IDXGIOutputDuplication` 的 `AcquireNextFrame` 返回的纹理是 DXGI 内部环形缓冲区的一个槽位。调用 `ReleaseFrame` 后，这个槽位被标记为可重用——虽然 COM 引用计数 > 0（QI 做了 AddRef），纹理对象没被销毁，但 **GPU 显存内容可以被 DXGI 在任何时刻覆盖**。

```cpp
// 错误做法（会导致黑屏）：
AcquireNextFrame → QI 获取纹理 → ReleaseFrame → 保留纹理指针
// 后续 CopyResource 读到的是全零数据 → NV12 Y=16（BT.601 黑色）→ 黑屏

// 正确做法（与 OBS gs_duplicator 一致）：
AcquireNextFrame → QI 获取纹理 → CopyResource 到私有纹理 → ReleaseFrame
```

`DxgiDuplicator::UpdateFrame` 的核心实现：

```cpp
// ---- 获取桌面纹理 ----
ID3D11Texture2D* desktop_texture = nullptr;
resource->QueryInterface(__uuidof(ID3D11Texture2D),
                         reinterpret_cast<void**>(&desktop_texture));
resource->Release();

// ---- 读取尺寸和格式 ----
D3D11_TEXTURE2D_DESC desc;
desktop_texture->GetDesc(&desc);

// ---- 创建或重建 target_texture_（尺寸变化时） ----
if (!target_texture_ || target_width_ != desc.Width || target_height_ != desc.Height)
{
    D3D11_TEXTURE2D_DESC target_desc = desc;
    target_desc.Usage = D3D11_USAGE_DEFAULT;
    target_desc.BindFlags = 0;
    target_desc.CPUAccessFlags = 0;
    target_desc.MiscFlags = 0;
    device_->CreateTexture2D(&target_desc, nullptr, &target_texture_);
}

// ---- 关键：在 ReleaseFrame 前拷贝到私有纹理 ----
context_->CopyResource(target_texture_, desktop_texture);

// ---- 释放桌面纹理并 ReleaseFrame ----
desktop_texture->Release();
duplication_->ReleaseFrame();
```

### 错误恢复

DXGI Desktop Duplication 有两种需要重建的场景：

- `DXGI_ERROR_ACCESS_LOST`：桌面会话切换、UAC 弹窗、显示器模式变化等，需要重建 `IDXGIOutputDuplication`
- `DXGI_ERROR_WAIT_TIMEOUT`：桌面无变化，保留上一帧（不是错误）

`MonitorCapture::Tick` 内部维护一个 3 秒的重试计时器（`RESET_INTERVAL_SEC`），`DxgiDuplicator` 失效后不立即重试，等 3 秒再重建，避免在 UAC 弹窗等场景下频繁重试。

## WGC 路线：WgcCapture

### 工作原理

Windows Graphics Capture API（Windows 10 1903+）是微软推荐的现代采集方案。与 DXGI 不同，它是事件驱动的——通过 `FrameArrived` 事件通知新帧到达，采集器在 `Tick` 中调用 `TryGetNextFrame` 获取帧：

```
GraphicsCaptureItem ──→ Direct3D11CaptureFramePool ──→ FrameArrived 事件
                                                         │
                                                      设置标志
                                                         │
                                                   WgcCapture::Tick
                                                         │
                                                   TryGetNextFrame
                                                         │
                                                   IDirect3DSurface
                                                         │
                                                   QI → ID3D11Texture2D
```

### WinRT ABI 投影

WGC 是 WinRT API，在 C++ 中通过 ABI（Application Binary Interface）投影调用。不使用 C++/WinRT，而是直接用 SDK 头文件中的 ABI 接口：

```cpp
// WinRT 接口通过 ABI 命名空间访问
ABI::Windows::Graphics::Capture::IGraphicsCaptureItem* item_{nullptr};
ABI::Windows::Graphics::Capture::IDirect3D11CaptureFramePool* frame_pool_{nullptr};
ABI::Windows::Graphics::Capture::IGraphicsCaptureSession* session_{nullptr};
```

从 D3D11 设备创建 WinRT Direct3D 设备，需要动态加载 `windows.graphics.directx.direct3d11.dll` 中的 `CreateDirect3D11DeviceFromDXGIDevice` 函数。

### 事件处理器的类型陷阱

`FrameArrived` 事件的处理器类型是 `ITypedEventHandler<Direct3D11CaptureFramePool*, IInspectable*>`。SDK 头文件对 `ITypedEventHandler` 做了特化，将运行时类名 `Direct3D11CaptureFramePool` 映射到接口 `IDirect3D11CaptureFramePool`。

编写事件处理器时的注意事项：

```cpp
// 正确：第二个参数是裸 IInspectable*（全局 COM 接口，不属于任何命名空间）
using FramePoolHandler = ABI::Windows::Foundation::ITypedEventHandler<
    ABI::Windows::Graphics::Capture::Direct3D11CaptureFramePool*,
    IInspectable*>;

// 错误：ABI::Windows::Foundation::IInspectable 不存在，会导致编译器内部错误（C1001 ICE）
```

WRL 的 `RuntimeClass` 通过 `FramePoolHandler` 和 `FtmBase` 两条路径继承 `IUnknown`（菱形继承），`ComPtr` 的隐式转换会因基类二义性失败。解决方式是用 `.As()` 走 `QueryInterface`：

```cpp
auto handler = Microsoft::WRL::Make<FrameArrivedHandler>(this);
// 不能直接赋值：frame_arrived_handler_ = handler;
handler.As(&frame_arrived_handler_);  // 通过 QI 绕过二义性
```

### 光标控制

光标采集通过 `IGraphicsCaptureSession2` 接口的 `put_IsCursorCaptureEnabled` 方法控制。这个接口需要从 `IGraphicsCaptureSession` 通过 `QueryInterface` 获取，不是所有系统版本都有（Windows 10 2004+）。

## GDI 路线：DcCapture

GDI 是最古老的采集方式，通过 `BitBlt` 从屏幕 DC 拷贝到内存 DC。性能低（CPU 介入、全量拷贝），但兼容性最好——不需要 D3D 设备，在远程桌面、安全模式等场景下是唯一的选项。

`DcCapture` 内部维护一个 DIB Section 作为 CPU 可直接访问的帧缓冲：

```cpp
// 创建兼容 DC 和 DIB Section
HDC screen_dc = GetDC(nullptr);
hdc_ = CreateCompatibleDC(screen_dc);
// DIB Section 的 bits_ 指针直接映射 CPU 内存，无需 BitBlt 后再 GetDIBits
bmp_ = CreateDIBSection(hdc_, &bmi, DIB_RGB_COLORS,
                         reinterpret_cast<void**>(&bits_), nullptr, 0);
ReleaseDC(nullptr, screen_dc);

// 采集时：BitBlt 直接写入 DIB Section
BitBlt(hdc_, 0, 0, width_, height_, screen_dc, x_, y_, SRCCOPY);
// bits_ 立即可读，无需额外拷贝
```

GDI 路线的光标通过 `DrawIconEx` 直接叠加到 DC 上，不需要外部合成。

## MonitorCapture 的自动选择策略

`MonitorCapture::ChooseMethod` 按以下优先级选择采集路线：

1. **无 D3D 设备** → GDI 回退（`use_gdi_ = true`）
2. **WGC 不支持** → DXGI
3. **Auto 模式** → 默认 DXGI，以下条件切 WGC：
   - `DxgiDuplicator::GetMonitorIndex` 返回 -1（DXGI 找不到该显示器）
   - 笔记本电池供电 + 双显卡（省电/兼容性考虑）

Auto 模式默认选 DXGI 而非 WGC，因为 DXGI 的延迟更低（无事件驱动开销），且不需要 WinRT 初始化。WGC 作为 DXGI 不可用时的备选方案。

## FFmpeg NVENC 编码器：ObsNvencEncoder

### 数据流

```
ID3D11Texture2D (GPU BGRA)
    │
    ├─ CopyResource → staging 纹理 (CPU 可读)
    │
    ├─ Map → CPU BGRA 内存
    │
    ├─ sws_scale → CPU NV12
    │
    ├─ avcodec_send_frame → NVENC 硬件编码
    │
    └─ avcodec_receive_packet → H.264 码流
```

这条路径与 FFmpeg `nvenc.c` 中的 `nvenc_upload_frame` 软件路径一致——GPU 纹理先回读到 CPU，转成 NV12，再送入 NVENC 编码。对应的 GPU 直传路径（通过 D3D11VA 上下文直接传 GPU 纹理）目前未实现，因为软件路径在 1080p60 下性能已经足够。

### 编码参数

与 FFmpeg 命令行 `-preset p1 -tune ll -rc cbr` 保持一致：

```cpp
codec_ctx_->pix_fmt = AV_PIX_FMT_NV12;
codec_ctx_->bit_rate = bitrate_kbps * 1000;
codec_ctx_->gop_size = fps * 2;        // GOP = 2 秒
codec_ctx_->max_b_frames = 0;          // 无 B 帧（低延迟）

// NVENC 专有参数
av_opt_set(codec_ctx_->priv_data, "preset", "p1", 0);      // 最快预设
av_opt_set(codec_ctx_->priv_data, "tune", "ll", 0);        // 低延迟调优
av_opt_set(codec_ctx_->priv_data, "rc", "cbr", 0);         // 恒定码率
av_opt_set(codec_ctx_->priv_data, "forced-idr", "1", 0);   // 允许强制 IDR
```

### 为什么不直接调 NVENC SDK

旧的 `NvencEncoder` 直接调 NVENC SDK 的 C API（`nvEncCreateInputBuffer` / `nvEncLockInputBuffer` / `nvEncEncodePicture` / `nvEncLockBitstream`），需要处理表面池管理、输入缓冲锁、码流锁等底层细节，代码量是 FFmpeg 封装版的 4 倍。FFmpeg 的 `libavcodec` 已经封装了所有这些细节，两行代码完成一帧编码：

```cpp
avcodec_send_frame(codec_ctx_, frame_);      // 送入 NV12 帧
avcodec_receive_packet(codec_ctx_, packet_); // 取出 H.264 码流
```

FFmpeg 的 `h264_nvenc` 编码器内部就是调 NVENC SDK，功能完全等价，但维护成本大幅降低。

### NV12 布局

NV12 是 NVENC 的标准输入格式，由一个 Y 平面和一个 UV 交错平面组成：

```
NV12 内存布局（width=1920, height=1080）:
┌─────────────────────────────────┐
│  Y 平面: 1920 × 1080 字节       │  ← nv12_data_[0]
├─────────────────────────────────┤
│  UV 交错平面: 1920 × 540 字节   │  ← nv12_data_[1920 * 1080]
└─────────────────────────────────┘
总计: 1920 × 1080 × 1.5 = 3,110,400 字节
```

`sws_scale` 负责将 BGRA（4 字节/像素）转为 NV12（1.5 字节/像素），行间距等于宽度（无对齐填充）。

## 完整采集编码链路

`PlayerApp::TestScreenCapture` 是完整的测试函数，演示了从采集到编码的完整流程：

```cpp
void PlayerApp::TestScreenCapture()
{
    // ---- 第一步：创建 D3D11 设备（采集器和编码器共用） ----
    D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                      D3D11_CREATE_DEVICE_VIDEO_SUPPORT, ...);

    // ---- 第二步：枚举显示器 ----
    auto monitors = MonitorCapture::EnumerateMonitors();
    const char* monitor_id = monitors[0].device_id;

    // ---- 第三步：初始化采集器 ----
    MonitorCapture capture;
    capture.Init(d3d_device, monitor_id,
                 DisplayCaptureMethod::Auto, true, false);

    // ---- 第四步：等首帧到达 ----
    while (wait < 300)
    {
        capture.Tick(1.0f / fps);
        if (capture.GetFrame(frame) && frame.IsValid())
            break;
        Sleep(16);
    }

    // ---- 第五步：初始化编码器 ----
    ObsNvencEncoder encoder;
    encoder.Init(d3d_device, width, height, fps, 10000);

    // ---- 第六步：主循环（采集 → 编码 → 写文件） ----
    while (written < total)
    {
        capture.Tick(1.0f / fps);
        capture.GetFrame(frame);

        ID3D11Texture2D* tex = nullptr;
        if (frame.IsGpu())
        {
            tex = frame.gpu_texture;           // DXGI/WGC 路线
        }
        else
        {
            d3d_ctx->UpdateSubresource(upload_tex, ...,
                frame.cpu_data, ...);          // GDI 路线：CPU→GPU 上传
            tex = upload_tex;
        }

        encoder.EncodeFrame(tex, idx, (idx == 0), h264_data);
        fwrite(h264_data.data(), 1, h264_data.size(), fp);
    }

    // ---- 第七步：Flush 编码器剩余帧 ----
    encoder.Flush([&](const std::vector<uint8_t>& data)
    {
        fwrite(data.data(), 1, data.size(), fp);
    });
}
```

### GDI 路线的 CPU→GPU 上传

DXGI 和 WGC 路线直接输出 GPU 纹理，编码器可以直接用。GDI 路线输出的是 CPU 内存（`frame.cpu_data`），需要通过 `UpdateSubresource` 上传到 GPU 纹理后才能交给编码器：

```cpp
if (!frame.IsGpu())
{
    // 创建一个 BGRA 纹理用于 CPU→GPU 上传
    d3d_ctx->UpdateSubresource(upload_tex, 0, nullptr,
                                frame.cpu_data, frame.cpu_stride, 0);
    tex = upload_tex;
}
```

## 踩坑记录

### 黑屏：DXGI ReleaseFrame 后纹理内容失效

现象：ffplay 播放生成的 H.264 文件，能正常播放但画面全黑。诊断日志显示 staging 纹理首像素为 `00 00 00 00`，NV12 Y 值为 `10`（十六进制 0x10 = 16，即 BT.601 黑色的 Y 分量）。

原因：原实现在 `AcquireNextFrame` 后立即 `ReleaseFrame`，然后保留桌面纹理指针。DXGI 的 `ReleaseFrame` 会将纹理槽位标记为可重用，后续 `CopyResource` 读到的是已被 DXGI 回收的显存内容。

修复：在 `ReleaseFrame` 前将桌面纹理拷贝到私有 `target_texture_`，`GetTexture` 返回 `target_texture_` 而非桌面纹理。

### WGC 编译器内部错误（C1001 ICE）

现象：`WgcCapture.cpp` 第 39 行报 `fatal error C1001: 内部编译器错误`。

原因：`IInspectable` 误写为 `ABI::Windows::Foundation::IInspectable`。`IInspectable` 是全局 COM 接口（定义在 `inspectable.h`），不属于 `ABI::Windows::Foundation` 命名空间。MSVC 在解析这个不存在的类型时直接崩溃。

修复：去掉命名空间前缀，使用裸 `IInspectable*`。SDK 头文件中的 typedef 也是这么写的。

### WGC ComPtr 赋值二义性

现象：`frame_arrived_handler_ = handler` 编译失败，报 `无法从 ComPtr<FrameArrivedHandler> 转换为 ComPtr<IUnknown>`。

原因：`FrameArrivedHandler` 通过 `FramePoolHandler`（继承 `IInspectable`→`IUnknown`）和 `FtmBase`（继承 `IUnknown`）两条路径继承 `IUnknown`，形成菱形继承。`ComPtr` 的赋值运算符无法在编译期确定用哪个 `IUnknown`。

修复：用 `handler.As(&frame_arrived_handler_)` 走 `QueryInterface(__uuidof(IUnknown))`，运行时解决二义性。

### const 成员函数锁 mutex

现象：`GetTexture() const` 中 `std::lock_guard<std::mutex> lock(texture_mutex_)` 编译失败。

原因：`const` 成员函数中 `texture_mutex_` 变成 `const std::mutex`，而 `lock_guard` 构造需要非 const 引用。

修复：`texture_mutex_` 声明为 `mutable std::mutex`。
