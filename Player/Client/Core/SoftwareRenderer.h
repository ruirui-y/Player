#ifndef SOFTWARERENDERER_H
#define SOFTWARERENDERER_H

#include <QObject>
#include <QImage>

struct AVFrame;

// CPU 软解渲染器
// 职责：sws_scale 做 YUV→RGB 转换 → 发射 QImage 给主线程显示
// 这是 GPU 渲染不可用时的回退路径
class SoftwareRenderer : public QObject
{
    Q_OBJECT

public:
    explicit SoftwareRenderer(QObject* parent = nullptr);                       // 构造
    ~SoftwareRenderer();                                                        // 析构

    void Render(AVFrame* frame);                                                // 渲染一帧（YUV→RGB → QImage）

signals:
    void SigFrameReady(const QImage& image);                                    // 一帧转换完成，交给主线程显示

private:
    void* sws_ctx_{ nullptr };                                                  // SwsContext*，CPU RGB 转换
    int   frame_width_{ 0 };                                                    // 当前视频宽度（分辨率变化时重建）
    int   frame_height_{ 0 };                                                   // 当前视频高度
};

#endif // SOFTWARERENDERER_H
