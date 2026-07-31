#include "StreamServer.h"

#include <WinSock2.h>
#include <WS2tcpip.h>
#include <d3d11.h>
#include <chrono>
#include <thread>
#include <cstdio>
#include <vector>

#include "Common/NetWork/VideoSender.h"
#include "Common/Input/InputTransport.h"
#include "Common/Input/InputInjector.h"
#include "Common/Input/InputEvent.h"
#include "Common/LogManager.h"
#include "Server/OBS_Capture/MonitorCapture.h"
#include "Server/OBS_Capture/IVideoEncoder.h"
#include "Server/OBS_Capture/ObsNvencEncoder.h"
#include "Server/OBS_Capture/ObsNvencEncoderFast.h"

// ---- 构造 ----
StreamServer::StreamServer()
{
}

// ---- 析构：逆序释放所有资源 ----
StreamServer::~StreamServer()
{
    Stop();

    // ---- 第三阶段：先停输入组件 ----
    if (input_server_)  { input_server_->Stop(); delete input_server_;  input_server_  = nullptr; }
    if (input_injector_) { delete input_injector_; input_injector_ = nullptr; }

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
                        const char* dest_ip, int fps, int bitrate_kbps,
                        uint16_t ctrl_port, bool use_fast)
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
        LogManager::Log("ERR", "[StreamServer] D3D11 设备创建失败, HR = 0x%08X", (unsigned)hr);
        return false;
    }

    // ---- 第二步：枚举显示器，取指定索引 ----
    auto monitors = MonitorCapture::EnumerateMonitors();
    if (monitors.empty() || monitor_index >= (int)monitors.size())
    {
        LogManager::Log("ERR", "[StreamServer] 显示器索引 %d 无效（共 %zu 个显示器）",
                        monitor_index, monitors.size());
        return false;
    }

    LogManager::Log("INFO", "[StreamServer] 目标显示器[%d]: %s  %dx%d",
                    monitor_index, monitors[monitor_index].name,
                    monitors[monitor_index].rect.right - monitors[monitor_index].rect.left,
                    monitors[monitor_index].rect.bottom - monitors[monitor_index].rect.top);

    const char* monitor_id = monitors[monitor_index].device_id;

    // ---- 第三步：初始化采集器 ----
    capture_ = new MonitorCapture();
    if (!capture_->Init(d3d_device_, monitor_id,
                        DisplayCaptureMethod::Auto, true, false))
    {
        LogManager::Log("ERR", "[StreamServer] 采集器初始化失败");
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
        LogManager::Log("ERR", "[StreamServer] 首帧超时");
        return false;
    }

    width_ = static_cast<int>(frame.width);
    height_ = static_cast<int>(frame.height);
    LogManager::Log("INFO", "[StreamServer] 首帧: %dx%d %s",
                    width_, height_, frame.IsGpu() ? "GPU" : "CPU");

    // ---- 第五步：初始化编码器（按 use_fast 选择 CPU/GPU 色彩转换路线） ----
    if (use_fast)
    {
        encoder_ = new ObsNvencEncoderFast();
        LogManager::Log("INFO", "[StreamServer] 使用 GPU 硬件色彩转换 (ObsNvencEncoderFast)");
    }
    else
    {
        encoder_ = new ObsNvencEncoder();
        LogManager::Log("INFO", "[StreamServer] 使用 CPU 软件色彩转换 (ObsNvencEncoder)");
    }

    if (!encoder_->Init(d3d_device_, width_, height_, fps_, bitrate_kbps))
    {
        LogManager::Log("ERR", "[StreamServer] 编码器初始化失败");
        return false;
    }

    // ---- 第六步：初始化 UDP 发送器 ----
    sender_ = new VideoSender();
    if (!sender_->Init(dest_ip, port))
    {
        LogManager::Log("ERR", "[StreamServer] VideoSender 初始化失败");
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

    // ---- 第八步：初始化输入控制组件（第三阶段：TCP 控制信道 + SendInput 注入） ----
    input_injector_ = new InputInjector();

    input_server_ = new InputTransportServer();
    if (!input_server_->Listen(ctrl_port))
    {
        LogManager::Log("ERR", "[StreamServer] 输入控制信道监听失败 (port %d)", ctrl_port);
        return false;
    }

    // ---- 设置显示器信息（客户端需要此信息做正确的多显示器坐标映射） ----
    {
        ServerMonitorInfo info{};
        info.capture_width = static_cast<uint16_t>(width_);
        info.capture_height = static_cast<uint16_t>(height_);
        info.monitor_x = static_cast<int16_t>(capture_->MonitorX());
        info.monitor_y = static_cast<int16_t>(capture_->MonitorY());
        info.virtual_width = static_cast<uint32_t>(GetSystemMetrics(SM_CXVIRTUALSCREEN));
        info.virtual_height = static_cast<uint32_t>(GetSystemMetrics(SM_CYVIRTUALSCREEN));
        input_server_->SetMonitorInfo(info);
        LogManager::Log("INFO", "[StreamServer] 显示器信息: capture=%dx%d  offset=(%d,%d)  virtual=%dx%d",
                        info.capture_width, info.capture_height,
                        info.monitor_x, info.monitor_y,
                        info.virtual_width, info.virtual_height);
    }

    // 收到客户端输入事件 → 注入到本地系统
    input_server_->OnInputEvent = [this](const InputMessage& msg)
    {
        if (input_injector_)
            input_injector_->Inject(msg);
    };

    input_server_->Start();
    LogManager::Log("INFO", "[StreamServer] 输入控制信道已启动 (port %d)", ctrl_port);

    LogManager::Log("INFO", "[StreamServer] 初始化完成 -> %s:%d  %dx%d@%dfps  %dbps  ctrl:%d",
                    dest_ip, port, width_, height_, fps_, bitrate_kbps * 1000, ctrl_port);
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

    LogManager::Log("INFO", "[StreamServer] 开始推流... (Ctrl+C 停止)");

    while (running_)
    {
        auto frame_start = std::chrono::steady_clock::now();

        // ---- 第一步：采集 ----
        capture_->Tick(1.0f / fps_);

        CaptureFrame frame;
        if (!capture_->GetFrame(frame) || !frame.IsValid())
        {
            // ---- 采集失败，记录日志（每秒一次避免刷屏） ----
            consecutive_failures_++;
            if (consecutive_failures_ % fps_ == 1)
            {
                LogManager::Log("WARN", "[StreamServer] 采集失败，连续 %d 次",
                                consecutive_failures_);
            }

            auto elapsed = std::chrono::steady_clock::now() - frame_start;
            if (elapsed < frame_duration)
                std::this_thread::sleep_for(frame_duration - elapsed);
            continue;
        }

        // ---- 采集恢复日志 ----
        if (consecutive_failures_ > 0)
        {
            LogManager::Log("INFO", "[StreamServer] 采集恢复，之前连续失败 %d 次",
                            consecutive_failures_);
            consecutive_failures_ = 0;
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

        if (encoder_->EncodeFrame(tex, frame_index, (frame_index == 0), h264_data,
                                  nullptr,
                                  capture_->MonitorX(), capture_->MonitorY())
            && !h264_data.empty())
        {
            sender_->SendFrame(h264_data.data(), (int)h264_data.size(),
                               (uint16_t)(frame_index & 0xFFFF),
                               timestamp, (frame_index == 0));
        }

        ++frame_index;

        // ---- 帧率限制：剩余时间休眠 ----
        auto elapsed = std::chrono::steady_clock::now() - frame_start;
        if (elapsed < frame_duration)
            std::this_thread::sleep_for(frame_duration - elapsed);
    }

    LogManager::Log("INFO", "[StreamServer] 推流结束，共 %llu 帧", (unsigned long long)frame_index);
}

// ---- 停止主循环 ----
void StreamServer::Stop()
{
    running_ = false;
}
