#include "PlayerApp.h"
#include "MainUI/MainWindow.h"
#include "MainUI/VideoWidget.h"
#include "MainUI/ControlBar.h"
#include "MainUI/FileBrowser.h"
#include "Core/FFmpegPlayer.h"

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