#include "StreamWindow.h"
#include "StreamVideoWidget.h"
#include "Client/MainUI/TitleBar.h"

#include <QVBoxLayout>
#include <QApplication>
#include <QKeyEvent>
#include <QCursor>
#include <QDebug>

// ---- 构造 ----
StreamWindow::StreamWindow(QWidget* parent)
    : QWidget(parent)
{
    setWindowFlags(Qt::FramelessWindowHint);

    // ---- 创建子控件 ----
    title_bar_ = new TitleBar("Stream", this);
    video_widget_ = new StreamVideoWidget(this);

    // ---- 底部状态栏 ----
    status_label_ = new QLabel(this);
    status_label_->setStyleSheet(
        "QLabel {"
        "  background-color: rgba(20, 20, 20, 255);"
        "  color: #aaa;"
        "  padding: 4px 12px;"
        "  font-size: 12px;"
        "}");
    status_label_->setText(u8"等待连接...");
    status_label_->setFixedHeight(24);

    // ---- 布局：标题栏 + 视频区 + 状态栏 ----
    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(title_bar_, 0);
    layout->addWidget(video_widget_, 1);
    layout->addWidget(status_label_, 0);

    // ---- 窗口按钮绑定 ----
    QObject::connect(title_bar_, &TitleBar::SigMinClicked,
        this, &QWidget::showMinimized);
    QObject::connect(title_bar_, &TitleBar::SigMaxClicked,
        this, &StreamWindow::ToggleFullScreen);
    QObject::connect(title_bar_, &TitleBar::SigCloseClicked, this, [this]()
        {
            emit SigRequestClose();
            QApplication::quit();
        });

    // ---- 鼠标闲置定时器（全屏时自动隐藏标题栏和状态栏） ----
    mouse_idle_timer_ = new QTimer(this);
    mouse_idle_timer_->setSingleShot(true);
    QObject::connect(mouse_idle_timer_, &QTimer::timeout,
        this, &StreamWindow::OnMouseIdle);

    // ---- 事件过滤器 ----
    installEventFilter(this);
    video_widget_->installEventFilter(this);
    title_bar_->installEventFilter(this);

    // ---- 启用鼠标跟踪 ----
    setMouseTracking(true);
    video_widget_->setMouseTracking(true);
    title_bar_->setMouseTracking(true);
}

// ---- 析构 ----
StreamWindow::~StreamWindow()
{
}

// ---- 设置窗口位置和大小 ----
void StreamWindow::SetVideoRect(int x, int y, int w, int h)
{
    setGeometry(x, y, w, h);
}

// ---- 获取 D3D11 用的 HWND ----
HWND StreamWindow::GetVideoHwnd() const
{
    return video_widget_ ? video_widget_->GetVideoHwnd() : nullptr;
}

// ---- 更新底部状态栏 ----
void StreamWindow::SetStatusText(const QString& text)
{
    if (status_label_)
        status_label_->setText(text);
}

// ---- 全屏/窗口切换 ----
void StreamWindow::ToggleFullScreen()
{
    fullscreen_ = !fullscreen_;

    if (fullscreen_)
    {
        title_bar_->hide();
        status_label_->hide();
        showFullScreen();
    }
    else
    {
        title_bar_->show();
        status_label_->show();
        showNormal();
    }
}

// ---- 事件过滤器：全屏时鼠标移动恢复标题栏 ----
bool StreamWindow::eventFilter(QObject* obj, QEvent* event)
{
    if (fullscreen_ && event->type() == QEvent::MouseMove)
    {
        if (obj == title_bar_)
        {
            mouse_idle_timer_->stop();
            title_bar_->show();
        }
        else
        {
            title_bar_->show();
            status_label_->show();
            mouse_idle_timer_->start(2000);
        }
    }
    return QWidget::eventFilter(obj, event);
}

// ---- 鼠标闲置：全屏时隐藏标题栏和状态栏 ----
void StreamWindow::OnMouseIdle()
{
    if (!fullscreen_) return;

    QPoint cursor_pos = mapFromGlobal(QCursor::pos());
    bool on_titlebar = title_bar_->geometry().contains(cursor_pos);

    if (!on_titlebar)
    {
        title_bar_->hide();
        status_label_->hide();
    }
}

// ---- 键盘事件：F 切换全屏，ESC 退出全屏 ----
void StreamWindow::keyPressEvent(QKeyEvent* event)
{
    if (event->key() == Qt::Key_F && !event->isAutoRepeat())
        ToggleFullScreen();
    if (event->key() == Qt::Key_Escape && fullscreen_)
        ToggleFullScreen();
    QWidget::keyPressEvent(event);
}
