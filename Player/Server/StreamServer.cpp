#include "StreamServer.h"

#include <WinSock2.h>
#include <WS2tcpip.h>
#include <d3d11.h>
#include <chrono>
#include <thread>
#include <cstdio>
#include <vector>

#include "Common/NetWork/VideoSender.h"
#include "Server/OBS_Capture/MonitorCapture.h"
#include "Server/OBS_Capture/ObsNvencEncoder.h"

// ---- 构造 ----
StreamServer::StreamServer()
{
}

// ---- 析构：逆序释放所有资源 ----
StreamServer::~StreamServer()
{
    Stop();

    if (sender_)   { sender_->Close();  delete sender_;   sender_   = nullptr; }
    if (encoder_)  { encoder_->Release(); delete encoder_; encoder_  = nullptr; }
    if (capture_)  { capture_->Shutdown(); delete capture_; capture_  = nullptr; }
    if (upload_tex_) { upload_tex_->Release(); upload_tex_ = nullptr; }
    if (d3d_ctx_)    { d3d_ctx_->Release();    d3d_ctx_    = nullptr; }
    if (d3d_device_) { d3d_device_->Release(); d3d_device_ = nullptr; }
}

// ================================================================
// ---- 初始化：创建 D3D11 → 枚举显示器 → 采集器 → 编码器 → 发送器 ----
// ================================================================
bool StreamServer::Init(uint16_t port, int monitor_index,
                        const char* dest_ip, int fps, int bitrate_kbps)
{
    fps_ = fps;

    // ---- 第一步：创建 D3D11 设备（需要 VIDEO_SUPPORT 给 NVENC 用） ----
    D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    HRESULT hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
        levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
        &d3d_device_, nullptr, &d3d_ctx_);
    if (FAILED(hr))
    {
        printf("[StreamServer] D3D11 设备创建失败, HR = 0x%08X\n", (unsigned)hr);
        return false;
    }

    // ---- 第二步：枚举显示器，取指定索引 ----
    auto monitors = MonitorCapture::EnumerateMonitors();
    if (monitors.empty() || monitor_index >= (int)monitors.size())
    {
        printf("[StreamServer] 显示器索引 %d 无效（共 %zu 个显示器）\n",
               monitor_index, monitors.size());
        return false;
    }

    printf("[StreamServer] 目标显示器[%d]: %s  %dx%d\n",
           monitor_index, monitors[monitor_index].name,
           monitors[monitor_index].rect.right - monitors[monitor_index].rect.left,
           monitors[monitor_index].rect.bottom - monitors[monitor_index].rect.top);

    const char* monitor_id = monitors[monitor_index].device_id;

    // ---- 第三步：初始化采集器 ----
    capture_ = new MonitorCapture();
    if (!capture_->Init(d3d_device_, monitor_id,
                        DisplayCaptureMethod::Auto, true, false))
    {
        printf("[StreamServer] 采集器初始化失败\n");
        return false;
    }

    // ---- 第四步：等首帧到达（最多约 5 秒） ----
    CaptureFrame frame;
    int wait = 0;
    while (wait < 300)
    {
        capture_->Tick(1.0f / fps_);
        if (capture_->GetFrame(frame) && frame.IsValid())
            break;
        ++wait;
        Sleep(16);
    }

    if (!frame.IsValid())
    {
        printf("[StreamServer] 首帧超时\n");
        return false;
    }

    width_ = static_cast<int>(frame.width);
    height_ = static_cast<int>(frame.height);
    printf("[StreamServer] 首帧: %dx%d %s\n", width_, height_,
           frame.IsGpu() ? "GPU" : "CPU");

    // ---- 第五步：初始化编码器 ----
    encoder_ = new ObsNvencEncoder();
    if (!encoder_->Init(d3d_device_, width_, height_, fps_, bitrate_kbps))
    {
        printf("[StreamServer] 编码器初始化失败\n");
        return false;
    }

    // ---- 第六步：初始化 UDP 发送器 ----
    sender_ = new VideoSender();
    if (!sender_->Init(dest_ip, port))
    {
        printf("[StreamServer] VideoSender 初始化失败\n");
        return false;
    }

    // ---- 第七步：GDI 路线需要创建上传纹理 ----
    // DXGI/WGC 路线直接给 GPU 纹理，GDI 路线给 CPU 数据需要上传到 GPU
    if (!frame.IsGpu())
    {
        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = width_;
        desc.Height = height_;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        d3d_device_->CreateTexture2D(&desc, nullptr, &upload_tex_);
    }

    printf("[StreamServer] 初始化完成 -> %s:%d  %dx%d@%dfps  %dbps\n",
           dest_ip, port, width_, height_, fps_, bitrate_kbps * 1000);
    return true;
}

// ================================================================
// ---- 主循环：按帧率采集 → 编码 → UDP 发送 ----
// ================================================================
void StreamServer::Run()
{
    running_ = true;

    // ---- 帧间隔（微秒精度） ----
    auto frame_duration = std::chrono::microseconds(1000000 / fps_);
    uint64_t frame_index = 0;

    printf("[StreamServer] 开始推流... (Ctrl+C 停止)\n");

    while (running_)
    {
        auto frame_start = std::chrono::steady_clock::now();

        // ---- 第一步：采集 ----
        capture_->Tick(1.0f / fps_);

        CaptureFrame frame;
        if (!capture_->GetFrame(frame) || !frame.IsValid())
        {
            // 帧未就绪，等待下一周期
            auto elapsed = std::chrono::steady_clock::now() - frame_start;
            if (elapsed < frame_duration)
                std::this_thread::sleep_for(frame_duration - elapsed);
            continue;
        }

        // ---- 第二步：获取纹理 ----
        ID3D11Texture2D* tex = nullptr;
        if (frame.IsGpu())
        {
            // DXGI/WGC 路线：直接用 GPU 纹理
            tex = frame.gpu_texture;
        }
        else if (upload_tex_ && frame.cpu_data)
        {
            // GDI 路线：CPU 数据上传到 GPU 纹理
            d3d_ctx_->UpdateSubresource(upload_tex_, 0, nullptr,
                frame.cpu_data, frame.cpu_stride, 0);
            tex = upload_tex_;
        }

        if (!tex)
        {
            auto elapsed = std::chrono::steady_clock::now() - frame_start;
            if (elapsed < frame_duration)
                std::this_thread::sleep_for(frame_duration - elapsed);
            continue;
        }

        // ---- 第三步：编码 + 发送 ----
        std::vector<uint8_t> h264_data;
        uint32_t timestamp = static_cast<uint32_t>(frame_index * (1000 / fps_));

        if (encoder_->EncodeFrame(tex, frame_index, (frame_index == 0), h264_data)
            && !h264_data.empty())
        {
            sender_->SendFrame(h264_data.data(), (int)h264_data.size(),
                               (uint16_t)(frame_index & 0xFFFF),
                               timestamp, (frame_index == 0));
        }

        ++frame_index;

        // ---- 每秒打印一次状态 ----
        if (frame_index % fps_ == 0)
        {
            printf("[StreamServer] 已推流 %llu 帧 (%.1f 秒)\n",
                   (unsigned long long)frame_index, (double)frame_index / fps_);
        }

        // ---- 帧率限制：剩余时间休眠 ----
        auto elapsed = std::chrono::steady_clock::now() - frame_start;
        if (elapsed < frame_duration)
            std::this_thread::sleep_for(frame_duration - elapsed);
    }

    printf("[StreamServer] 推流结束，共 %llu 帧\n", (unsigned long long)frame_index);
}

// ---- 停止主循环 ----
void StreamServer::Stop()
{
    running_ = false;
}
