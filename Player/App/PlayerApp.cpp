#include "PlayerApp.h"
#include "Client/MainUI/MainWindow.h"
#include "Client/MainUI/VideoWidget.h"
#include "Client/MainUI/ControlBar.h"
#include "Client/MainUI/FileBrowser.h"
#include "Client/Core/FFmpegPlayer.h"
#include "Server/OBS_Capture/MonitorCapture.h"
#include "Server/OBS_Capture/ObsNvencEncoder.h"
#include <d3d11.h>
extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixfmt.h>
#include <libavformat/avformat.h>
}

#include <QApplication>
#include <QFile>
#include <QSlider>
#include <QTimer>
#include <QFileDialog>
#include <QDebug>

// ==== 辅助函数：毫秒 → "mm:ss" 格式 ====
static QString msToTimeStr(qint64 ms)
{
    int total_sec = static_cast<int>(ms / 1000);
    int min = total_sec / 60;
    int sec = total_sec % 60;
    return QString("%1:%2")
        .arg(min, 2, 10, QChar('0'))
        .arg(sec, 2, 10, QChar('0'));
}

PlayerApp::PlayerApp(QObject* parent)
    : QObject(parent)
{
}

PlayerApp::~PlayerApp()
{
    if (main_window_)
    {
        delete main_window_;
        main_window_ = nullptr;
    }
}

bool PlayerApp::Init(int argc, char* argv[])
{
    Q_UNUSED(argc);
    Q_UNUSED(argv);

    LoadStyle();
    CreateUI();
    CreatePlayer();
    BindSignals();

    TestScreenCapture();        // 取消注释即可运行测试
    // 不再自动打开视频，等待用户通过文件浏览器选择

    return true;
}

void PlayerApp::LoadStyle()
{
    QFile qss("QSS/style.qss");
    if (qss.open(QFile::ReadOnly | QFile::Text))
    {
        QString style = qss.readAll();
        qApp->setStyleSheet(style);
        qDebug() << "[PlayerApp] 样式文件加载成功";
    }
    else
    {
        qDebug() << "[PlayerApp] 样式文件加载失败，使用默认样式";
    }
}

void PlayerApp::CreateUI()
{
    main_window_ = new MainWindow();
    main_window_->SetVideoRect(100, 100, 1280, 720);
    main_window_->show();
}

void PlayerApp::CreatePlayer()
{
    player_ = new FFmpegPlayer(this);

    // D3D11 交换链绑定到 VideoWidget 的原生 HWND
    player_->SetVideoHwnd(main_window_->GetVideoWidget()->GetVideoHwnd());

    // 软解回退：视频帧 QImage → VideoWidget 的 QLabel
    QObject::connect(player_, &FFmpegPlayer::SigFrameReady,
        main_window_->GetVideoWidget(), &VideoWidget::OnFrameReady);

    // ===== 状态信号 =====
    QObject::connect(player_, &FFmpegPlayer::SigLoaded, this, [this](qint64 duration_ms) {
        qDebug("文件加载完成，时长: %lld ms", duration_ms);

        ControlBar* bar = main_window_->GetControlBar();

        bar->GetSeekBar()->setRange(0, static_cast<int>(duration_ms));

        bar->GetTimeLabel()->setText(
            QString("00:00 / %1").arg(msToTimeStr(duration_ms)));
        });

    QObject::connect(player_, &FFmpegPlayer::SigFinished, this, [this]() 
        {
            qDebug("播放结束");

            // ---- 停止进度轮询 ----
            if (progress_timer_)
                progress_timer_->stop();

            // ---- UI 重置（这些是线程安全的信号槽调用，不受影响） ----
            main_window_->GetControlBar()->GetPlayBtn()->setText("▶");
            main_window_->GetControlBar()->GetSeekBar()->setValue(0);
            main_window_->GetControlBar()->GetTimeLabel()->setText("00:00 / 00:00");
            main_window_->GetVideoWidget()->ClearFrame();
        });

    QObject::connect(player_, &FFmpegPlayer::SigError, [](const QString& msg) {
        qDebug("错误: %s", qPrintable(msg));
        });

    QObject::connect(player_, &FFmpegPlayer::SigPlayState, [](const QString& state) {
        qDebug("状态变化: %s", qPrintable(state));
        });
}

void PlayerApp::BindSignals()
{
    ControlBar* bar = main_window_->GetControlBar();

    // ---- 播放/暂停（无文件时打开文件浏览器） ----
    QObject::connect(bar->GetPlayBtn(), &QPushButton::clicked, this, [this, bar]() {
        if (!player_ || player_->GetDuration() <= 0)
        {
            // 未加载文件 → 弹出文件浏览器让用户选择
            OnSelectFile();
            return;
        }

        if (player_->IsPlaying())
        {
            if (player_->IsPaused())
            {
                player_->Play();
                bar->GetPlayBtn()->setText("⏸");
                StartProgressTimer();
            }
            else
            {
                player_->Pause();
                bar->GetPlayBtn()->setText("▶");
            }
        }
        else
        {
            player_->Play();
            bar->GetPlayBtn()->setText("⏸");
            StartProgressTimer();
        }
        });

    // ---- 进度条拖动 ----
    QObject::connect(bar->GetSeekBar(), &QSlider::sliderPressed, this, [this]() {
        if (progress_timer_)
            progress_timer_->stop();
        });

    QObject::connect(bar->GetSeekBar(), &QSlider::sliderReleased, this, [this]() {
        int pos_ms = main_window_->GetControlBar()->GetSeekBar()->value();
        player_->Seek(pos_ms);

        if (player_->IsPlaying() && !player_->IsPaused())
            StartProgressTimer();
        });

    QObject::connect(bar, &ControlBar::SigSeekRequested, this, [this](int pos_ms) {
        player_->Seek(pos_ms);

        if (player_->IsPlaying() && !player_->IsPaused())
            StartProgressTimer();
        });

    // 拖拽过程中仅更新时间标签
    QObject::connect(bar->GetSeekBar(), &QSlider::sliderMoved, this, [this, bar](int pos_ms) {
        qint64 dur_ms = player_->GetDuration();
        auto ts = [](qint64 ms) -> QString {
            int s = static_cast<int>(ms / 1000);
            return QString("%1:%2")
                .arg(s / 60, 2, 10, QChar('0'))
                .arg(s % 60, 2, 10, QChar('0'));
            };
        bar->GetTimeLabel()->setText(
            QString("%1 / %2").arg(ts(pos_ms)).arg(ts(dur_ms)));
        });

    // ---- 音量控制 ----
    QObject::connect(bar->GetVolSlider(), &QSlider::valueChanged, this, [this](int vol) {
        player_->SetVolume(vol / 100.0);
        });

    // ---- 文件浏览器切换 ----
    QObject::connect(bar, &ControlBar::SigToggleFileBrowser,
        main_window_, &MainWindow::ToggleFileBrowser);

    // ---- 文件浏览器选定文件 ----
    QObject::connect(main_window_->GetFileBrowser(), &FileBrowser::SigFileSelected,
        this, &PlayerApp::OnFileSelected);

    // ---- 外部拖拽文件 ----
    QObject::connect(main_window_, &MainWindow::SigFileDropped,
        this, [this](const QString& path) {
            OpenFile(path);
        });

    // ---- 退出 ----
    QObject::connect(main_window_, &MainWindow::SigRequestClose, this, [this]() {
        if (player_)
        {
            player_->Close();
            delete player_;
            player_ = nullptr;
        }
        });
}

// ---- 启动进度轮询（每 200ms） ----
void PlayerApp::StartProgressTimer()
{
    if (!progress_timer_)
    {
        progress_timer_ = new QTimer(this);
        QObject::connect(progress_timer_, &QTimer::timeout, this, &PlayerApp::UpdateProgress);
    }
    progress_timer_->start(200);
}

// ---- 更新进度条 + 时间标签 ----
void PlayerApp::UpdateProgress()
{
    if (!player_ || !main_window_) return;

    qint64 pos_ms = player_->GetPosition();
    qint64 dur_ms = player_->GetDuration();

    ControlBar* bar = main_window_->GetControlBar();

    bar->GetSeekBar()->blockSignals(true);
    bar->GetSeekBar()->setValue(static_cast<int>(pos_ms));
    bar->GetSeekBar()->blockSignals(false);

    bar->GetTimeLabel()->setText(
        QString("%1 / %2").arg(msToTimeStr(pos_ms)).arg(msToTimeStr(dur_ms)));
}

// ---- 打开文件并播放 ----
void PlayerApp::OpenFile(const QString& path)
{
    player_->Stop();

    if (player_->OpenFile(path))
    {
        player_->Play();
        main_window_->GetControlBar()->GetPlayBtn()->setText("⏸");
        StartProgressTimer();
    }
    else
    {
        qDebug("文件打开失败: %s", qPrintable(path));
    }
}

// ---- 文件浏览器选定文件 ----
void PlayerApp::OnFileSelected(const QString& path)
{
    OpenFile(path);
}

// ---- 点击播放按钮但无文件时，弹出文件选择对话框 ----
void PlayerApp::OnSelectFile()
{
    QString path = QFileDialog::getOpenFileName(
        main_window_,
        u8"选择视频文件",
        QString(),
        u8"视频文件 (*.mp4 *.mkv *.avi *.mov *.flv *.wmv *.ts *.webm *.m4v "
        u8"*.3gp *.mpeg *.mpg *.rmvb *.vob);;所有文件 (*.*)");

    if (!path.isEmpty())
        OpenFile(path);
}

// 测试桌面捕获 + NVENC 编码（FFmpeg 架构）
void PlayerApp::TestScreenCapture()
{
    qDebug() << "[PlayerApp] 开始录制 10 秒（MonitorCapture + FFmpeg NVENC）";

    // ---- 第一步：创建 D3D11 设备 ----
    D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    ID3D11Device* d3d_device = nullptr;
    ID3D11DeviceContext* d3d_ctx = nullptr;
    HRESULT hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
        levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
        &d3d_device, nullptr, &d3d_ctx);
    if (FAILED(hr))
    {
        qDebug() << "[PlayerApp] D3D11 设备创建失败, HR =" << hr;
        return;
    }

    // ---- 第二步：枚举显示器，取主显示器 ----
    auto monitors = MonitorCapture::EnumerateMonitors();
    if (monitors.empty())
    {
        qDebug() << "[PlayerApp] 未找到显示器";
        d3d_ctx->Release();
        d3d_device->Release();
        return;
    }

    for (size_t i = 0; i < monitors.size(); ++i)
    {
        qDebug() << "[PlayerApp] 显示器" << i << ":"
            << monitors[i].name
            << monitors[i].rect.right - monitors[i].rect.left << "x"
            << monitors[i].rect.bottom - monitors[i].rect.top;
    }

    const char* monitor_id = monitors[1].device_id;
    qDebug() << "[PlayerApp] 目标:" << monitors[1].name;

    // ---- 第三步：初始化采集器 ----
    MonitorCapture capture;
    if (!capture.Init(d3d_device, monitor_id,
        DisplayCaptureMethod::Auto, true, false))
    {
        qDebug() << "[PlayerApp] 采集器初始化失败";
        d3d_ctx->Release();
        d3d_device->Release();
        return;
    }

    // ---- 第四步：等首帧到达 ----
    int fps = 60;
    CaptureFrame frame;
    int wait = 0;

    while (wait < 300)
    {
        capture.Tick(1.0f / fps);
        if (capture.GetFrame(frame) && frame.IsValid())
            break;
        ++wait;
        Sleep(16);
    }

    if (!frame.IsValid())
    {
        qDebug() << "[PlayerApp] 首帧超时";
        capture.Shutdown();
        d3d_ctx->Release();
        d3d_device->Release();
        return;
    }

    int width = static_cast<int>(frame.width);
    int height = static_cast<int>(frame.height);
    qDebug() << "[PlayerApp] 首帧:" << width << "x" << height
        << (frame.IsGpu() ? "GPU" : "CPU");

    // ---- 第五步：初始化编码器 ----
    ObsNvencEncoder encoder;
    if (!encoder.Init(d3d_device, width, height, fps, 10000))
    {
        qDebug() << "[PlayerApp] 编码器初始化失败";
        capture.Shutdown();
        d3d_ctx->Release();
        d3d_device->Release();
        return;
    }

    // ---- 第六步：打开输出文件 ----
    FILE* fp = nullptr;
    fopen_s(&fp, "capture_10s.h264", "wb");
    if (!fp)
    {
        qDebug() << "[PlayerApp] 文件创建失败";
        encoder.Release();
        capture.Shutdown();
        d3d_ctx->Release();
        d3d_device->Release();
        return;
    }

    // ---- 第七步：GDI 路线需要上传纹理 ----
    ID3D11Texture2D* upload_tex = nullptr;
    if (!frame.IsGpu())
    {
        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = width;
        desc.Height = height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        d3d_device->CreateTexture2D(&desc, nullptr, &upload_tex);
    }

    // ---- 第八步：主循环 ----
    int total = fps * 10;
    int written = 0;
    uint64_t idx = 0;

    qDebug() << "[PlayerApp] 开始录制...";

    while (written < total)
    {
        capture.Tick(1.0f / fps);

        if (!capture.GetFrame(frame) || !frame.IsValid())
            continue;

        ID3D11Texture2D* tex = nullptr;
        if (frame.IsGpu())
        {
            tex = frame.gpu_texture;
        }
        else if (upload_tex && frame.cpu_data)
        {
            d3d_ctx->UpdateSubresource(upload_tex, 0, nullptr,
                frame.cpu_data, frame.cpu_stride, 0);
            tex = upload_tex;
        }

        if (!tex)
            continue;

        std::vector<uint8_t> h264_data;
        if (encoder.EncodeFrame(tex, idx, (idx == 0), h264_data))
        {
            fwrite(h264_data.data(), 1, h264_data.size(), fp);
            ++written;

            if (written % fps == 0)
                qDebug() << "[PlayerApp] 已录制" << written / fps << "秒";
        }

        ++idx;
    }

    // ---- 第九步：Flush ----
    qDebug() << "[PlayerApp] Flush...";
    encoder.Flush([&](const std::vector<uint8_t>& data)
        {
            fwrite(data.data(), 1, data.size(), fp);
            ++written;
        });

    fclose(fp);
    if (upload_tex) upload_tex->Release();
    encoder.Release();
    capture.Shutdown();
    d3d_ctx->Release();
    d3d_device->Release();

    qDebug() << "[PlayerApp] 完成:" << written << "帧 → capture_10s.h264";
}
