// 必须在所有头文件之前定义，阻止 windows.h 拉入旧版 winsock.h（与 winsock2.h 冲突）
#define WIN32_LEAN_AND_MEAN

#include "StreamWindow.h"
#include "StreamVideoWidget.h"
#include "Client/MainUI/TitleBar.h"
#include "Common/Input/InputCollector.h"
#include "Common/Input/InputTransport.h"
#include "Common/Input/InputEvent.h"
#include "Common/LogManager.h"

#include <QVBoxLayout>
#include <QApplication>
#include <QKeyEvent>
#include <QCursor>
#include <QDebug>
#include <windows.h>

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

    // ---- 启用鼠标跟踪（即使不按按键也能收到 mouseMoveEvent） ----
    setMouseTracking(true);
    video_widget_->setMouseTracking(true);
    title_bar_->setMouseTracking(true);

    // ---- 设置焦点策略：窗口可以接收键盘事件 ----
    setFocusPolicy(Qt::StrongFocus);
}

// ---- 析构 ----
StreamWindow::~StreamWindow()
{
    StopInput();
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

// ================================================================
// ---- 全屏/窗口切换 ----
// 修复：全屏时用 raise() + activateWindow() 确保窗口在前台且有焦点
// ================================================================
void StreamWindow::ToggleFullScreen()
{
    fullscreen_ = !fullscreen_;

    if (fullscreen_)
    {
        // ---- 进入全屏：隐藏状态栏 ----
        status_label_->hide();
        showFullScreen();

        // 确保窗口有焦点，能接收键盘事件
        raise();
        activateWindow();
        setFocus();
    }
    else
    {
        // ---- 退出全屏：恢复标题栏和状态栏 ----
        title_bar_->show();
        status_label_->show();
        showNormal();

        raise();
        activateWindow();
        setFocus();
    }
}

// ================================================================
// ---- 键盘事件：F 切换全屏，ESC 退出全屏 ----
// 修复：全屏时确保窗口有焦点能收到键盘事件
// ================================================================
void StreamWindow::keyPressEvent(QKeyEvent* event)
{
    if (event->key() == Qt::Key_F && !event->isAutoRepeat())
    {
        ToggleFullScreen();
        return;
    }

    if (event->key() == Qt::Key_Escape && fullscreen_)
    {
        ToggleFullScreen();
        return;
    }

    QWidget::keyPressEvent(event);
}

// ================================================================
// ---- 第三阶段：启动输入采集 + TCP 控制信道 ----
// ================================================================
bool StreamWindow::StartInput(const char* server_ip, uint16_t ctrl_port)
{
    LogManager::Log("INFO", "[StreamWindow] 启动输入转发 → %s:%d", server_ip, ctrl_port);

    // ---- 第一步：创建 TCP 传输客户端并连接服务端 ----
    input_transport_ = new InputTransportClient();
    if (!input_transport_->Connect(server_ip, ctrl_port))
    {
        LogManager::Log("ERR", "[StreamWindow] 控制信道连接失败 %s:%d", server_ip, ctrl_port);
        delete input_transport_;
        input_transport_ = nullptr;
        return false;
    }

    LogManager::Log("INFO", "[StreamWindow] 控制信道连接成功");

    // ---- 第二步：创建输入采集器 ----
    input_collector_ = new InputCollector();

    // ---- 第三步：接收服务端显示器信息，传给采集器（多显示器坐标映射） ----
    input_collector_->SetMonitorInfo(input_transport_->GetMonitorInfo());

    // ---- 第四步：设置采集回调 → TCP 发送 ----
    input_collector_->OnInputEvent = [this](const InputMessage& msg)
    {
        if (input_transport_)
            input_transport_->Send(msg);
    };

    // ---- 第五步：启动采集（钩子需要消息循环，Qt 事件循环即可） ----
    if (!input_collector_->Start())
    {
        LogManager::Log("ERR", "[StreamWindow] 输入采集启动失败");
        delete input_collector_;
        input_collector_ = nullptr;
        delete input_transport_;
        input_transport_ = nullptr;
        return false;
    }

    SetStatusText(QString(u8"已连接  控制信道 %1:%2").arg(server_ip).arg(ctrl_port));
    LogManager::Log("INFO", "[StreamWindow] 输入转发已启动");

    // ---- 第六步：传递视频窗口 HWND 给采集器，用于绝对坐标映射 ----
    // 采集器根据光标在视频窗口内的位置，结合 ServerMonitorInfo 做多显示器映射
    input_collector_->SetVideoWidget(video_widget_->GetVideoHwnd());

    return true;
}

// ---- 停止输入转发 ----
void StreamWindow::StopInput()
{
    // ---- 移除事件过滤器 ----
    if (video_widget_)
        video_widget_->removeEventFilter(this);

    if (input_collector_)
    {
        input_collector_->Stop();
        delete input_collector_;
        input_collector_ = nullptr;
    }

    if (input_transport_)
    {
        input_transport_->Disconnect();
        delete input_transport_;
        input_transport_ = nullptr;
    }
}

// ---- 嵌入模式：隐藏内部 TitleBar，由 MainWindow 统一管理 ----
void StreamWindow::SetEmbedded(bool embedded)
{
    if (title_bar_)
        title_bar_->setVisible(!embedded);
}
