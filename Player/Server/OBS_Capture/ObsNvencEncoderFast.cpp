#include "ObsNvencEncoderFast.h"
#include <cstring>
#include <QDebug>
#include <d3d11_3.h>   // ID3D11VideoContext1 + DXGI_COLOR_SPACE_TYPE（VideoContext1 精确色彩空间设置需要）

extern "C" {
#include <libavutil/opt.h>
}

// ========== 构造 / 析构 ==========
ObsNvencEncoderFast::ObsNvencEncoderFast() {}

ObsNvencEncoderFast::~ObsNvencEncoderFast() {
    Release();
}

// ========== 初始化 ==========
bool ObsNvencEncoderFast::Init(ID3D11Device* d3d_device, int width, int height, int fps, int bitrate_kbps) {
    Release();

    if (!d3d_device || width <= 0 || height <= 0) return false;

    // 1. 保存基础资源
    d3d_device_ = d3d_device;
    d3d_device_->AddRef();
    d3d_device_->GetImmediateContext(&d3d_ctx_);

    width_ = width;
    height_ = height;
    fps_ = fps;
    bitrate_kbps_ = bitrate_kbps;

    // 2. 初始化视频处理器
    if (!InitVideoProcessor()) {
        qDebug() << "[FastEncoder] 初始化 Video Processor 失败";
        return false;
    }

    // 3. 创建 NV12 目标纹理
    // 用于接收 VideoProcessor 的输出结果 (GPU 内部使用)
    D3D11_TEXTURE2D_DESC desc_rt = {};
    desc_rt.Width = width_;
    desc_rt.Height = height_;
    desc_rt.MipLevels = 1;
    desc_rt.ArraySize = 1;
    desc_rt.Format = DXGI_FORMAT_NV12;
    desc_rt.SampleDesc.Count = 1;
    desc_rt.Usage = D3D11_USAGE_DEFAULT;
    desc_rt.BindFlags = D3D11_BIND_RENDER_TARGET; // VideoProcessor 必须要 RenderTarget

    if (FAILED(d3d_device_->CreateTexture2D(&desc_rt, nullptr, &nv12_texture_))) {
        qDebug() << "[FastEncoder] 创建 NV12 RenderTarget 失败";
        return false;
    }

    // 4. 创建 NV12 Staging 纹理
    // 用于将 GPU 数据转给 CPU (供 FFmpeg 读取)
    D3D11_TEXTURE2D_DESC desc_staging = {};
    desc_staging.Width = width_;
    desc_staging.Height = height_;
    desc_staging.MipLevels = 1;
    desc_staging.ArraySize = 1;
    desc_staging.Format = DXGI_FORMAT_NV12;
    desc_staging.SampleDesc.Count = 1;
    desc_staging.Usage = D3D11_USAGE_STAGING;
    desc_staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc_staging.BindFlags = 0;

    if (FAILED(d3d_device_->CreateTexture2D(&desc_staging, nullptr, &nv12_staging_))) {
        qDebug() << "[FastEncoder] 创建 NV12 Staging 失败";
        return false;
    }

    // 5. 初始化 FFmpeg NVENC
    codec_ = avcodec_find_encoder_by_name("h264_nvenc");
    if (!codec_) {
        qDebug() << "[FastEncoder] 未找到 NVENC 编码器";
        return false;
    }

    codec_ctx_ = avcodec_alloc_context3(codec_);
    if (!codec_ctx_) return false;

    codec_ctx_->width = width_;
    codec_ctx_->height = height_;
    codec_ctx_->time_base = { 1, fps_ };
    codec_ctx_->framerate = { fps_, 1 };
    codec_ctx_->pix_fmt = AV_PIX_FMT_NV12;
    codec_ctx_->bit_rate = bitrate_kbps_ * 1000;
    codec_ctx_->gop_size = fps_ * 2;
    codec_ctx_->max_b_frames = 0;
    codec_ctx_->thread_count = 1;

    // 设置 NVENC 参数
    av_opt_set(codec_ctx_->priv_data, "preset", "p1", 0);   // 最快预设
    av_opt_set(codec_ctx_->priv_data, "tune", "ll", 0);     // 低延迟
    av_opt_set(codec_ctx_->priv_data, "rc", "cbr", 0);      // 恒定码率
    av_opt_set(codec_ctx_->priv_data, "forced-idr", "1", 0);

    // 设置 VUI 色彩参数 —— 让解码器知道这是 full range BT.709
    // 不设置的话解码器收到 "unspecified"，默认按 limited range 处理 → 二次偏色
    codec_ctx_->color_range = AVCOL_RANGE_JPEG;     // full range (0-255)
    codec_ctx_->colorspace = AVCOL_SPC_BT709;       // BT.709 色彩矩阵
    codec_ctx_->color_primaries = AVCOL_PRI_BT709;  // BT.709 原色
    codec_ctx_->color_trc = AVCOL_TRC_BT709;        // BT.709 传输特性

    if (avcodec_open2(codec_ctx_, codec_, nullptr) < 0) {
        qDebug() << "[FastEncoder] 打开编码器失败";
        return false;
    }

    frame_ = av_frame_alloc();
    frame_->format = AV_PIX_FMT_NV12;
    frame_->width = width_;
    frame_->height = height_;
    // 注意：这里不调用 av_frame_get_buffer，因为我们手动管理 data 指针

    packet_ = av_packet_alloc();

    qDebug() << "[FastEncoder] 初始化成功 (D3D11 硬件转换)";
    return true;
}

// ========== 初始化视频处理器 ==========
bool ObsNvencEncoderFast::InitVideoProcessor() {
    // 1. 获取 Video Device 和 Context 接口
    if (FAILED(d3d_device_->QueryInterface(__uuidof(ID3D11VideoDevice), (void**)&video_device_))) {
        return false;
    }
    if (FAILED(d3d_ctx_->QueryInterface(__uuidof(ID3D11VideoContext), (void**)&video_ctx_))) {
        return false;
    }

    // 2. 创建 Video Processor Enumerator
    D3D11_VIDEO_PROCESSOR_CONTENT_DESC content_desc = {};

    // 【修正 1】InputFrameFormat 是视频帧类型（渐进/隔行），不是像素格式
    // 桌面采集通常是渐进式
    content_desc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;

    // 【修正 2】正确的成员名称是 InputWidth/Height，不是 InputFrameWidth...
    content_desc.InputWidth = width_;
    content_desc.InputHeight = height_;

    content_desc.OutputWidth = width_;
    content_desc.OutputHeight = height_;

    // 帧率可选填，默认0即可
    content_desc.InputFrameRate.Numerator = fps_;
    content_desc.InputFrameRate.Denominator = 1;
    content_desc.OutputFrameRate.Numerator = fps_;
    content_desc.OutputFrameRate.Denominator = 1;

    content_desc.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;

    // 创建枚举器
    if (FAILED(video_device_->CreateVideoProcessorEnumerator(&content_desc, &video_proc_enum_))) {
        return false;
    }

    // 3. 【修正 3】检查格式支持
    // 注意：CheckVideoProcessorFormatConversion 不存在，改为检查输入和输出格式支持位
    UINT flags = 0;

    // 检查输入格式 BGRA
    // 注意：CheckVideoProcessorFormat 是枚举器的方法，不是设备的方法
    if (FAILED(video_proc_enum_->CheckVideoProcessorFormat(DXGI_FORMAT_B8G8R8A8_UNORM, &flags))) {
        return false;
    }
    if ((flags & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT) == 0) {
        qDebug() << "[FastEncoder] 显卡不支持 BGRA 作为 VideoProcessor 输入";
        return false;
    }

    // 检查输出格式 NV12
    if (FAILED(video_proc_enum_->CheckVideoProcessorFormat(DXGI_FORMAT_NV12, &flags))) {
        return false;
    }
    if ((flags & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT) == 0) {
        qDebug() << "[FastEncoder] 显卡不支持 NV12 作为 VideoProcessor 输出";
        return false;
    }

    // 4. 创建 Video Processor 实例
    if (FAILED(video_device_->CreateVideoProcessor(video_proc_enum_, 0, &video_processor_))) {
        return false;
    }

    // 5. 设置色彩空间 —— 修复发青的关键步骤
    // 不设置时驱动用 undefined 默认值，RGB→YUV 矩阵和 range 都不确定
    // UV 通道产生直流偏移 → 画面整体发青
    // 优先用 VideoContext1 精确指定 DXGI_COLOR_SPACE_TYPE
    ID3D11VideoContext1* video_ctx1 = nullptr;
    if (SUCCEEDED(video_ctx_->QueryInterface(__uuidof(ID3D11VideoContext1), (void**)&video_ctx1)))
    {
        // 输入：BGRA = RGB full range (0-255), gamma 2.2, BT.709
        // 注意：方法名是 StreamColorSpace1（按流设置），不是 InputColorSpace1（不存在）
        video_ctx1->VideoProcessorSetStreamColorSpace1(
            video_processor_, 0, DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);

        // 输出：NV12 = YCbCr full range (0-255), gamma 2.2, BT.709, 4:2:0 left chroma
        video_ctx1->VideoProcessorSetOutputColorSpace1(
            video_processor_, DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P709);

        video_ctx1->Release();
        qDebug() << "[FastEncoder] 色彩空间已设置 (VideoContext1): RGB full -> YCbCr BT.709 full";
    }
    else
    {
        // ---- 降级：老驱动不支持 VideoContext1，用基础 API ----
        // ID3D11VideoContext 设置输入色彩空间的方法是 VideoProcessorSetStreamColorSpace（按流设置）
        D3D11_VIDEO_PROCESSOR_COLOR_SPACE input_cs = {};
        input_cs.Usage = 0;           // playback
        input_cs.RGB_Range = 0;       // full range (0-255)
        input_cs.YCbCr_Matrix = 1;    // BT.709
        input_cs.YCbCr_xvYCC = 0;
        input_cs.Nominal_Range = 0;
        video_ctx_->VideoProcessorSetStreamColorSpace(video_processor_, 0, &input_cs);

        D3D11_VIDEO_PROCESSOR_COLOR_SPACE output_cs = {};
        output_cs.Usage = 0;          // playback
        output_cs.RGB_Range = 0;      // full range
        output_cs.YCbCr_Matrix = 1;   // BT.709
        output_cs.YCbCr_xvYCC = 0;
        output_cs.Nominal_Range = 0;
        video_ctx_->VideoProcessorSetOutputColorSpace(video_processor_, &output_cs);

        qDebug() << "[FastEncoder] 色彩空间已设置 (基础API): RGB full -> YCbCr BT.709 full";
    }

    return true;
}

// ========== GPU 格式转换 ==========
bool ObsNvencEncoderFast::ConvertBgraToNv12Gpu(ID3D11Texture2D* src_bgra) {
    if (!src_bgra) return false;

    // 1. 创建输入视图 - 指向源 BGRA 纹理
    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC input_desc = {};
    input_desc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    input_desc.Texture2D.MipSlice = 0;
    input_desc.Texture2D.ArraySlice = 0;

    ID3D11VideoProcessorInputView* input_view = nullptr;
    HRESULT hr = video_device_->CreateVideoProcessorInputView(src_bgra, video_proc_enum_, &input_desc, &input_view);
    if (FAILED(hr)) return false;

    // 2. 创建输出视图 - 指向目标 NV12 纹理
    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC output_desc = {};
    output_desc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    output_desc.Texture2D.MipSlice = 0;

    ID3D11VideoProcessorOutputView* output_view = nullptr;
    hr = video_device_->CreateVideoProcessorOutputView(nv12_texture_, video_proc_enum_, &output_desc, &output_view);
    if (FAILED(hr)) {
        input_view->Release();
        return false;
    }

    // 3. 执行转换
    D3D11_VIDEO_PROCESSOR_STREAM stream = {};
    stream.Enable = TRUE;
    stream.pInputSurface = input_view;

    // 核心：GPU 硬件转换
    video_ctx_->VideoProcessorBlt(video_processor_, output_view, 0, 1, &stream);

    // 释放临时视图
    input_view->Release();
    output_view->Release();

    return true;
}

// ========== 编码一帧 ==========
bool ObsNvencEncoderFast::EncodeFrame(ID3D11Texture2D* texture, uint64_t frame_index,
    bool force_idr, std::vector<uint8_t>& out_data,
    const CursorInfo* /*cursor*/, int /*monitor_x*/, int /*monitor_y*/) {
    out_data.clear();

    // ---- 第一步：GPU 硬件转换 (BGRA -> NV12) ----
    if (!ConvertBgraToNv12Gpu(texture)) {
        qDebug() << "[FastEncoder] GPU 转换失败";
        return false;
    }

    // ---- 第二步：读回数据 (GPU -> CPU) ----
    // 1. 拷贝：从 Default 纹理 -> Staging 纹理
    d3d_ctx_->CopyResource(nv12_staging_, nv12_texture_);

    // 2. 映射：获取 CPU 指针
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    HRESULT hr = d3d_ctx_->Map(nv12_staging_, 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) {
        qDebug() << "[FastEncoder] Map 失败";
        return false;
    }

    // 3. 填充 AVFrame
    // NV12 内存布局：Y 平面 + UV 交错平面
    frame_->data[0] = static_cast<uint8_t*>(mapped.pData);                       // Y 平面
    frame_->data[1] = static_cast<uint8_t*>(mapped.pData) + mapped.RowPitch * height_; // UV 平面

    frame_->linesize[0] = mapped.RowPitch; // Y 行宽
    frame_->linesize[1] = mapped.RowPitch; // UV 行宽 (NV12 特性：UV 行宽与 Y 相同)

    // 设置帧色彩信息 —— 与编码器 VUI 和 Video Processor 输出保持一致
    frame_->color_range = AVCOL_RANGE_JPEG;   // full range
    frame_->colorspace = AVCOL_SPC_BT709;     // BT.709

    frame_->pts = frame_index;

    if (force_idr) {
        frame_->pict_type = AV_PICTURE_TYPE_I;
    }
    else {
        frame_->pict_type = AV_PICTURE_TYPE_NONE;
    }

    // ---- 第三步：发送到编码器 ----
    int ret = avcodec_send_frame(codec_ctx_, frame_);

    // 必须立即 Unmap，否则显存被锁住
    d3d_ctx_->Unmap(nv12_staging_, 0);

    if (ret < 0) {
        qDebug() << "[FastEncoder] send_frame 错误:" << ret;
        return false;
    }

    // ---- 第四步：获取编码结果 ----
    ret = avcodec_receive_packet(codec_ctx_, packet_);
    if (ret == 0) {
        out_data.assign(packet_->data, packet_->data + packet_->size);
        av_packet_unref(packet_);
        return true;
    }
    else if (ret == AVERROR(EAGAIN)) {
        // 需要更多输入，正常情况
        return false;
    }
    else {
        qDebug() << "[FastEncoder] receive_packet 错误:" << ret;
        return false;
    }
}

// ========== 刷新编码器 ==========
void ObsNvencEncoderFast::Flush(std::function<void(const std::vector<uint8_t>&)> on_packet) {
    if (!codec_ctx_) return;

    avcodec_send_frame(codec_ctx_, nullptr);

    while (true) {
        int ret = avcodec_receive_packet(codec_ctx_, packet_);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
        if (ret < 0) break;

        std::vector<uint8_t> data(packet_->data, packet_->data + packet_->size);
        on_packet(data);
        av_packet_unref(packet_);
    }
}

// ========== 释放资源 ==========
void ObsNvencEncoderFast::Release() {
    // FFmpeg 资源
    if (packet_) av_packet_free(&packet_);
    if (frame_) av_frame_free(&frame_);
    if (codec_ctx_) avcodec_free_context(&codec_ctx_);

    // D3D11 视频资源
    if (video_processor_) video_processor_->Release();
    if (video_proc_enum_) video_proc_enum_->Release();
    if (video_ctx_) video_ctx_->Release();
    if (video_device_) video_device_->Release();

    // 纹理资源
    if (nv12_texture_) nv12_texture_->Release();
    if (nv12_staging_) nv12_staging_->Release();

    // D3D 基础资源
    if (d3d_ctx_) d3d_ctx_->Release();
    if (d3d_device_) d3d_device_->Release();

    // 指针置空
    video_processor_ = nullptr;
    video_proc_enum_ = nullptr;
    video_ctx_ = nullptr;
    video_device_ = nullptr;
    nv12_texture_ = nullptr;
    nv12_staging_ = nullptr;
    d3d_ctx_ = nullptr;
    d3d_device_ = nullptr;
    codec_ctx_ = nullptr;
}