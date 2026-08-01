#include "WasapiCapture.h"

#include <QDebug>
#include <chrono>
#include <algorithm>

#include "Common/LogManager.h"

extern "C"
{
#include <libswresample/swresample.h>
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>
}

#pragma comment(lib, "ole32.lib")

// 构造
WasapiCapture::WasapiCapture()
{
}

// 析构：逆序释放所有资源
WasapiCapture::~WasapiCapture()
{
    Stop();

    if (capture_client_) { capture_client_->Release(); capture_client_ = nullptr; }
    if (render_client_)  { render_client_->Stop(); render_client_->Release(); render_client_ = nullptr; }
    if (audio_client_)   { audio_client_->Release();   audio_client_   = nullptr; }
    if (device_)         { device_->Release();         device_         = nullptr; }
    if (enumerator_)     { enumerator_->Release();     enumerator_     = nullptr; }
    if (pwfex_)          { CoTaskMemFree(pwfex_);      pwfex_          = nullptr; }

    if (swr_ctx_)
    {
        swr_free(&swr_ctx_);
        swr_ctx_ = nullptr;
    }

    if (com_initialized_)
    {
        CoUninitialize();
        com_initialized_ = false;
    }
}

// ================================================================
// ---- 初始化：COM → 设备枚举 → 获取默认设备 → 激活 AudioClient → 环回模式 → 重采样器 ----
// ================================================================
bool WasapiCapture::Init(uint32_t output_sample_rate, uint32_t channels)
{
    output_sample_rate_ = output_sample_rate;
    channels_ = channels;

    // ---- 第一步：初始化 COM（MTA 模式，允许跨线程使用 COM 对象）----
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (hr == RPC_E_CHANGED_MODE)
    {
        // COM 已被其他线程以不同模式初始化，不影响 WASAPI 使用
        com_initialized_ = false;
    }
    else if (FAILED(hr))
    {
        LogManager::Log("ERR", "[WasapiCapture] CoInitializeEx 失败, HR = 0x%08X", (unsigned)hr);
        return false;
    }
    else
    {
        com_initialized_ = true;
    }

    // ---- 第二步：创建设备枚举器 ----
    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                          __uuidof(IMMDeviceEnumerator), (void**)&enumerator_);
    if (FAILED(hr))
    {
        LogManager::Log("ERR", "[WasapiCapture] 创建设备枚举器失败, HR = 0x%08X", (unsigned)hr);
        return false;
    }

    // ---- 第三步：获取默认音频渲染设备（扬声器）----
    hr = enumerator_->GetDefaultAudioEndpoint(eRender, eConsole, &device_);
    if (FAILED(hr))
    {
        LogManager::Log("ERR", "[WasapiCapture] 获取默认音频设备失败, HR = 0x%08X", (unsigned)hr);
        return false;
    }

    // ---- 第四步：激活 IAudioClient ----
    hr = device_->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&audio_client_);
    if (FAILED(hr))
    {
        LogManager::Log("ERR", "[WasapiCapture] 激活 IAudioClient 失败, HR = 0x%08X", (unsigned)hr);
        return false;
    }

    // ---- 第五步：获取设备混音格式（loopback 模式必须使用 mix format）----
    hr = audio_client_->GetMixFormat(&pwfex_);
    if (FAILED(hr))
    {
        LogManager::Log("ERR", "[WasapiCapture] GetMixFormat 失败, HR = 0x%08X", (unsigned)hr);
        return false;
    }

    device_sample_rate_ = pwfex_->nSamplesPerSec;

    LogManager::Log("INFO", "[WasapiCapture] 设备格式: %dHz %dch %d-bit wFormatTag=%d",
                    pwfex_->nSamplesPerSec, pwfex_->nChannels, pwfex_->wBitsPerSample, pwfex_->wFormatTag);
    LogManager::Log("INFO", "[WasapiCapture] 输出格式: %dHz %dch int16 (重采样目标)",
                    output_sample_rate_, channels_);

    // ---- 第六步：初始化 AudioClient（环回模式）----
    // AUDCLNT_STREAMFLAGS_LOOPBACK：捕获系统播放的音频，不需要打开麦克风
    // 共享模式 + 环回 = 零延迟获取系统声音
    hr = audio_client_->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                   AUDCLNT_STREAMFLAGS_LOOPBACK,
                                   0, 0, pwfex_, nullptr);
    if (FAILED(hr))
    {
        LogManager::Log("ERR", "[WasapiCapture] Initialize 失败, HR = 0x%08X", (unsigned)hr);
        return false;
    }

    // ---- 第七步：获取采集客户端 ----
    hr = audio_client_->GetService(__uuidof(IAudioCaptureClient), (void**)&capture_client_);
    if (FAILED(hr))
    {
        LogManager::Log("ERR", "[WasapiCapture] GetService IAudioCaptureClient 失败, HR = 0x%08X", (unsigned)hr);
        return false;
    }

    // ---- 第八步：启动 AudioClient ----
    hr = audio_client_->Start();
    if (FAILED(hr))
    {
        LogManager::Log("ERR", "[WasapiCapture] Start 失败, HR = 0x%08X", (unsigned)hr);
        return false;
    }

    // ---- 第九步：创建辅助渲染流（保持音频引擎活跃）----
    // WASAPI loopback 的陷阱：如果没有渲染流在播放，GetNextPacketSize 永远返回 0
    // 自己开一个静音渲染流，让音频引擎持续运行，loopback 就能收到数据包
    hr = device_->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&render_client_);
    if (SUCCEEDED(hr))
    {
        hr = render_client_->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                        0,              // 注意：不加 LOOPBACK，这是正常渲染流
                                        0, 0, pwfex_, nullptr);
        if (SUCCEEDED(hr))
        {
            hr = render_client_->Start();
            if (SUCCEEDED(hr))
            {
                LogManager::Log("INFO", "[WasapiCapture] 辅助渲染流已启动");
            }
            else
            {
                LogManager::Log("WARN", "[WasapiCapture] 辅助渲染流 Start 失败, HR = 0x%08X", (unsigned)hr);
                render_client_->Release();
                render_client_ = nullptr;
            }
        }
        else
        {
            LogManager::Log("WARN", "[WasapiCapture] 辅助渲染流 Initialize 失败, HR = 0x%08X", (unsigned)hr);
            render_client_->Release();
            render_client_ = nullptr;
        }
    }
    else
    {
        LogManager::Log("WARN", "[WasapiCapture] 辅助渲染流 Activate 失败, HR = 0x%08X", (unsigned)hr);
    }

    // ---- 第十步：创建 SwrContext 重采样器 ----
    // 将 WASAPI 的 float32@device_rate 转换为 int16@output_rate
    // Opus 只支持 8/12/16/24/48kHz，设备可能是 44100Hz，必须重采样
    AVChannelLayout src_layout = AV_CHANNEL_LAYOUT_STEREO;
    AVChannelLayout dst_layout = AV_CHANNEL_LAYOUT_STEREO;

    int ret = swr_alloc_set_opts2(&swr_ctx_,
        &dst_layout, AV_SAMPLE_FMT_S16, output_sample_rate_,           // 目标：int16 @ 48000Hz
        &src_layout, AV_SAMPLE_FMT_FLT, device_sample_rate_,           // 源：float32 @ 设备采样率
        0, nullptr);
    if (ret < 0 || !swr_ctx_)
    {
        LogManager::Log("ERR", "[WasapiCapture] swr_alloc_set_opts2 失败, ret = %d", ret);
        return false;
    }

    ret = swr_init(swr_ctx_);
    if (ret < 0)
    {
        LogManager::Log("ERR", "[WasapiCapture] swr_init 失败, ret = %d", ret);
        swr_free(&swr_ctx_);
        swr_ctx_ = nullptr;
        return false;
    }

    LogManager::Log("INFO", "[WasapiCapture] 重采样器就绪: %dHz float32 → %dHz int16",
                    device_sample_rate_, output_sample_rate_);

    LogManager::Log("INFO", "[WasapiCapture] 初始化成功");
    return true;
}

// 设置音频回调
void WasapiCapture::SetCallback(AudioCallback cb)
{
    callback_ = std::move(cb);
}

// 启动采集线程
void WasapiCapture::Start()
{
    if (running_)
        return;

    running_ = true;
    capture_thread_ = std::thread(&WasapiCapture::CaptureLoop, this);
}

// 停止采集线程
void WasapiCapture::Stop()
{
    if (!running_)
        return;

    running_ = false;

    if (capture_thread_.joinable())
        capture_thread_.join();

    // ---- 停止采集客户端和辅助渲染流 ----
    if (audio_client_)
        audio_client_->Stop();
    if (render_client_)
        render_client_->Stop();
}

// ================================================================
// ---- 采集线程主循环：GetBuffer → SwrContext 重采样 → 回调投递 ----
// ================================================================
void WasapiCapture::CaptureLoop()
{
    // ---- 采集线程也需要初始化 COM（MTA 模式）----
    HRESULT hr_com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    LogManager::Log("INFO", "[WasapiCapture] 采集线程 COM 初始化: hr=0x%08X (%s)",
                    (unsigned)hr_com,
                    (hr_com == RPC_E_CHANGED_MODE) ? "已被其他线程初始化" :
                    (SUCCEEDED(hr_com)) ? "成功" : "失败");

    LogManager::Log("INFO", "[WasapiCapture] 采集线程启动");

    // ---- 统计计数 ----
    uint64_t total_frames = 0;
    uint64_t callback_count = 0;
    uint64_t silent_count = 0;
    uint64_t empty_packet_count = 0;
    uint64_t get_buffer_fail_count = 0;
    uint64_t non_zero_count = 0;

    while (running_)
    {
        // ---- 第一步：获取缓冲区中是否有数据 ----
        UINT32 packet_length = 0;
        HRESULT hr = capture_client_->GetNextPacketSize(&packet_length);
        if (FAILED(hr))
        {
            LogManager::Log("ERR", "[WasapiCapture] GetNextPacketSize 失败: hr=0x%08X", (unsigned)hr);
            break;
        }

        // 没有数据 → 短暂休眠后重试
        if (packet_length == 0)
        {
            empty_packet_count++;
            //if (empty_packet_count % 1000 == 1)
            //{
            //    LogManager::Log("INFO", "[WasapiCapture] 等待数据... 空包计数=%llu",
            //                    (unsigned long long)empty_packet_count);
            //}
            Sleep(1);
            continue;
        }

        // ---- 第二步：循环处理所有可用包 ----
        while (packet_length != 0 && running_)
        {
            BYTE* data = nullptr;
            UINT32 num_frames = 0;
            DWORD flags = 0;

            hr = capture_client_->GetBuffer(&data, &num_frames, &flags, nullptr, nullptr);
            if (FAILED(hr))
            {
                get_buffer_fail_count++;
                LogManager::Log("ERR", "[WasapiCapture] GetBuffer 失败: hr=0x%08X, 失败次数=%llu",
                                (unsigned)hr, (unsigned long long)get_buffer_fail_count);
                break;
            }

            // ---- 静音包跳过（但仍需 ReleaseBuffer）----
            if (flags & AUDCLNT_BUFFERFLAGS_SILENT)
            {
                silent_count++;
                capture_client_->ReleaseBuffer(num_frames);
                hr = capture_client_->GetNextPacketSize(&packet_length);
                if (FAILED(hr)) break;
                continue;
            }

            // ---- 第三步：使用 SwrContext 重采样 ----
            // 输入：float32 @ device_sample_rate_（WASAPI loopback 原始格式）
            // 输出：int16 @ output_sample_rate_（Opus 编码器需要的格式）
            int max_out_samples = swr_get_out_samples(swr_ctx_, num_frames);
            if (max_out_samples <= 0)
                max_out_samples = num_frames + 256;         // 保守估计

            // 输出缓冲区：每声道 max_out_samples × 2 字节(int16) × channels_
            std::vector<int16_t> pcm_buffer(max_out_samples * channels_);
            uint8_t* dst[] = { reinterpret_cast<uint8_t*>(pcm_buffer.data()) };
            const uint8_t* src[] = { data };

            int out_samples = swr_convert(swr_ctx_,
                                          dst, max_out_samples,
                                          src, num_frames);
            if (out_samples < 0)
            {
                LogManager::Log("ERR", "[WasapiCapture] swr_convert 失败: %d", out_samples);
                capture_client_->ReleaseBuffer(num_frames);
                hr = capture_client_->GetNextPacketSize(&packet_length);
                if (FAILED(hr)) break;
                continue;
            }

            // 调整缓冲区到实际大小
            pcm_buffer.resize(out_samples * channels_);

            // ---- 第四步：时间戳（毫秒）----
            uint32_t timestamp_ms = static_cast<uint32_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count() & 0xFFFFFFFF);

            // ---- 第五步：回调投递重采样后的 PCM 数据 ----
            if (callback_ && out_samples > 0)
            {
                callback_(pcm_buffer.data(), out_samples, timestamp_ms);
                callback_count++;

                // ---- 前 5 次回调逐次打印 ----
                //if (callback_count <= 5)
                //{
                //    bool all_zero = true;
                //    for (int i = 0; i < out_samples * (int)channels_ && i < 100; ++i)
                //    {
                //        if (pcm_buffer[i] != 0) { all_zero = false; break; }
                //    }
                //    if (!all_zero) non_zero_count++;

                //    LogManager::Log("INFO", "[WasapiCapture] 回调 #%llu: in_frames=%u, out_samples=%d, ts=%u, %s",
                //                    (unsigned long long)callback_count, num_frames, out_samples,
                //                    timestamp_ms, all_zero ? "全零" : "有数据");
                //}
            }

            // ---- 每 500 次回调打印一次统计 ----
            //if (callback_count % 500 == 0 && callback_count > 0)
            //{
            //    LogManager::Log("INFO", "[WasapiCapture] 统计: callback=%llu, total_frames=%llu, silent=%llu, non_zero=%llu",
            //                    (unsigned long long)callback_count, (unsigned long long)total_frames,
            //                    (unsigned long long)silent_count, (unsigned long long)non_zero_count);
            //}

            total_frames += num_frames;

            // ---- 释放缓冲区 ----
            capture_client_->ReleaseBuffer(num_frames);

            // 取下一个包
            hr = capture_client_->GetNextPacketSize(&packet_length);
            if (FAILED(hr)) break;
        }
    }

    // ---- 线程退出时打印汇总 ----
    LogManager::Log("INFO", "[WasapiCapture] 采集线程退出: callback=%llu, total_frames=%llu, silent=%llu, empty=%llu, fail=%llu, non_zero=%llu",
                    (unsigned long long)callback_count, (unsigned long long)total_frames,
                    (unsigned long long)silent_count, (unsigned long long)empty_packet_count,
                    (unsigned long long)get_buffer_fail_count, (unsigned long long)non_zero_count);

    CoUninitialize();
}
