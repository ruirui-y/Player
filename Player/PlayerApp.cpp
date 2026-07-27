#include "PlayerApp.h"
#include "MainUI/MainWindow.h"
#include "MainUI/VideoWidget.h"
#include "MainUI/ControlBar.h"
#include "Core/FFmpegPlayer.h"

#include <QApplication>
#include <QFile>
#include <QSlider>
#include <QTimer>
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

    QString videoPath = "H:/YJJ/Project/Player/Player/Movie/huanyou.mp4";
    OpenFile(videoPath);

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
    QObject::connect(player_, &FFmpegPlayer::SigLoaded, [this](qint64 duration_ms) {
        qDebug("文件加载完成，时长: %lld ms", duration_ms);

        ControlBar* bar = main_window_->GetControlBar();

        // 设置进度条范围：0 ~ 总时长（毫秒）
        bar->GetSeekBar()->setRange(0, static_cast<int>(duration_ms));

        // 更新时间标签：00:00 / 总时长
        bar->GetTimeLabel()->setText(
            QString("00:00 / %1").arg(msToTimeStr(duration_ms)));
        });

    QObject::connect(player_, &FFmpegPlayer::SigFinished, [this]() {
        qDebug("播放结束");
        main_window_->GetControlBar()->GetPlayBtn()->setText("▶");
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

    // 播放/暂停
    QObject::connect(bar->GetPlayBtn(), &QPushButton::clicked, [this, bar]() {
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
    // 用户拖拽过程中：暂停进度轮询，但不触发 seek
    QObject::connect(bar->GetSeekBar(), &QSlider::sliderPressed, [this]() {
        if (progress_timer_)
            progress_timer_->stop();
        });

    // 用户松开时：才执行真正的 seek
    QObject::connect(bar->GetSeekBar(), &QSlider::sliderReleased, [this]() {
        int pos_ms = main_window_->GetControlBar()->GetSeekBar()->value();
        player_->Seek(pos_ms);

        if (player_->IsPlaying() && !player_->IsPaused())
            StartProgressTimer();
        });

    // 用户点击进度条时
    QObject::connect(bar, &ControlBar::SigSeekRequested, [this](int pos_ms) {
        player_->Seek(pos_ms);

        if (player_->IsPlaying() && !player_->IsPaused())
            StartProgressTimer();
        });

    // 拖拽过程中：只更新进度条和时间标签，不 seek
    QObject::connect(bar->GetSeekBar(), &QSlider::sliderMoved, [this, bar](int pos_ms) {
        // 只更新时间标签，不做 seek
        qint64 dur_ms = player_->GetDuration();
        auto msToTimeStr = [](qint64 ms) -> QString {
            int total_sec = static_cast<int>(ms / 1000);
            return QString("%1:%2")
                .arg(total_sec / 60, 2, 10, QChar('0'))
                .arg(total_sec % 60, 2, 10, QChar('0'));
            };
        bar->GetTimeLabel()->setText(
            QString("%1 / %2").arg(msToTimeStr(pos_ms)).arg(msToTimeStr(dur_ms)));
        });

    // 音量控制
    QObject::connect(bar->GetVolSlider(), &QSlider::valueChanged, [this](int vol) {
        player_->SetVolume(vol / 100.0);
        });

    // 退出按钮绑定 清理d3d设备资源
    QObject::connect(main_window_, &MainWindow::SigRequestClose, [this]() {
        if (player_)
        {
            player_->Close();
            delete player_;
            player_ = nullptr;
        }
        });
}

// ---- 启动进度轮询（每 200ms 更新一次） ----
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

    // 更新进度条（不触发 sliderMoved 信号）
    bar->GetSeekBar()->blockSignals(true);
    bar->GetSeekBar()->setValue(static_cast<int>(pos_ms));
    bar->GetSeekBar()->blockSignals(false);

    // 更新时间标签：当前时间 / 总时长
    bar->GetTimeLabel()->setText(
        QString("%1 / %2").arg(msToTimeStr(pos_ms)).arg(msToTimeStr(dur_ms)));
}

void PlayerApp::OpenFile(const QString& path)
{
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