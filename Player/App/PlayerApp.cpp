#include "PlayerApp.h"
#include "Client/MainUI/MainWindow.h"
#include "Client/MainUI/VideoWidget.h"
#include "Client/MainUI/ControlBar.h"
#include "Client/MainUI/FileBrowser.h"
#include "Client/StreamUI/StreamWindow.h"
#include "Client/StreamUI/StreamVideoWidget.h"
#include "Client/Core/FFmpegPlayer.h"
#include "Server/OBS_Capture/MonitorCapture.h"
#include "Server/OBS_Capture/ObsNvencEncoder.h"
#include "Server/OBS_Capture/ObsNvencEncoderFast.h"
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
#include <cstring>
#include <cstdlib>

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
    if (player_)
    {
        player_->Close();
        delete player_;
        player_ = nullptr;
    }

    if (main_window_)
    {
        delete main_window_;
        main_window_ = nullptr;
    }

    if (stream_window_)
    {
        delete stream_window_;
        stream_window_ = nullptr;
    }
}

// ================================================================
// ---- Init：解析命令行，分流两种模式 ----
// ================================================================
bool PlayerApp::Init(int argc, char* argv[])
{
    LoadStyle();

    // ---- 检测串流模式 ----
    // 用法：Player.exe --stream --port 47998 --fps 60
    // 分辨率不需要指定：服务端发什么分辨率，客户端就显示什么分辨率
    // H.264 码流的 SPS 中包含分辨率信息，解码器自动解析
    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--stream") == 0)
        {
            uint16_t port = 47998;
            int fps = 60;
            uint16_t ctrl_port = 47989;

            for (int j = i + 1; j < argc; ++j)
            {
                if (std::strcmp(argv[j], "--port") == 0 && j + 1 < argc)
                    port = (uint16_t)std::atoi(argv[++j]);
                else if (std::strcmp(argv[j], "--fps") == 0 && j + 1 < argc)
                    fps = std::atoi(argv[++j]);
                else if (std::strcmp(argv[j], "--ctrl-port") == 0 && j + 1 < argc)
                    ctrl_port = (uint16_t)std::atoi(argv[++j]);
            }

            is_streaming_ = true;
            CreateStreamUI();
            CreateStreamPlayer();
            BindStreamSignals();
            OpenStream(port, fps);

            // ---- 第三阶段：延迟启动输入转发 ----
            // 等 VideoReceiver 收到第一个 UDP 包后，自动获取服务端 IP
            // 不再依赖命令行 --ip 参数（两台设备时用户可能忘记传）
            QTimer* input_timer = new QTimer(this);
            QObject::connect(input_timer, &QTimer::timeout, this, [this, input_timer, ctrl_port]()
                {
                    std::string sender_ip = player_->GetSenderIP();
                    if (!sender_ip.empty())
                    {
                        input_timer->stop();
                        input_timer->deleteLater();
                        stream_window_->StartInput(sender_ip.c_str(), ctrl_port);
                    }
                });
            input_timer->start(500);   // 每 500ms 检查一次，直到收到视频流
            return true;
        }
    }

    // ---- 默认：文件播放器模式 ----
    CreatePlayerUI();
    CreatePlayer();
    BindPlayerSignals();
    // TestScreenCapture();
    return true;
}

// ---- 加载 QSS 样式 ----
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

// ================================================================
// ============== 文件播放器模式 ==============
// ================================================================

void PlayerApp::CreatePlayerUI()
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

            // ---- UI 重置 ----
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

void PlayerApp::BindPlayerSignals()
{
    ControlBar* bar = main_window_->GetControlBar();

    // ---- 播放/暂停（无文件时打开文件浏览器） ----
    QObject::connect(bar->GetPlayBtn(), &QPushButton::clicked, this, [this, bar]() {
        if (!player_ || player_->GetDuration() <= 0)
        {
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

void PlayerApp::StartProgressTimer()
{
    if (!progress_timer_)
    {
        progress_timer_ = new QTimer(this);
        QObject::connect(progress_timer_, &QTimer::timeout, this, &PlayerApp::UpdateProgress);
    }
    progress_timer_->start(200);
}

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

void PlayerApp::OnFileSelected(const QString& path)
{
    OpenFile(path);
}

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
    qDebug() << "[PlayerApp] 开始录制 10 秒（MonitorCapture + FFmpeg NVENC Fast）";

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

    // 注意：这里 monitors[1] 是您原来的逻辑，请确保索引有效
    if (monitors.size() < 2) {
        qDebug() << "[PlayerApp] 未找到第二个显示器，尝试使用第一个";
        if (monitors.empty()) return;
    }
    const char* monitor_id = monitors.size() > 1 ? monitors[1].device_id : monitors[0].device_id;
    qDebug() << "[PlayerApp] 目标:" << (monitors.size() > 1 ? monitors[1].name : monitors[0].name);

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

    // ---- 第五步：初始化编码器 (修改点：使用 Fast 版本) ----
    ObsNvencEncoderFast encoder;
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
    fopen_s(&fp, "capture_10s_fast.h264", "wb"); // 改个文件名以示区分
    if (!fp)
    {
        qDebug() << "[PlayerApp] 文件创建失败";
        encoder.Release();
        capture.Shutdown();
        d3d_ctx->Release();
        d3d_device->Release();
        return;
    }

    // ---- 第七步：GDI 路线需要上传纹理 (修改点：增加 BindFlags) ----
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

        // 【关键修改】必须添加 BIND_SHADER_RESOURCE 或 BIND_VIDEO_PROCESSOR
        // 否则 VideoProcessorInputView 创建会失败
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        d3d_device->CreateTexture2D(&desc, nullptr, &upload_tex);
    }

    if (frame.IsValid())
    {
        int width = static_cast<int>(frame.width);
        int height = static_cast<int>(frame.height);
        qDebug() << "[PlayerApp] 首帧:" << width << "x" << height
            << (frame.IsGpu() ? "GPU" : "CPU");

        if (frame.IsGpu())
        {
            ID3D11Texture2D* tex = frame.gpu_texture;
            D3D11_TEXTURE2D_DESC desc;
            tex->GetDesc(&desc);  // 需要传入参数
            DXGI_FORMAT format = desc.Format;
            qDebug() << "[PlayerApp] 输入 GPU 纹理格式:" << format;

            // 检查是否为BT.601或BT.709
            if (format == DXGI_FORMAT_B8G8R8A8_UNORM)
                qDebug() << "[PlayerApp] 输入可能是sRGB (BT.709)";
            else if (format == DXGI_FORMAT_YUY2)
                qDebug() << "[PlayerApp] 输入可能是BT.601 (YUY2)";
        }
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

    qDebug() << "[PlayerApp] 完成:" << written << "帧 → capture_10s_fast.h264";
}


// ================================================================
// ============== 串流客户端模式 ==============
// ================================================================

void PlayerApp::CreateStreamUI()
{
    stream_window_ = new StreamWindow();
    stream_window_->SetVideoRect(200, 100, 1280, 720);
    stream_window_->show();
}

void PlayerApp::CreateStreamPlayer()
{
    player_ = new FFmpegPlayer(this);

    // D3D11 交换链绑定到 StreamVideoWidget 的原生 HWND
    player_->SetVideoHwnd(stream_window_->GetVideoHwnd());

    // 软解回退：视频帧 QImage → StreamVideoWidget 的 QLabel
    QObject::connect(player_, &FFmpegPlayer::SigFrameReady,
        stream_window_->GetVideoWidget(), &StreamVideoWidget::OnFrameReady);

    // ---- 状态信号：更新底部状态栏 ----
    QObject::connect(player_, &FFmpegPlayer::SigLoaded, this, [this](qint64) {
        stream_window_->SetStatusText(u8"已连接");
        });

    QObject::connect(player_, &FFmpegPlayer::SigError, this, [this](const QString& msg) {
        stream_window_->SetStatusText(u8"错误: " + msg);
        qDebug("[Stream] 错误: %s", qPrintable(msg));
        });

    QObject::connect(player_, &FFmpegPlayer::SigPlayState, this, [this](const QString& state) {
        qDebug("[Stream] 状态: %s", qPrintable(state));
        });
}

void PlayerApp::BindStreamSignals()
{
    // ---- 退出：关闭播放器引擎 ----
    QObject::connect(stream_window_, &StreamWindow::SigRequestClose, this, [this]() {
        if (player_)
        {
            player_->Close();
            delete player_;
            player_ = nullptr;
        }
        });
}

void PlayerApp::OpenStream(uint16_t port, int fps)
{
    stream_window_->SetStatusText(
        QString(u8"等待连接... 端口 %1  @%2fps")
            .arg(port).arg(fps));

    if (!player_->OpenStream(port, fps))
    {
        qDebug() << "[PlayerApp] 串流启动失败，端口" << port;
        stream_window_->SetStatusText(u8"连接失败");
        return;
    }

    player_->Play();

    qDebug() << "[PlayerApp] 串流模式启动，端口" << port
             << "@" << fps << "fps";
}
