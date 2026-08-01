#include "MainWindow.h"
#include "TitleBar.h"
#include "LaunchPage.h"
#include "PlayerPage.h"
#include "Client/StreamUI/ConnectDialog.h"
#include "Client/StreamUI/StreamWindow.h"
#include "Client/ServerUI/ServerPanel.h"
#include "App/PlayerApp.h"
#include <QVBoxLayout>
#include <QKeyEvent>
#include <QCloseEvent>
#include <QApplication>
#include <QDebug>

MainWindow::MainWindow(PlayerApp* app, QWidget* parent)
    : QWidget(parent), app_(app)
{
    setWindowFlags(Qt::FramelessWindowHint);

    title_bar_ = new TitleBar("Player", this);
    stack_ = new QStackedWidget(this);

    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(title_bar_, 0);
    layout->addWidget(stack_, 1);

    CreatePages();
    SetupTitleBar();

    stack_->setCurrentWidget(launch_page_);
    resize(1200, 800);
}

MainWindow::~MainWindow()
{
    CleanupPages();
}

// ================================================================
// ============== 页面创建 ==============
// ================================================================

void MainWindow::CreatePages()
{
    // [0] 启动页
    launch_page_ = new LaunchPage(this);
    stack_->addWidget(launch_page_);

    QObject::connect(launch_page_, &LaunchPage::SigPlayerMode,    this, &MainWindow::SwitchToPlayer);
    QObject::connect(launch_page_, &LaunchPage::SigControllerMode,this, &MainWindow::SwitchToController);
    QObject::connect(launch_page_, &LaunchPage::SigServerMode,    this, &MainWindow::SwitchToServer);
}

void MainWindow::CleanupPages()
{
    // 内联清理逻辑，避免派生类指针到基类引用转换问题
    auto remove = [this](QWidget* w) {
        if (w) { stack_->removeWidget(w); delete w; }
    };
    remove(player_page_);      player_page_ = nullptr;
    remove(connect_dialog_);   connect_dialog_ = nullptr;
    remove(stream_window_);    stream_window_ = nullptr;
    remove(server_panel_);     server_panel_ = nullptr;
}

// ================================================================
// ============== 标题栏 ==============
// ================================================================

void MainWindow::SetupTitleBar()
{
    QObject::connect(title_bar_, &TitleBar::SigMinClicked,  this, &QWidget::showMinimized);
    QObject::connect(title_bar_, &TitleBar::SigMaxClicked,  this, &MainWindow::ToggleFullScreen);
    QObject::connect(title_bar_, &TitleBar::SigCloseClicked, this, &MainWindow::OnClose);
}

void MainWindow::UpdateTitleBar()
{
    QWidget* current = stack_->currentWidget();

    if (current == launch_page_)
    {
        title_bar_->SetTitle("Player");
        title_bar_->ShowBackButton(false);
    }
    else if (current == player_page_)
    {
        title_bar_->SetTitle("播放器");
        title_bar_->ShowBackButton(true);
    }
    else if (current == connect_dialog_)
    {
        title_bar_->SetTitle("远程控制");
        title_bar_->ShowBackButton(true);
    }
    else if (current == stream_window_)
    {
        title_bar_->SetTitle("远程桌面");
        title_bar_->ShowBackButton(true);
    }
    else if (current == server_panel_)
    {
        title_bar_->SetTitle("被控端");
        title_bar_->ShowBackButton(true);
    }

    // 返回按钮 → 启动页
    QObject::disconnect(title_bar_, &TitleBar::SigBackClicked, nullptr, nullptr);
    QObject::connect(title_bar_, &TitleBar::SigBackClicked, this, &MainWindow::SwitchToLaunch);
}

// ================================================================
// ============== 页面切换 ==============
// ================================================================

void MainWindow::SwitchToLaunch()
{
    CleanupPages();
    app_->DestroyPlayer();
    stack_->setCurrentWidget(launch_page_);
    UpdateTitleBar();
}

void MainWindow::SwitchToPlayer()
{
    CleanupPages();
    app_->DestroyPlayer();

    player_page_ = new PlayerPage(this);
    stack_->addWidget(player_page_);
    stack_->setCurrentWidget(player_page_);
    UpdateTitleBar();

    // 让 PlayerApp 接管播放器引擎 + 信号绑定
    app_->StartPlayer();
}

void MainWindow::SwitchToController()
{
    CleanupPages();

    connect_dialog_ = new ConnectDialog(this);
    stack_->addWidget(connect_dialog_);
    stack_->setCurrentWidget(connect_dialog_);
    UpdateTitleBar();

    QObject::connect(connect_dialog_, &ConnectDialog::SigConnect,
                     this, &MainWindow::OnConnect);
    QObject::connect(connect_dialog_, &ConnectDialog::SigBack,
                     this, &MainWindow::SwitchToLaunch);
}

void MainWindow::SwitchToServer()
{
    CleanupPages();

    server_panel_ = new ServerPanel(this);
    stack_->addWidget(server_panel_);
    stack_->setCurrentWidget(server_panel_);
    UpdateTitleBar();

    QObject::connect(server_panel_, &ServerPanel::SigBack,
                     this, &MainWindow::SwitchToLaunch);
}

void MainWindow::SwitchToStream(StreamWindow* sw)
{
    if (!sw) return;

    // 移除连接对话框
    if (connect_dialog_)
    {
        stack_->removeWidget(connect_dialog_);
        delete connect_dialog_;
        connect_dialog_ = nullptr;
    }

    stream_window_ = sw;
    stream_window_->SetEmbedded(true);                          // 隐藏内部 TitleBar
    stack_->addWidget(stream_window_);
    stack_->setCurrentWidget(stream_window_);
    UpdateTitleBar();

    QObject::connect(stream_window_, &StreamWindow::SigRequestClose,
                     this, &MainWindow::SwitchToLaunch);
}

// ================================================================
// ============== 控制端连接 ==============
// ================================================================

void MainWindow::OnConnect(const QString& ip, uint16_t port,
                           uint16_t ctrl_port, int fps)
{
    auto* sw = new StreamWindow(this);
    sw->SetVideoRect(0, 0, 1100, 720);

    SwitchToStream(sw);

    // 交给 PlayerApp 管理串流引擎
    app_->StartStream(ip, port, ctrl_port, fps);

    // 延迟启动输入转发（等解码器就绪后再连控制信道，500ms 足够）
    QTimer* input_timer = new QTimer(this);
    QObject::connect(input_timer, &QTimer::timeout, this,
        [this, input_timer, ip, ctrl_port]()
        {
            input_timer->stop();
            input_timer->deleteLater();
            app_->OnStreamReady(ip, ctrl_port);
        });
    input_timer->start(500);
}

// ================================================================
// ============== 窗口控制 ==============
// ================================================================

void MainWindow::ToggleFullScreen()
{
    fullscreen_ = !fullscreen_;
    if (fullscreen_)
        showFullScreen();
    else
        showNormal();
}

void MainWindow::keyPressEvent(QKeyEvent* event)
{
    if (event->key() == Qt::Key_F || event->key() == Qt::Key_F11)
        ToggleFullScreen();
    else if (event->key() == Qt::Key_Escape && fullscreen_)
        ToggleFullScreen();

    QWidget::keyPressEvent(event);
}

void MainWindow::OnClose()
{
    CleanupPages();
    app_->DestroyPlayer();
    QApplication::quit();
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    // 确保用户通过 Alt+F4 / 系统菜单关闭时也走完整清理
    if (!event->spontaneous()) {
        // 来自 OnClose() 的 close()，已经清理过了，直接接受
        event->accept();
        return;
    }

    // 用户主动关闭（Alt+F4），先做清理
    CleanupPages();
    app_->DestroyPlayer();
    QApplication::quit();
    event->accept();
}

void MainWindow::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
}
