#ifndef NV12GPUPLOADER_H
#define NV12GPUPLOADER_H

#include <d3d11.h>
#include <d3d11_3.h>
#include <dxgi.h>

struct AVFrame;

// NV12 GPU 上传器
// 职责：把 AVFrame 指向的 GPU 解码帧，通过 CopySubresourceRegion
//       拷贝到自建 NV12 纹理，并通过 PlaneSlice SRV 使着色器可读
// 全程不经过 CPU
class Nv12GpuUploader
{
public:
    Nv12GpuUploader() = default;
    ~Nv12GpuUploader();

    void Init(ID3D11Device* device, ID3D11DeviceContext* ctx);                  // 初始化，传入 D3D11 设备和上下文
    void Release();                                                             // 释放所有资源

    bool UploadFrame(AVFrame* frame,                                            // 上传一帧，返回两个 SRV（Y / UV）
        ID3D11ShaderResourceView*& out_srv_y,
        ID3D11ShaderResourceView*& out_srv_uv);

private:
    bool CreateNV12Texture(int width, int height);                              // 创建自建 NV12 纹理
    bool CreatePlaneSliceSRVs();                                                // 创建 PlaneSlice SRV（Y 和 UV）

    ID3D11Device3*          d3d11_device3_{ nullptr };                          // D3D 11.3 接口（PlaneSlice 需要）
    ID3D11Device*           d3d11_device_{ nullptr };                           // D3D11 设备
    ID3D11DeviceContext*    d3d11_ctx_{ nullptr };                              // D3D11 设备上下文

    ID3D11Texture2D*            nv12_texture_{ nullptr };                       // 自建 NV12 纹理
    ID3D11ShaderResourceView*   nv12_srv_y_{ nullptr };                         // PlaneSlice=0 → R8 (Y)
    ID3D11ShaderResourceView*   nv12_srv_uv_{ nullptr };                        // PlaneSlice=1 → R8G8 (UV)
    int                         nv12_width_{ 0 };                               // 缓存分辨率
    int                         nv12_height_{ 0 };
};

#endif // NV12GPUPLOADER_H
