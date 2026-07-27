#include "MainWindow.h"
#include "TitleBar.h"
#include "VideoWidget.h"
#include "ControlBar.h"
#include "FileBrowser.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QApplication>
#include <QKeyEvent>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QFileInfo>
#include <QDebug>

MainWindow::MainWindow(QWidget* parent)
    : QWidget(parent)
{
    setWindowFlags(Qt::FramelessWindowHint);

    // ---- 创建子控件 ----
    title_bar_ = new TitleBar("Player", this);
    video_widget_ = new VideoWidget(this);
    control_bar_ = new ControlBar(this);
    file_browser_ = new FileBrowser(this);
    file_browser_->hide();                                          // 默认隐藏

    // ---- 左侧：标题栏 + 视频区域 + 控制栏 ----
    QVBoxLayout* left_layout = new QVBoxLayout();
    left_layout->setContentsMargins(0, 0, 0, 0);
    left_layout->setSpacing(0);
    left_layout->addWidget(title_bar_, 0);
    left_layout->addWidget(video_widget_, 1);
    left_layout->addWidget(control_bar_, 0);

    // ---- 左侧容器（把 left_layout 塞进一个 QWidget） ----
    QWidget* left_container = new QWidget(this);
    left_container->setLayout(left_layout);

    // ---- QSplitter：左侧容器 + 文件浏览器 ----
    splitter_ = new QSplitter(Qt::Horizontal, this);
    splitter_->addWidget(left_container);
    splitter_->addWidget(file_browser_);
    splitter_->setStretchFactor(0, 1);                              // 视频区域拉伸填满
    splitter_->setStretchFactor(1, 0);                              // 文件浏览器不拉伸
    splitter_->setHandleWidth(3);                                   // 分割线宽度
    splitter_->setChildrenCollapsible(false);                       // 不允许完全折叠

    // ---- 主布局 ----
    QVBoxLayout* main_layout = new QVBoxLayout(this);
    main_layout->setContentsMargins(0, 0, 0, 0);
    main_layout->setSpacing(0);
    main_layout->addWidget(splitter_, 1);

    // ---- 窗口按钮绑定 ----
    QObject::connect(title_bar_, &TitleBar::SigMinClicked,
        this, &QWidget::showMinimized);
    QObject::connect(title_bar_, &TitleBar::SigMaxClicked,
        this, &MainWindow::ToggleFullScreen);
    QObject::connect(title_bar_, &TitleBar::SigCloseClicked, this, [this]()
        {
            emit SigRequestClose();
            QApplication::quit();
        });

    // ---- 鼠标闲置定时器 ----
    mouse_idle_timer_ = new QTimer(this);
    mouse_idle_timer_->setSingleShot(true);
    QObject::connect(mouse_idle_timer_, &QTimer::timeout,
        this, &MainWindow::OnMouseIdle);

    // ---- 事件过滤器 ----
    installEventFilter(this);
    video_widget_->installEventFilter(this);
    title_bar_->installEventFilter(this);
    control_bar_->installEventFilter(this);

    // ---- 启用鼠标跟踪 ----
    setMouseTracking(true);
    video_widget_->setMouseTracking(true);
    title_bar_->setMouseTracking(true);
    control_bar_->setMouseTracking(true);

    // ---- 启用拖放（从外部文件管理器拖入） ----
    setAcceptDrops(true);
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

void MainWindow::ToggleFileBrowser()
{
    if (file_browser_)
        file_browser_->setVisible(!file_browser_->isVisible());
}

void MainWindow::ToggleFullScreen()
{
    fullscreen_ = !fullscreen_;

    if (fullscreen_)
    {
        title_bar_->hide();
        control_bar_->hide();
        file_browser_->hide();
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
        if (obj == title_bar_ || obj == control_bar_)
        {
            mouse_idle_timer_->stop();
            title_bar_->show();
            control_bar_->show();
        }
        else
        {
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

    QPoint cursor_pos = mapFromGlobal(QCursor::pos());
    bool on_titlebar = title_bar_->geometry().contains(cursor_pos);
    bool on_control = control_bar_->geometry().contains(cursor_pos);

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

// ================================================================
// 拖放支持（从外部文件管理器拖拽视频文件到播放器窗口）
// ================================================================
void MainWindow::dragEnterEvent(QDragEnterEvent* event)
{
    if (!event->mimeData()->hasUrls())
    {
        QWidget::dragEnterEvent(event);
        return;
    }

    const auto& urls = event->mimeData()->urls();
    for (const auto& url : urls)
    {
        if (!url.isLocalFile()) continue;

        QString suffix = QFileInfo(url.toLocalFile()).suffix().toLower();
        static const QStringList VIDEO_EXTS =
        {
            "mp4", "mkv", "avi", "mov", "flv", "wmv",
            "ts", "webm", "m4v", "3gp", "mpeg", "mpg", "rmvb", "vob"
        };
        if (VIDEO_EXTS.contains(suffix))
        {
            event->acceptProposedAction();
            return;
        }
    }
    QWidget::dragEnterEvent(event);
}

void MainWindow::dropEvent(QDropEvent* event)
{
    if (!event->mimeData()->hasUrls())
    {
        QWidget::dropEvent(event);
        return;
    }

    const auto& urls = event->mimeData()->urls();
    for (const auto& url : urls)
    {
        if (url.isLocalFile())
        {
            QString path = url.toLocalFile();
            emit SigFileDropped(path);
            break;                  // 只处理第一个文件
        }
    }
    event->acceptProposedAction();
}
