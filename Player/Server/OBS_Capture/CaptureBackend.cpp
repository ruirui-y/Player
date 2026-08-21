#include "CaptureBackend.h"

#include "GdiCapture.h"
#include "WgcCapture.h"
#include "DxgiDuplicator.h"

#include <memory>

// ========== 工厂：按采集方法创建对应后端 ==========
// Auto / 非法值返回 nullptr（由调用方解析为具体方法后再创建）

std::unique_ptr<CaptureBackend> CreateCaptureBackend(DisplayCaptureMethod method)
{
    switch (method)
    {
    case DisplayCaptureMethod::Gdi:  return std::make_unique<GdiCapture>();
    case DisplayCaptureMethod::Wgc:  return std::make_unique<WgcCapture>();
    case DisplayCaptureMethod::Dxgi: return std::make_unique<DxgiDuplicator>();
    default:                         return nullptr;
    }
}
