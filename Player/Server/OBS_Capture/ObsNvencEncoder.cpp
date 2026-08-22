#include "ObsNvencEncoder.h"

#include <cstdio>
#include <cstring>
#include <QDebug>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavutil/pixfmt.h>
#include <libavutil/frame.h>
#include <libavutil/error.h>
#include <libavutil/opt.h>
#include <libavutil/version.h>
#include <libswscale/swscale.h>
}

static const char* AvErrorText(int error, char* buffer, size_t buffer_size)
{
    if (av_strerror(error, buffer, buffer_size) < 0)
    {
        std::snprintf(buffer, buffer_size, "unknown FFmpeg error");
    }
    return buffer;
}

// ========== 构造 / 析构 ==========

ObsNvencEncoder::ObsNvencEncoder()
{
}

ObsNvencEncoder::~ObsNvencEncoder()
{
    Release();
}

// ========== 创建 staging 纹理（GPU → CPU 回读） ==========

bool ObsNvencEncoder::CreateStagingTexture()
{
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = width_;
    desc.Height = height_;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    HRESULT hr = d3d_device_->CreateTexture2D(&desc, nullptr, &staging_texture_);
    if (FAILED(hr))
    {
        qDebug() << "[ObsNvenc] 创建 staging 纹理失败, HR =" << hr;
        return false;
    }

    return true;
}

// ========== 创建 BGRA→NV12 色彩转换上下文 ==========

bool ObsNvencEncoder::CreateSwsContext()
{
    // ---- NV12 布局：Y 平面 W×H，紧接 UV 交错平面 W×(H/2) ----
    nv12_linesize_ = width_;                                            // NV12 行间距 = 宽度（无对齐填充）
    nv12_data_ = static_cast<uint8_t*>(_aligned_malloc(width_ * height_ * 3 / 2, 16));
    if (!nv12_data_)
    {
        qDebug() << "[ObsNvenc] NV12 缓冲分配失败";
        return false;
    }

    // ---- sws_scale：BGRA → NV12 ----
    sws_ctx_ = sws_getContext(
        width_, height_, AV_PIX_FMT_BGRA,
        width_, height_, AV_PIX_FMT_NV12,
        SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);

    if (!sws_ctx_)
    {
        qDebug() << "[ObsNvenc] sws_getContext 失败";
        return false;
    }

    return true;
}

// ========== GPU 纹理 → CPU NV12 ==========
// 修改：增加光标合成参数，将光标绘制到 BGRA 缓冲后再做色彩转换

bool ObsNvencEncoder::GpuTextureToNv12(ID3D11Texture2D* texture,
                                       const CursorInfo* cursor,
                                       int monitor_x, int monitor_y)
{
    if (!texture || !staging_texture_ || !sws_ctx_)
    {
        return false;
    }

    // ---- 第一步：GPU 纹理 → staging 纹理（CopyResource，GPU 内部操作） ----
    d3d_ctx_->CopyResource(staging_texture_, texture);

    // ---- 第二步：Map staging 纹理 → CPU 可读 ----
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (FAILED(d3d_ctx_->Map(staging_texture_, 0, D3D11_MAP_READ, 0, &mapped)))
    {
        qDebug() << "[ObsNvenc] Map staging 失败";
        return false;
    }

    // ---- 第三步：准备 sws_scale 输入 ----
    // 没有光标时直接读取 Map 内存，避免每帧额外复制整张 BGRA 纹理。
    const bool has_cursor = cursor && cursor->visible && cursor->bitmap;
    const uint8_t* source_bgra = static_cast<const uint8_t*>(mapped.pData);
    int source_stride = static_cast<int>(mapped.RowPitch);

    // DXGI 路线不包含光标，只有需要合成时才复制到可写缓冲。
    if (has_cursor)
    {
        const int buf_stride = width_ * 4;
        if (bgra_buffer_.empty())
        {
            bgra_buffer_.resize(static_cast<size_t>(width_) * height_ * 4);
        }

        // GPU RowPitch 可能与 width*4 不同，需要逐行拷贝。
        for (int y = 0; y < height_; y++)
        {
            memcpy(bgra_buffer_.data() + y * buf_stride,
                   static_cast<const uint8_t*>(mapped.pData) + y * mapped.RowPitch,
                   buf_stride);
        }

        DrawCursorOnBuffer(bgra_buffer_.data(), width_, height_,
                           cursor, monitor_x, monitor_y);
        source_bgra = bgra_buffer_.data();
        source_stride = buf_stride;
    }

    // ---- 第五步：BGRA → NV12（sws_scale 色彩转换） ----
    const uint8_t* src_data[] = { source_bgra };
    const int src_stride[] = { source_stride };

    uint8_t* dst_data[] = {
        nv12_data_,                                                    // Y 平面起点
        nv12_data_ + nv12_linesize_ * height_                          // UV 交错平面起点
    };
    const int dst_stride[] = { nv12_linesize_, nv12_linesize_ };

    // ---- 首帧诊断：打印输入纹理格式 + staging 首像素 ----
    static bool pixel_logged = false;
    if (!pixel_logged)
    {
        D3D11_TEXTURE2D_DESC tex_desc;
        texture->GetDesc(&tex_desc);
        const char* fmt_name = "unknown";
        switch (tex_desc.Format)
        {
        case DXGI_FORMAT_B8G8R8A8_UNORM: fmt_name = "BGRA"; break;
        case DXGI_FORMAT_R8G8B8A8_UNORM: fmt_name = "RGBA"; break;
        case DXGI_FORMAT_R10G10B10A2_UNORM: fmt_name = "R10G10B10A2"; break;
        case DXGI_FORMAT_R16G16B16A16_FLOAT: fmt_name = "R16G16B16A16_FLOAT"; break;
        default: break;
        }
        const uint8_t* p = source_bgra;
        qDebug("[ObsNvenc] 输入纹理格式: %s, staging RowPitch: %u",
               fmt_name, (uint32_t)mapped.RowPitch);
        qDebug("[ObsNvenc] staging 首像素 BGRA: %02X %02X %02X %02X",
               p[0], p[1], p[2], p[3]);
        qDebug("[ObsNvenc] staging 第100行首像素: %02X %02X %02X %02X",
               p[source_stride * 100], p[source_stride * 100 + 1],
               p[source_stride * 100 + 2], p[source_stride * 100 + 3]);
    }

    sws_scale(sws_ctx_, src_data, src_stride, 0, height_,
              dst_data, const_cast<int*>(dst_stride));

    // ---- 首帧诊断：打印 NV12 首个 Y 值 ----
    if (!pixel_logged)
    {
        qDebug("[ObsNvenc] NV12 首个 Y 值: %02X %02X %02X %02X",
               nv12_data_[0], nv12_data_[1], nv12_data_[2], nv12_data_[3]);
        qDebug("[ObsNvenc] NV12 第100行 Y 值: %02X %02X %02X %02X",
               nv12_data_[nv12_linesize_ * 100],
               nv12_data_[nv12_linesize_ * 100 + 1],
               nv12_data_[nv12_linesize_ * 100 + 2],
               nv12_data_[nv12_linesize_ * 100 + 3]);
        pixel_logged = true;
    }

    d3d_ctx_->Unmap(staging_texture_, 0);

    return true;
}

// ========== 在 BGRA 缓冲上合成光标 ==========
// 将光标位图通过 Alpha 混合绘制到桌面帧的 BGRA 缓冲上
// DXGI Desktop Duplication 不包含光标，必须手动合成

void ObsNvencEncoder::DrawCursorOnBuffer(uint8_t* bgra, int buf_w, int buf_h,
                                         const CursorInfo* cursor,
                                         int monitor_x, int monitor_y)
{
    if (!bgra || !cursor || !cursor->bitmap || cursor->width == 0 || cursor->height == 0)
        return;

    // ---- 计算光标在纹理中的左上角位置 ----
    // screen_x/y 是屏幕绝对坐标，需要减去显示器偏移转为纹理局部坐标
    // 再减去热点偏移得到光标位图的左上角
    int dst_x = cursor->screen_x - monitor_x - cursor->x_hotspot;
    int dst_y = cursor->screen_y - monitor_y - cursor->y_hotspot;

    // ---- 裁剪到缓冲范围内 ----
    int src_x = 0, src_y = 0;
    if (dst_x < 0) { src_x = -dst_x; dst_x = 0; }
    if (dst_y < 0) { src_y = -dst_y; dst_y = 0; }

    int draw_w = static_cast<int>(cursor->width) - src_x;
    int draw_h = static_cast<int>(cursor->height) - src_y;

    if (dst_x + draw_w > buf_w) draw_w = buf_w - dst_x;
    if (dst_y + draw_h > buf_h) draw_h = buf_h - dst_y;

    if (draw_w <= 0 || draw_h <= 0)
        return;

    int buf_stride = buf_w * 4;
    int cur_stride = static_cast<int>(cursor->width) * 4;

    // ---- 逐像素 Alpha 混合 ----
    for (int y = 0; y < draw_h; y++)
    {
        uint8_t* dst = bgra + (dst_y + y) * buf_stride + dst_x * 4;
        const uint8_t* src = cursor->bitmap + (src_y + y) * cur_stride + src_x * 4;

        for (int x = 0; x < draw_w; x++)
        {
            uint8_t alpha = src[3];
            if (alpha == 0)
            {
                dst += 4;
                src += 4;
                continue;
            }

            if (alpha == 255)
            {
                // 完全不透明 → 直接覆盖
                dst[0] = src[0];
                dst[1] = src[1];
                dst[2] = src[2];
                dst[3] = 255;
            }
            else
            {
                // 半透明 → Alpha 混合
                dst[0] = static_cast<uint8_t>((src[0] * alpha + dst[0] * (255 - alpha)) / 255);
                dst[1] = static_cast<uint8_t>((src[1] * alpha + dst[1] * (255 - alpha)) / 255);
                dst[2] = static_cast<uint8_t>((src[2] * alpha + dst[2] * (255 - alpha)) / 255);
            }

            dst += 4;
            src += 4;
        }
    }
}

// ========== 初始化编码器 ==========

bool ObsNvencEncoder::Init(ID3D11Device* d3d_device, int width, int height,
                           int fps, int bitrate_kbps)
{
    Release();

    if (!d3d_device || width <= 0 || height <= 0 || fps <= 0)
    {
        return false;
    }

    d3d_device_ = d3d_device;
    d3d_device_->AddRef();
    d3d_device_->GetImmediateContext(&d3d_ctx_);
    width_ = width;
    height_ = height;
    fps_ = fps;
    bitrate_kbps_ = bitrate_kbps;

    // ---- 第一步：查找 NVENC 编码器 ----
    codec_ = avcodec_find_encoder_by_name("h264_nvenc");
    if (!codec_)
    {
        codec_ = avcodec_find_encoder_by_name("libx264");
        if (!codec_)
        {
            qDebug() << "[ObsNvenc] 找不到 H.264 编码器";
            return false;
        }
        qDebug() << "[ObsNvenc] h264_nvenc 不可用，回退到软件编码器:" << codec_->name;
    }
    else
    {
        qDebug() << "[ObsNvenc] 使用 NVENC 编码器";
    }

    qDebug("[ObsNvenc] FFmpeg libavcodec=%u, 配置: %s",
           avcodec_version(), avcodec_configuration());

    // ---- 第二步：创建编码上下文 ----
    codec_ctx_ = avcodec_alloc_context3(codec_);
    if (!codec_ctx_)
    {
        qDebug() << "[ObsNvenc] avcodec_alloc_context3 失败";
        return false;
    }

    auto configure_codec_context = [this](AVCodecContext* context, const AVCodec* codec)
    {
        context->width = width_;
        context->height = height_;
        context->time_base = { 1, fps_ };                              // PTS 时间基
        context->framerate = { fps_, 1 };
        context->pix_fmt = AV_PIX_FMT_NV12;
        context->bit_rate = static_cast<int64_t>(bitrate_kbps_) * 1000;
        context->gop_size = fps_ * 2;
        context->max_b_frames = 0;
        // NVENC 本身不需要 FFmpeg 多线程；libx264 使用自动线程数。
        context->thread_count = strcmp(codec->name, "libx264") == 0 ? 0 : 1;
        context->flags |= AV_CODEC_FLAG_LOW_DELAY;

        if (strcmp(codec->name, "h264_nvenc") == 0)
        {
            const int preset_ret = av_opt_set(context->priv_data, "preset", "p1", 0);
            const int tune_ret = av_opt_set(context->priv_data, "tune", "ll", 0);
            const int rc_ret = av_opt_set(context->priv_data, "rc", "cbr", 0);
            const int idr_ret = av_opt_set(context->priv_data, "forced-idr", "1", 0);

            qDebug("[ObsNvenc] NVENC 参数结果: preset=%d tune=%d rc=%d forced-idr=%d",
                   preset_ret, tune_ret, rc_ret, idr_ret);
        }
        else if (strcmp(codec->name, "libx264") == 0)
        {
            // 低延迟软件编码使用切片线程，避免 x264 因帧间 lookahead 产生额外排队。
            // 这些参数通过 x264-params 传入；部分 FFmpeg 构建不暴露同名独立 AVOption。
            context->thread_type = FF_THREAD_SLICE;
            context->flags2 |= AV_CODEC_FLAG2_FAST;

            const int preset_ret = av_opt_set(context->priv_data, "preset", "ultrafast", 0);
            const int tune_ret = av_opt_set(context->priv_data, "tune", "zerolatency", 0);
            const int x264_params_ret = av_opt_set(
                context->priv_data,
                "x264-params",
                "rc-lookahead=0:sync-lookahead=0:sliced-threads=1:repeat-headers=1",
                0);

            qDebug("[ObsNvenc] libx264 参数结果: preset=%d tune=%d x264-params=%d "
                   "threads=%d thread_type=slice",
                   preset_ret, tune_ret, x264_params_ret,
                   context->thread_count);
        }
    };

    // ---- 第三步：设置编码参数 ----
    configure_codec_context(codec_ctx_, codec_);

    // ---- 第四步：打开编码器 ----
    int open_ret = avcodec_open2(codec_ctx_, codec_, nullptr);
    if (open_ret < 0 && strcmp(codec_->name, "h264_nvenc") == 0)
    {
        char error_text[AV_ERROR_MAX_STRING_SIZE] = {};
        qDebug("[ObsNvenc] h264_nvenc avcodec_open2 失败: ret=%d (%s)",
               open_ret, AvErrorText(open_ret, error_text, sizeof(error_text)));

        // NVENC 失败时回退到软件 H.264，避免采集服务因硬件编码器不可用而无法启动。
        avcodec_free_context(&codec_ctx_);
        codec_ = avcodec_find_encoder_by_name("libx264");
        if (!codec_)
        {
            qDebug() << "[ObsNvenc] NVENC 失败且找不到 libx264，编码器初始化终止";
            return false;
        }

        codec_ctx_ = avcodec_alloc_context3(codec_);
        if (!codec_ctx_)
        {
            qDebug() << "[ObsNvenc] 回退 libx264 时 avcodec_alloc_context3 失败";
            return false;
        }

        configure_codec_context(codec_ctx_, codec_);
        open_ret = avcodec_open2(codec_ctx_, codec_, nullptr);
        if (open_ret == 0)
        {
            qDebug("[ObsNvenc] 已回退到软件编码器: %s", codec_->name);
        }
    }

    if (open_ret < 0)
    {
        char error_text[AV_ERROR_MAX_STRING_SIZE] = {};
        qDebug("[ObsNvenc] avcodec_open2 失败: codec=%s ret=%d (%s)",
               codec_ ? codec_->name : "null",
               open_ret, AvErrorText(open_ret, error_text, sizeof(error_text)));
        return false;
    }

    // ---- 第五步：创建 AVFrame 和 AVPacket ----
    frame_ = av_frame_alloc();
    frame_->format = AV_PIX_FMT_NV12;
    frame_->width = width_;
    frame_->height = height_;
    frame_->pts = 0;

    // ---- 设置 AVFrame 数据指针指向 NV12 缓冲 ----
    frame_->data[0] = nullptr;                                          // 编码前由 GpuTextureToNv12 填充
    frame_->data[1] = nullptr;
    frame_->linesize[0] = nv12_linesize_ ? nv12_linesize_ : width_;
    frame_->linesize[1] = nv12_linesize_ ? nv12_linesize_ : width_;

    packet_ = av_packet_alloc();

    // ---- 第六步：创建 staging 纹理和 sws 上下文 ----
    if (!CreateStagingTexture())
    {
        return false;
    }

    if (!CreateSwsContext())
    {
        return false;
    }

    // ---- 更新 frame_ 的 linesize（CreateSwsContext 中设置了 nv12_linesize_） ----
    frame_->linesize[0] = nv12_linesize_;
    frame_->linesize[1] = nv12_linesize_;

    qDebug() << "[ObsNvenc] 初始化成功:"
             << width_ << "x" << height_ << "@" << fps_ << "fps"
             << "码率:" << bitrate_kbps_ << "kbps"
             << "编码器:" << codec_->name
             << "GOP:" << codec_ctx_->gop_size
             << "B帧:" << codec_ctx_->max_b_frames;

    return true;
}

// ========== 编码一帧 ==========

bool ObsNvencEncoder::EncodeFrame(ID3D11Texture2D* texture, uint64_t frame_index,
                                  bool force_idr, std::vector<uint8_t>& out_data,
                                  const CursorInfo* cursor,
                                  int monitor_x, int monitor_y)
{
    out_data.clear();

    if (!codec_ctx_ || !texture)
    {
        return false;
    }

    // ---- 第一步：GPU 纹理 → CPU NV12（含光标合成） ----
    if (!GpuTextureToNv12(texture, cursor, monitor_x, monitor_y))
    {
        return false;
    }

    // ---- 第二步：填充 AVFrame ----
    frame_->data[0] = nv12_data_;                                      // Y 平面
    frame_->data[1] = nv12_data_ + nv12_linesize_ * height_;           // UV 交错平面
    frame_->pts = static_cast<int64_t>(frame_index);

    // ---- 强制 IDR 帧 ----
    if (force_idr)
    {
        frame_->pict_type = AV_PICTURE_TYPE_I;
    }
    else
    {
        frame_->pict_type = AV_PICTURE_TYPE_NONE;
    }

    // ---- 第三步：发送帧到编码器（类比 FFmpeg nvenc_send_frame） ----
    int ret = avcodec_send_frame(codec_ctx_, frame_);
    if (ret < 0)
    {
        qDebug() << "[ObsNvenc] avcodec_send_frame 失败, ret =" << ret;
        return false;
    }

    // ---- 第四步：接收编码输出（类比 FFmpeg nvenc_receive_packet） ----
    // NVENC 无 B 帧时，send 一帧通常立即能 receive 到一帧
    // 有 B 帧时，前几帧会延迟输出（本实现 max_b_frames=0，不会延迟）
    ret = avcodec_receive_packet(codec_ctx_, packet_);
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
    {
        // 编码器需要更多帧才有输出 → 正常
        return false;
    }
    if (ret < 0)
    {
        qDebug() << "[ObsNvenc] avcodec_receive_packet 失败, ret =" << ret;
        return false;
    }

    // ---- 第五步：拷贝码流到输出 ----
    out_data.assign(packet_->data, packet_->data + packet_->size);
    av_packet_unref(packet_);

    return true;
}

// ========== Flush 剩余帧 ==========

void ObsNvencEncoder::Flush(std::function<void(const std::vector<uint8_t>&)> on_packet)
{
    if (!codec_ctx_)
    {
        return;
    }

    // ---- 发送 flush 信号（nullptr = flush） ----
    avcodec_send_frame(codec_ctx_, nullptr);

    // ---- 取出所有剩余编码输出 ----
    while (true)
    {
        int ret = avcodec_receive_packet(codec_ctx_, packet_);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
        {
            break;
        }
        if (ret < 0)
        {
            break;
        }

        std::vector<uint8_t> data(packet_->data, packet_->data + packet_->size);
        on_packet(data);
        av_packet_unref(packet_);
    }
}

// ========== 释放所有资源 ==========

void ObsNvencEncoder::Release()
{
    if (packet_)
    {
        av_packet_free(&packet_);
    }

    if (frame_)
    {
        av_frame_free(&frame_);
    }

    if (codec_ctx_)
    {
        avcodec_free_context(&codec_ctx_);
    }

    if (sws_ctx_)
    {
        sws_freeContext(sws_ctx_);
        sws_ctx_ = nullptr;
    }

    if (nv12_data_)
    {
        _aligned_free(nv12_data_);
        nv12_data_ = nullptr;
    }

    // 释放光标合成缓冲
    bgra_buffer_.clear();
    bgra_buffer_.shrink_to_fit();

    if (staging_texture_)
    {
        staging_texture_->Release();
        staging_texture_ = nullptr;
    }

    if (d3d_ctx_)
    {
        d3d_ctx_->Release();
        d3d_ctx_ = nullptr;
    }

    if (d3d_device_)
    {
        d3d_device_->Release();
        d3d_device_ = nullptr;
    }

    codec_ = nullptr;
    width_ = 0;
    height_ = 0;
    nv12_linesize_ = 0;
}

// ---- 运行时调整码率 ----
bool ObsNvencEncoder::SetBitrate(int bitrate_kbps)
{
    if (!codec_ctx_ || bitrate_kbps <= 0)
        return false;

    if (bitrate_kbps == bitrate_kbps_)
        return true;

    bitrate_kbps_ = bitrate_kbps;
    codec_ctx_->bit_rate = static_cast<int64_t>(bitrate_kbps_) * 1000;

    qDebug() << "[ObsNvencEncoder] 码率已调整为" << bitrate_kbps_ << "kbps";
    return true;
}
