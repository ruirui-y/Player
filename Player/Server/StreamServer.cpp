#include "StreamServer.h"

#include <WinSock2.h>
#include <WS2tcpip.h>
#include <d3d11.h>
#include <chrono>
#include <thread>
#include <cstdio>
#include <cstring>
#include <vector>

#include "Common/NetWork/VideoSender.h"
#include "Common/NetWork/AudioSender.h"
#include "Common/NetWork/NetworkStats.h"
#include "Common/Input/InputTransport.h"
#include "Common/Input/InputInjector.h"
#include "Common/Input/InputEvent.h"
#include "Common/LogManager.h"
#include "Server/BitrateController.h"
#include "Server/OBS_Capture/MonitorCapture.h"
#include "Server/OBS_Capture/IVideoEncoder.h"
#include "Server/OBS_Capture/ObsNvencEncoder.h"
#include "Server/OBS_Capture/ObsNvencEncoderFast.h"
#include "Server/AudioCapture/WasapiCapture.h"
#include "Server/AudioCapture/OpusAudioEncoder.h"

// ---- 构造 ----
StreamServer::StreamServer()
{
}

// ---- 析构：逆序释放所有资源 ----
StreamServer::~StreamServer()
{
    Stop();

    // ---- 第四阶段：先停音频组件（独立线程，需先 join）----
    if (wasapi_capture_) { wasapi_capture_->Stop(); delete wasapi_capture_; wasapi_capture_ = nullptr; }
    if (opus_encoder_)   { delete opus_encoder_;   opus_encoder_   = nullptr; }
    if (audio_sender_)   { audio_sender_->Close(); delete audio_sender_;   audio_sender_   = nullptr; }

    // ---- 第三阶段：先停输入组件 ----
    if (input_server_)  { input_server_->Stop(); delete input_server_;  input_server_  = nullptr; }
    if (bitrate_ctrl_)  { delete bitrate_ctrl_;  bitrate_ctrl_  = nullptr; }
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
                        uint16_t ctrl_port, bool use_fast, uint16_t audio_port)
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

    // ---- 第六阶段：初始化码率自适应控制器 ----
    current_bitrate_.store(bitrate_kbps);
    bitrate_ctrl_ = new BitrateController(bitrate_kbps);

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
        client_connected_.store(true);
        if (input_injector_)
            input_injector_->Inject(msg);
    };

    // 收到客户端控制消息（新签名：type + payload）
    input_server_->OnControlMessage = [this](uint8_t msg_type, const uint8_t* payload, int payload_len)
    {
        client_connected_.store(true);

        if (msg_type == 0x01)                               // RequestIDR
        {
            force_next_idr_.store(true);
            LogManager::Log("INFO", "[StreamServer] 收到 IDR 请求，下一帧将编码为 IDR");
        }
        else if (msg_type == 0x02 && payload && payload_len > 0)  // LossReport
        {
            std::string json((const char*)payload, payload_len);
            NetworkStats stats = JsonToStats(json);
            if (bitrate_ctrl_)
            {
                int new_bitrate = bitrate_ctrl_->OnStatsReport(stats);
                if (new_bitrate != current_bitrate_.load() && encoder_)
                {
                    if (encoder_->SetBitrate(new_bitrate))
                    {
                        current_bitrate_.store(new_bitrate);
                        LogManager::Log("INFO", "[StreamServer] 码率自适应: %d kbps", new_bitrate);
                    }
                }
            }
        }
        else if (msg_type == 0x03 && payload_len == 8)      // Ping → Pong
        {
            input_server_->SendToClient(0x04, payload, payload_len);
        }
    };

    // 兼容旧回调（字符串版，保留向后兼容）
    input_server_->OnControlMessageLegacy = [this](const char* msg_type)
    {
        if (std::strcmp(msg_type, "request_idr") == 0)
        {
            force_next_idr_.store(true);
            LogManager::Log("INFO", "[StreamServer] 收到 IDR 请求(legacy)，下一帧将编码为 IDR");
        }
    };

    // 客户端断开通知
    input_server_->OnClientDisconnected = [this]()
    {
        client_connected_.store(false);
        LogManager::Log("INFO", "[StreamServer] 客户端已断开");
    };

    input_server_->Start();
    LogManager::Log("INFO", "[StreamServer] 输入控制信道已启动 (port %d)", ctrl_port);

    // ---- 第九步：初始化音频组件（第四阶段：WASAPI 采集 → Opus 编码 → UDP 发送）----
    wasapi_capture_ = new WasapiCapture();
    if (!wasapi_capture_->Init(48000, 2))
    {
        LogManager::Log("WARN", "[StreamServer] WASAPI 采集器初始化失败，音频功能不可用");
        delete wasapi_capture_;
        wasapi_capture_ = nullptr;
    }
    else
    {
        opus_encoder_ = new OpusAudioEncoder();
        if (!opus_encoder_->Init(48000, 2, 128000, 20))
        {
            LogManager::Log("WARN", "[StreamServer] Opus 编码器初始化失败，音频功能不可用");
            delete opus_encoder_;
            opus_encoder_ = nullptr;
        }

        audio_sender_ = new AudioSender();
        if (!audio_sender_->Init(dest_ip, audio_port))
        {
            LogManager::Log("WARN", "[StreamServer] AudioSender 初始化失败，音频功能不可用");
            delete audio_sender_;
            audio_sender_ = nullptr;
        }

        // ---- 设置音频采集回调：PCM 缓冲 → 编码 → 发送 ----
        if (opus_encoder_ && audio_sender_)
        {
            wasapi_capture_->SetCallback(
                [this](const int16_t* pcm_data, int sample_count, uint32_t timestamp_ms)
                {
                    OnAudioData(pcm_data, sample_count, timestamp_ms);
                });
        }
    }

    LogManager::Log("INFO", "[StreamServer] 初始化完成 -> %s:%d  %dx%d@%dfps  %dbps  ctrl:%d  audio:%d",
                    dest_ip, port, width_, height_, fps_, bitrate_kbps * 1000, ctrl_port, audio_port);
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

    // ---- 启动音频采集（独立线程，不阻塞视频主循环）----
    if (wasapi_capture_)
        wasapi_capture_->Start();

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
        // 首帧或收到 IDR 请求时强制编 IDR
        bool force_idr = (frame_index == 0) || force_next_idr_.exchange(false);
        std::vector<uint8_t> h264_data;
        uint32_t timestamp = static_cast<uint32_t>(frame_index * (1000 / fps_));

        if (encoder_->EncodeFrame(tex, frame_index, force_idr, h264_data,
                                  nullptr,
                                  capture_->MonitorX(), capture_->MonitorY())
            && !h264_data.empty())
        {
            sender_->SendFrame(h264_data.data(), (int)h264_data.size(),
                               (uint16_t)(frame_index & 0xFFFF),
                               timestamp, force_idr);
        }

        ++frame_index;
        ++frame_count_;

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

    // ---- 停止音频采集（独立线程）----
    if (wasapi_capture_)
        wasapi_capture_->Stop();
}

// ================================================================
// ---- 音频回调：PCM 缓冲 → 凑够一帧 → Opus 编码 → UDP 发送 ----
// ================================================================
void StreamServer::OnAudioData(const int16_t* pcm_data, int sample_count, uint32_t timestamp_ms)
{
    if (!opus_encoder_ || !audio_sender_)
        return;

    // ---- 第一次收到数据时初始化时间戳 ----
    if (audio_frame_index_ == 0)
    {
        audio_timestamp_ms_ = timestamp_ms;
        LogManager::Log("INFO", "[StreamServer] 首次收到音频数据: %d 帧, timestamp=%u",
                        sample_count, timestamp_ms);
    }

    // ---- 每 500 帧（约 10 秒）打印一次统计 ----
    if (audio_frame_index_ > 0 && audio_frame_index_ % 500 == 0)
    {
        LogManager::Log("INFO", "[StreamServer] 音频统计: 已发送 %u 帧, buffer=%zu",
                        audio_frame_index_, audio_pcm_buffer_.size());
    }

    // ---- 第一步：追加 PCM 数据到缓冲区 ----
    // WASAPI 每次返回的采样数不固定，需要凑够 Opus 帧大小（960 = 20ms@48kHz）
    uint32_t channels = wasapi_capture_ ? wasapi_capture_->Channels() : 2;
    audio_pcm_buffer_.insert(audio_pcm_buffer_.end(),
                             pcm_data, pcm_data + sample_count * channels);

    // ---- 第二步：凑够一帧就编码发送 ----
    int frame_samples = opus_encoder_->FrameSamples();     // 960
    int frame_total = frame_samples * channels;             // 1920

    while (audio_pcm_buffer_.size() >= static_cast<size_t>(frame_total))
    {
        std::vector<uint8_t> opus_data;
        int encoded = opus_encoder_->Encode(audio_pcm_buffer_.data(),
                                            frame_samples, opus_data);
        if (encoded > 0)
        {
            audio_sender_->SendFrame(opus_data.data(), encoded,
                                     audio_frame_index_++, audio_timestamp_ms_);
        }

        // 时间戳递增（每帧 20ms）
        audio_timestamp_ms_ += opus_encoder_->FrameMs();

        // 移除已编码的数据
        audio_pcm_buffer_.erase(audio_pcm_buffer_.begin(),
                                audio_pcm_buffer_.begin() + frame_total);
    }
}
