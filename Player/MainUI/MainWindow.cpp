#include "MainWindow.h"
#include "TitleBar.h"
#include "VideoWidget.h"
#include "ControlBar.h"
#include <QVBoxLayout>
#include <QApplication>
#include <QKeyEvent>

MainWindow::MainWindow(QWidget* parent)
    : QWidget(parent)
{
    setWindowFlags(Qt::FramelessWindowHint);
    setStyleSheet("background-color: black;");

    // 主布局：TitleBar + VideoWidget + ControlBar
    title_bar_ = new TitleBar("Player", this);
    video_widget_ = new VideoWidget(this);
    control_bar_ = new ControlBar(this);

    QVBoxLayout* main_layout = new QVBoxLayout(this);
    main_layout->setContentsMargins(0, 0, 0, 0);
    main_layout->setSpacing(0);
    main_layout->addWidget(title_bar_, 0);
    main_layout->addWidget(video_widget_, 1);
    main_layout->addWidget(control_bar_, 0);
     
    // 窗口按钮绑定
    QObject::connect(title_bar_, &TitleBar::SigMinClicked, this, &QWidget::showMinimized);
    QObject::connect(title_bar_, &TitleBar::SigMaxClicked, this, &MainWindow::ToggleFullScreen);
    QObject::connect(title_bar_, &TitleBar::SigCloseClicked, this, [this]() 
        {
            // 先关闭播放器，确保 D3D11 资源在 Qt 析构窗口前释放
            emit SigRequestClose();
            QApplication::quit();
        });

    // 鼠标闲置定时器
    mouse_idle_timer_ = new QTimer(this);
    mouse_idle_timer_->setSingleShot(true);
    QObject::connect(mouse_idle_timer_, &QTimer::timeout, this, &MainWindow::OnMouseIdle);

    // 事件过滤器
    installEventFilter(this);
    video_widget_->installEventFilter(this);
    title_bar_->installEventFilter(this);
    control_bar_->installEventFilter(this);

    // 移动时触发
    setMouseTracking(true);
    video_widget_->setMouseTracking(true);
    title_bar_->setMouseTracking(true);
    control_bar_->setMouseTracking(true);
}

MainWindow::~MainWindow() {}

void MainWindow::SetVideoRect(int x, int y, int w, int h)
{
    setGeometry(x, y, w, h);
}

HWND MainWindow::GetVideoHwnd() const
{
    return video_widget_ ? video_widget_->GetVideoHwnd() : nullptr;
}

void MainWindow::OnFrameReady(const QImage& image)
{
    if (video_widget_)
        video_widget_->OnFrameReady(image);
}

void MainWindow::ToggleFullScreen()
{
    fullscreen_ = !fullscreen_;

    if (fullscreen_)
    {
        title_bar_->hide();
        control_bar_->hide();
        showFullScreen();
    }
    else
    {
        title_bar_->show();
        control_bar_->show();
        showNormal();
    }
}

bool MainWindow::eventFilter(QObject* obj, QEvent* event)
{
    if (fullscreen_ && event->type() == QEvent::MouseMove)
    {
        // 鼠标在 TitleBar 或 ControlBar 上移动时，保持显示，不启动隐藏计时器
        if (obj == title_bar_ || obj == control_bar_)
        {
            mouse_idle_timer_->stop();
            title_bar_->show();
            control_bar_->show();
        }
        else
        {
            // 鼠标在视频区域移动时，显示 + 启动 2 秒隐藏
            title_bar_->show();
            control_bar_->show();
            mouse_idle_timer_->start(2000);
        }
    }
    return QWidget::eventFilter(obj, event);
}

void MainWindow::OnMouseIdle()
{
    if (!fullscreen_) return;

    // 获取鼠标当前在窗口内的位置
    QPoint cursor_pos = mapFromGlobal(QCursor::pos());

    // 判断鼠标是否在 TitleBar 或 ControlBar 区域内
    bool on_titlebar = title_bar_->geometry().contains(cursor_pos);
    bool on_control = control_bar_->geometry().contains(cursor_pos);

    // 只有在视频区域时才隐藏，标题栏和控制栏上不隐藏
    if (!on_titlebar && !on_control)
    {
        title_bar_->hide();
        control_bar_->hide();
    }
}

void MainWindow::keyPressEvent(QKeyEvent* event)
{
    if (event->key() == Qt::Key_F && !event->isAutoRepeat())
        ToggleFullScreen();
    if (event->key() == Qt::Key_Escape && fullscreen_)
        ToggleFullScreen();
    QWidget::keyPressEvent(event);
}

void MainWindow::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
}