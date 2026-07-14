#include "Nv12GpuUploader.h"
#include <QDebug>

#include <libavutil/frame.h>

Nv12GpuUploader::~Nv12GpuUploader()
{
    Release();
}

// 初始化，传入 D3D11 设备和上下文
void Nv12GpuUploader::Init(ID3D11Device* device, ID3D11DeviceContext* ctx)
{
    d3d11_device_ = device;
    d3d11_ctx_ = ctx;

    // 查询 D3D 11.3 接口（PlaneSlice 需要）
    if (d3d11_device_)
    {
        d3d11_device_->QueryInterface(
            __uuidof(ID3D11Device3), (void**)&d3d11_device3_);
    }
}

// 释放所有资源
void Nv12GpuUploader::Release()
{
    if (nv12_srv_uv_) { nv12_srv_uv_->Release();  nv12_srv_uv_ = nullptr; }
    if (nv12_srv_y_) { nv12_srv_y_->Release();   nv12_srv_y_ = nullptr; }
    if (nv12_texture_) { nv12_texture_->Release(); nv12_texture_ = nullptr; }
    if (d3d11_device3_) { d3d11_device3_->Release(); d3d11_device3_ = nullptr; }
    nv12_width_ = 0;
    nv12_height_ = 0;
}

// 创建自建 NV12 纹理
bool Nv12GpuUploader::CreateNV12Texture(int width, int height)
{
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.Format = DXGI_FORMAT_NV12;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.SampleDesc.Count = 1;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.MiscFlags = 0;
    desc.CPUAccessFlags = 0;

    HRESULT hr = d3d11_device_->CreateTexture2D(&desc, nullptr, &nv12_texture_);
    if (FAILED(hr))
    {
        qDebug() << "[Nv12GpuUploader] 创建 NV12 纹理失败, HR=" << hr;
        return false;
    }
    nv12_width_ = width;
    nv12_height_ = height;
    return true;
}

// 为 NV12 纹理创建两个 SRV（D3D 11.3 PlaneSlice 方案）
// 不需要把 NV12 拆成两张纹理——一个 SRV 用 PlaneSlice=0 读 Y，另一个用 PlaneSlice=1 读 UV
bool Nv12GpuUploader::CreatePlaneSliceSRVs()
{
    if (!d3d11_device3_ || !nv12_texture_)
    {
        qDebug() << "[Nv12GpuUploader] 创建 SRV 失败: D3D11.3 接口或纹理为空";
        return false;
    }

    // ---- 构建 SRV 描述（必须用 D3D11.1+ 的 DESC1 结构体才能访问 PlaneSlice） ----
    D3D11_SHADER_RESOURCE_VIEW_DESC1 srv_desc = {};
    srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
    srv_desc.Texture2DArray.MipLevels = 1;
    srv_desc.Texture2DArray.FirstArraySlice = 0;
    srv_desc.Texture2DArray.ArraySize = 1;

    // ---- Y 平面：PlaneSlice=0，格式 R8_UNORM ----
    srv_desc.Format = DXGI_FORMAT_R8_UNORM;
    srv_desc.Texture2DArray.PlaneSlice = 0;

    ID3D11ShaderResourceView1* tmp_y = nullptr;
    HRESULT hr = d3d11_device3_->CreateShaderResourceView1(
        nv12_texture_, &srv_desc, &tmp_y);
    if (FAILED(hr))
    {
        qDebug() << "[Nv12GpuUploader] 创建 Y Plane SRV 失败, HR=" << hr;
        return false;
    }
    nv12_srv_y_ = tmp_y;   // ID3D11ShaderResourceView1* 兼容赋值给 ID3D11ShaderResourceView*

    // ---- UV 平面：PlaneSlice=1，格式 R8G8_UNORM ----
    srv_desc.Format = DXGI_FORMAT_R8G8_UNORM;
    srv_desc.Texture2DArray.PlaneSlice = 1;

    ID3D11ShaderResourceView1* tmp_uv = nullptr;
    hr = d3d11_device3_->CreateShaderResourceView1(
        nv12_texture_, &srv_desc, &tmp_uv);
    if (FAILED(hr))
    {
        qDebug() << "[Nv12GpuUploader] 创建 UV Plane SRV 失败, HR=" << hr;
        if (nv12_srv_y_) { nv12_srv_y_->Release(); nv12_srv_y_ = nullptr; }
        return false;
    }
    nv12_srv_uv_ = tmp_uv;

    return true;
}

// ================================================================
// UploadFrame：上传一帧，返回两个 SRV，每帧调用
//
// 流程：
//   第①步：从 AVFrame 取解码器纹理和帧索引
//   第②步：首次或分辨率变化时，创建自建 NV12 纹理 + PlaneSlice SRV
//   第③步：CopySubresourceRegion，仅拷贝当前帧（GPU 内部）
// ================================================================
bool Nv12GpuUploader::UploadFrame(
    AVFrame* frame,
    ID3D11ShaderResourceView*& out_srv_y,
    ID3D11ShaderResourceView*& out_srv_uv)
{
    out_srv_y = nullptr;
    out_srv_uv = nullptr;

    if (!d3d11_ctx_ || !frame || frame->format != AV_PIX_FMT_D3D11)
    {
        return false;
    }

    // ---- 第①步：从 frame 取解码器纹理和帧索引 ----
    // frame->data[0] 在硬解模式下直接存了 ID3D11Texture2D* 指针
    // frame->data[1] 存了帧在数组纹理中的索引
    ID3D11Texture2D* decoder_texture = (ID3D11Texture2D*)frame->data[0];
    int subresource_index = (int)(intptr_t)frame->data[1];
    if (!decoder_texture)
    {
        qDebug() << "[Nv12GpuUploader] frame->data[0] 为空";
        return false;
    }

    D3D11_TEXTURE2D_DESC dec_desc;
    decoder_texture->GetDesc(&dec_desc);

    int w = dec_desc.Width;
    int h = dec_desc.Height;

    // ---- 第②步：首次运行或分辨率变化时，创建自己的 NV12 纹理和 SRV ----
    if (!nv12_texture_ || w != nv12_width_ || h != nv12_height_)
    {
        // 清理旧资源
        if (nv12_srv_uv_) { nv12_srv_uv_->Release();  nv12_srv_uv_ = nullptr; }
        if (nv12_srv_y_) { nv12_srv_y_->Release();   nv12_srv_y_ = nullptr; }
        if (nv12_texture_) { nv12_texture_->Release(); nv12_texture_ = nullptr; }

        if (!CreateNV12Texture(w, h))
        {
            qDebug() << "[Nv12GpuUploader] 创建 NV12 纹理失败";
            return false;
        }
        if (!CreatePlaneSliceSRVs())
        {
            qDebug() << "[Nv12GpuUploader] 创建 PlaneSlice SRV 失败";
            return false;
        }
    }

    // ---- 第③步：CopySubresourceRegion — 只拷贝当前帧 ----
    // 源纹理是 ArraySize=20 的数组纹理，用 subresource_index 指定要拷贝哪一帧
    // 目标纹理是 ArraySize=1 的单独纹理，subresource=0
    D3D11_BOX src_box;
    src_box.left = 0;
    src_box.top = 0;
    src_box.front = 0;
    src_box.right = w;
    src_box.bottom = h;
    src_box.back = 1;

    d3d11_ctx_->CopySubresourceRegion(
        nv12_texture_,       // 目标纹理
        0,                   // 目标 subresource（ArraySize=1 → 0）
        0, 0, 0,             // 目标偏移
        decoder_texture,     // 源纹理（FFmpeg 的数组纹理）
        subresource_index,   // 源 subresource（= 帧在数组中的索引）
        &src_box);

    out_srv_y = nv12_srv_y_;
    out_srv_uv = nv12_srv_uv_;
    return true;
}
