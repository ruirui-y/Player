#include "PlayerApp.h"
#include "Client/MainUI/MainWindow.h"
#include "Client/MainUI/PlayerPage.h"
#include "Client/MainUI/VideoWidget.h"
#include "Client/MainUI/ControlBar.h"
#include "Client/MainUI/FileBrowser.h"
#include "Client/StreamUI/StreamWindow.h"
#include "Client/StreamUI/StreamVideoWidget.h"
#include "Client/ServerUI/ServerPanel.h"
#include "Client/Core/FFmpegPlayer.h"
#include "Client/Core/SignalingClient.h"
#include <QApplication>
#include <QFile>
#include <QSysInfo>
#include <QSlider>
#include <QTimer>
#include <QFileDialog>
#include <QDebug>

static QString msToTimeStr(qint64 ms)
{
    int total_sec = static_cast<int>(ms / 1000);
    return QString("%1:%2")
        .arg(total_sec / 60, 2, 10, QChar('0'))
        .arg(total_sec % 60, 2, 10, QChar('0'));
}

PlayerApp::PlayerApp(QObject* parent) : QObject(parent) {}
PlayerApp::~PlayerApp() { DestroyPlayer(); }

// ================================================================
// ---- Init ----
// ================================================================

void PlayerApp::Init()
{
    LoadStyle();
    main_window_ = new MainWindow(this);
    main_window_->show();

    // ---- 第九阶段测试：自动连接信令服务器 ----
    signaling_ = new SignalingClient(this);

    QString server_host = "192.168.31.142";
    QString device_id  = QSysInfo::machineHostName();

    // 同机优先 127.0.0.1（Qt 5.14 QWebSocket 连本机公网 IP 偶发 UnsupportedSocketOperationError）
    if (device_id == "DESKTOP-IASOGT4")
        server_host = "127.0.0.1";

    qDebug() << "[Signal] 设备ID:" << device_id << " 服务器:" << server_host;
    signaling_->SetServer(server_host, 8080, 8081);

    signaling_->OnSignaling = [](const QJsonObject& msg) {
        qDebug() << "[Signal] 收到:" << msg;
    };

    signaling_->OnDeviceList = [](const QJsonArray& devices) {
        qDebug() << "[Signal] 设备列表:" << QJsonValue(devices);
        };

    signaling_->RegisterDevice(device_id, device_id);
    signaling_->StartHeartbeat(device_id, 30000);
    signaling_->QueryDevices();
}

void PlayerApp::LoadStyle()
{
    QFile qss("QSS/style.qss");

    QFileInfo fileInfo(qss);
    qDebug() << "[PlayerApp] 样式文件绝对路径:" << fileInfo.absoluteFilePath();

    if (qss.open(QFile::ReadOnly | QFile::Text))
    {
        qApp->setStyleSheet(qss.readAll());
        qDebug() << "[PlayerApp] 样式文件加载成功";
    }
    else
    {
        qDebug() << "[PlayerApp] 样式文件加载失败，使用默认样式";
    }
}

// ================================================================
// ============== 引擎管理 ==============
// ================================================================

void PlayerApp::DestroyPlayer()
{
    if (progress_timer_)
    {
        progress_timer_->stop();
        delete progress_timer_;
        progress_timer_ = nullptr;
    }
    if (player_)
    {
        player_->Close();
        delete player_;
        player_ = nullptr;
    }
}

void PlayerApp::StartPlayer(const QString& path)
{
    player_ = new FFmpegPlayer(this);
    PlayerPage* pp = main_window_->GetPlayerPage();

    player_->SetVideoHwnd(pp->GetVideoWidget()->GetVideoHwnd());

    BindPlayerSignals();

    if (!path.isEmpty())
    {
        player_->Stop();
        if (player_->OpenFile(path))
        {
            player_->Play();
            pp->GetControlBar()->GetPlayBtn()->setText("⏸");
            StartProgressTimer();
        }
    }
}

void PlayerApp::StartStream(uint16_t port, uint16_t ctrl_port, int fps)
{
    stream_ctrl_port_ = ctrl_port;

    player_ = new FFmpegPlayer(this);
    StreamWindow* sw = main_window_->GetStreamWindow();

    player_->SetVideoHwnd(sw->GetVideoHwnd());

    BindStreamSignals();

    sw->SetStatusText(QString(u8"等待连接... 端口 %1 @%2fps").arg(port).arg(fps));

    if (!player_->OpenStream(port, fps))
    {
        qDebug() << "[PlayerApp] 串流启动失败";
        sw->SetStatusText(u8"连接失败");
        return;
    }

    player_->Play();
    qDebug() << "[PlayerApp] 串流模式启动，端口" << port << "@" << fps << "fps";
}

void PlayerApp::OnStreamReady(const QString& ip, uint16_t ctrl_port)
{
    StreamWindow* sw = main_window_->GetStreamWindow();
    if (!sw || !player_) return;

    stream_ip_ = ip;
    if (!sw->StartInput(stream_ip_.toStdString().c_str(), ctrl_port))
    {
        sw->SetStatusText(u8"控制信道连接失败");
        return;
    }

    sw->SetStatusText(u8"已连接 — 控制信道就绪");
    player_->SetupStreamControl(sw->GetInputTransport());

    // ---- 第八阶段：启动 OSD 统计定时器 ----
    sw->StartStatsTimer(player_);
}

bool PlayerApp::HasSenderIP() const
{
    return player_ && !player_->GetSenderIP().empty();
}

// ================================================================
// ============== 播放器信号绑定 ==============
// ================================================================

void PlayerApp::BindPlayerSignals()
{
    PlayerPage* pp = main_window_->GetPlayerPage();
    ControlBar* bar = pp->GetControlBar();

    QObject::connect(player_, &FFmpegPlayer::SigFrameReady,
                     pp->GetVideoWidget(), &VideoWidget::OnFrameReady);

    QObject::connect(player_, &FFmpegPlayer::SigLoaded, this, [this, bar, pp](qint64 duration_ms) {
        qDebug("文件加载完成，时长: %lld ms", duration_ms);
        bar->GetSeekBar()->setRange(0, static_cast<int>(duration_ms));
        bar->GetTimeLabel()->setText(QString("00:00 / %1").arg(msToTimeStr(duration_ms)));
        });

    QObject::connect(player_, &FFmpegPlayer::SigFinished, this, [this, bar, pp]() {
        qDebug("播放结束");
        if (progress_timer_) progress_timer_->stop();
        bar->GetPlayBtn()->setText("▶");
        bar->GetSeekBar()->setValue(0);
        bar->GetTimeLabel()->setText("00:00 / 00:00");
        pp->GetVideoWidget()->ClearFrame();
        });

    QObject::connect(player_, &FFmpegPlayer::SigError, [](const QString& msg) {
        qDebug("错误: %s", qPrintable(msg));
        });

    QObject::connect(player_, &FFmpegPlayer::SigPlayState, [bar](const QString& state)
        {
            qDebug("状态变化: %s", qPrintable(state));
            if (state == "playing")
            {
                bar->GetPlayBtn()->setText("⏸");
                qDebug("播放中");
            }
            else if (state == "paused")
            {
                bar->GetPlayBtn()->setText("▶");
                qDebug("暂停中");
            }
            else if (state == "stopped")
            {
                bar->GetPlayBtn()->setText("▶");
                qDebug("停止");
            }
        });


    // 播放/暂停
    QObject::connect(bar->GetPlayBtn(), &QPushButton::clicked, this, [this, bar]() {
        if (!player_ || player_->GetDuration() <= 0)
        {
            QString path = QFileDialog::getOpenFileName(
                main_window_, u8"选择视频文件", QString(),
                u8"视频文件 (*.mp4 *.mkv *.avi *.mov *.flv *.wmv *.ts *.webm *.m4v "
                u8"*.3gp *.mpeg *.mpg *.rmvb *.vob);;所有文件 (*.*)");
            if (!path.isEmpty())
            {
                player_->Stop();
                if (player_->OpenFile(path))
                {
                    player_->Play();
                    bar->GetPlayBtn()->setText("⏸");
                    StartProgressTimer();
                }
            }
            return;
        }
        if (player_->IsPlaying())
        {
            if (player_->IsPaused()) { player_->Play(); bar->GetPlayBtn()->setText("⏸"); StartProgressTimer(); }
            else                    { player_->Pause(); bar->GetPlayBtn()->setText("▶"); }
        }
        else
        {
            player_->Play(); bar->GetPlayBtn()->setText("⏸"); StartProgressTimer();
        }
        });

    // Seek
    QObject::connect(bar->GetSeekBar(), &QSlider::sliderPressed, this, [this]() {
        if (progress_timer_) progress_timer_->stop(); });
    QObject::connect(bar->GetSeekBar(), &QSlider::sliderReleased, this, [this, bar]() {
        player_->Seek(bar->GetSeekBar()->value());
        if (player_->IsPlaying() && !player_->IsPaused()) StartProgressTimer();
        });
    QObject::connect(bar, &ControlBar::SigSeekRequested, this, [this](int pos_ms) {
        player_->Seek(pos_ms);
        if (player_->IsPlaying() && !player_->IsPaused()) StartProgressTimer();
        });
    QObject::connect(bar->GetSeekBar(), &QSlider::sliderMoved, this, [this, bar](int pos_ms) {
        qint64 dur = player_->GetDuration();
        auto ts = [](qint64 ms) -> QString {
            int s = static_cast<int>(ms / 1000);
            return QString("%1:%2").arg(s/60,2,10,QChar('0')).arg(s%60,2,10,QChar('0'));
        };
        bar->GetTimeLabel()->setText(QString("%1 / %2").arg(ts(pos_ms)).arg(ts(dur)));
        });

    // 音量
    QObject::connect(bar->GetVolSlider(), &QSlider::valueChanged, this, [this](int vol) {
        player_->SetVolume(vol / 100.0); });

    // 文件浏览器
    QObject::connect(bar, &ControlBar::SigToggleFileBrowser, pp, &PlayerPage::ToggleFileBrowser);
    QObject::connect(pp->GetFileBrowser(), &FileBrowser::SigFileSelected, this,
        [this](const QString& path) {
            player_->Stop();
            if (player_->OpenFile(path)) { player_->Play(); StartProgressTimer(); }
        });
}

// ================================================================
// ============== 控制端信号绑定 ==============
// ================================================================

void PlayerApp::BindStreamSignals()
{
    StreamWindow* sw = main_window_->GetStreamWindow();

    QObject::connect(player_, &FFmpegPlayer::SigFrameReady,
                     sw->GetVideoWidget(), &StreamVideoWidget::OnFrameReady);

    QObject::connect(player_, &FFmpegPlayer::SigLoaded, this, [sw](qint64) {
        sw->SetStatusText(u8"编解码就绪，等待视频数据..." ); });

    QObject::connect(player_, &FFmpegPlayer::SigError, this, [sw](const QString& msg) {
        sw->SetStatusText(u8"错误: " + msg); });

    // 首包到达 → 已知服务端 IP → 启动控制信道（替代 while+Sleep 轮询）
    QObject::connect(player_, &FFmpegPlayer::SigSenderIPReady, this,
                     [this](const QString& ip)
    {
        OnStreamReady(ip, stream_ctrl_port_);
    });
}

// ================================================================
// ============== 进度 ==============
// ================================================================

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
    if (!player_ || !main_window_ || !main_window_->GetPlayerPage()) return;

    qint64 pos_ms = player_->GetPosition();
    qint64 dur_ms = player_->GetDuration();
    ControlBar* bar = main_window_->GetPlayerPage()->GetControlBar();

    bar->GetSeekBar()->blockSignals(true);
    bar->GetSeekBar()->setValue(static_cast<int>(pos_ms));
    bar->GetSeekBar()->blockSignals(false);
    bar->GetTimeLabel()->setText(
        QString("%1 / %2").arg(msToTimeStr(pos_ms)).arg(msToTimeStr(dur_ms)));
}
