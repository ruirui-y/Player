# FFmpeg TCP 播放器 — 从 CPU 到 GPU 的演进总结

## 一、当前版本的渲染管线

这个版本有一个关键的变化：**它同时实现了两条渲染管线，但目前跑的是混合模式。**

### 解码层：已经是 GPU

`FFmpegDecoder::OpenFile(path, true)` 调用 `av_hwdevice_ctx_create` 创建 D3D11VA 硬件设备，解码器走的是 GPU 硬解。这块没有问题，解码不占 CPU。

### 渲染层：两条管线并存

| 管线                                                    | 实现状态                                                 | 当前是否启用                |
| ------------------------------------------------------- | -------------------------------------------------------- | --------------------------- |
| **CPU 软渲染** (download → sws_scale → QImage → QLabel) | 完整实现                                                 | **是（当前正在跑的）**      |
| **GPU 纯硬件管线** (HLSL Shader → Present)              | 完整实现：含 VS/PS 着色器 + InitShaders + RenderHardware | **否（Render 分派未切换）** |

### 为什么叫"混合模式"

数据流是：

```
GPU 解码（硬解）→ av_hwframe_transfer_data（GPU→CPU 下载）
→ sws_scale（CPU 做 YUV→RGB）
→ QImage（CPU 内存）
→ emit 信号到主线程（跨线程拷贝）
→ QLabel::setPixmap（主线程再拷贝一次）
→ 显示
```

解码在 GPU，颜色转换和显示全部在 CPU。4K 能看但卡，瓶颈在：
- `av_hwframe_transfer_data`：把 4K NV12 从显存搬回内存，一次几毫秒
- `sws_scale`：CPU 做 4096x2048 的 YUV→RGB 转换，一次十几毫秒
- QImage 跨线程拷贝 + QLabel 绘制：主线程开销

---

## 二、纯 GPU 管线已经就绪

另一个 AI 在渲染器里加了完整的 HLSL 着色器渲染管线：

### 着色器

一个嵌入在 C++ 字符串里的 HLSL 程序，包含两个着色器：

**顶点着色器 VS：** 利用 `SV_VertexID` 凭空生成一个覆盖全屏的大三角形，不需要顶点缓冲区。

**像素着色器 PS：** 接收两个纹理（Y 平面 + UV 平面），采样后用 BT.709 色彩矩阵做 YUV→RGB 转换。

### InitShaders

用 `D3DCompile` 在运行时编译 HLSL 代码，创建 `ID3D11VertexShader`、`ID3D11PixelShader`、`ID3D11SamplerState`。

在 `CreateSwapChain` 成功后自动调用，所以只要硬件解码成功、交换链创建成功，着色器也一并初始化好了。

### RenderHardware

完整的 GPU 渲染流程：
1. 从 `frame->data[0]` 拿到解码器的 `ID3D11Texture2D`
2. 用 `D3D11_SRV_DIMENSION_TEXTURE2DARRAY` 为 Y 平面（R8_UNORM）和 UV 平面（R8G8_UNORM）分别创建着色器资源视图 SRV
3. 把交换链的后备缓冲绑定为渲染目标 RTV
4. 设置视口、绑定着色器和采样器
5. `Draw(3, 0)` 触发全屏三角形绘制
6. `Present(0, 0)` 上屏

**整个过程：解码器的 GPU 纹理 → 着色器直接采样 → Present 到屏幕，完全不经过 CPU。**

### 链接库

项目已经加上了 `d3dcompiler.lib` 的链接，编译没问题。

---

## 三、唯一没做完的事：把 Render 分派切换到 GPU 管线

当前 `VideoRenderer::Render` 函数的逻辑是：

```cpp
if (fmt == AV_PIX_FMT_D3D11 || fmt == AV_PIX_FMT_D3D11VA_VLD || fmt == AV_PIX_FMT_DXVA2_VLD)
{
    // 走 CPU 下载 + sws_scale 路径
    av_hwframe_transfer_data → RenderSoftware
    return;
}
// 纯软解格式
RenderSoftware(frame);
```

要在纯 GPU 模式下跑，只需要改成：

```cpp
if (fmt == AV_PIX_FMT_D3D11 && swapchain_ && pixel_shader_)
{
    RenderHardware(frame);  // 全 GPU 管线
    return;
}
if (fmt == AV_PIX_FMT_D3D11VA_VLD || fmt == AV_PIX_FMT_DXVA2_VLD)
{
    // 这些格式没有完整的纹理数组结构，还是走 CPU 下载
    av_hwframe_transfer_data → RenderSoftware
    return;
}
RenderSoftware(frame);
```

关键取决于解码器输出的帧格式：
- 如果是 `AV_PIX_FMT_D3D11`（170）→ 走 RenderHardware，全 GPU
- 如果是 `DXVA2_VLD`（171）→ 走 CPU 下载，因为 DXVA2 的纹理布局不同

---

## 四、从 CPU 到 GPU 的演进路线

```
阶段一（最初）：
解码：CPU（sws_scale）→ 软解整个流程
渲染：CPU（QImage → QLabel）
性能：4K 一帧几十到上百毫秒，卡死

阶段二（上一步的混合模式）：
解码：GPU（D3D11VA 硬解）
渲染：CPU（av_hwframe_transfer_data → sws_scale → QLabel）
性能：4K 能看，~15fps，CPU 占用高

阶段三（当前，就绪但未切换）：
解码：GPU（D3D11VA 硬解）
渲染：GPU（HLSL Shader → Present）
性能：预期 4K 60fps 流畅，CPU 占用极低
```

**阶段三只需要改 `Render()` 里的一行分派逻辑就能生效。**