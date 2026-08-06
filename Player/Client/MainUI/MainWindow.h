#pragma once

#include <QWidget>
#include <QStackedWidget>

class TitleBar;
class LaunchPage;
class PlayerPage;
class ConnectDialog;
class StreamWindow;
class ServerPanel;
class PlayerApp;

// 主窗口（第七阶段重构：UI 根节点）
// 持有 QStackedWidget + TitleBar，管理所有页面导航
// 不直接持有 FFmpegPlayer，播放逻辑由 PlayerApp 通过信号绑定实现
class MainWindow : public QWidget
{
    Q_OBJECT

public:
    MainWindow(PlayerApp* app, QWidget* parent = nullptr);
    ~MainWindow();

    // 页面切换
    void SwitchToLaunch();
    void SwitchToPlayer();
    void SwitchToController();
    void SwitchToServer();
    void SwitchToStream(StreamWindow* sw);                                  // 连接成功后才进入

    // 暴露给 PlayerApp
    PlayerPage*    GetPlayerPage()  const { return player_page_; }
    StreamWindow*  GetStreamWindow() const { return stream_window_; }
    ServerPanel*   GetServerPanel() const { return server_panel_; }

protected:
    void keyPressEvent(QKeyEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void closeEvent(QCloseEvent* event) override;                           // 窗口关闭 → 清理 PlayerApp + 退出

private slots:
    void OnConnect(uint16_t port, uint16_t ctrl_port, int fps);
    void OnClose();                                                         // 关闭窗口 → 完整清理
    void ToggleFullScreen();

private:
    void SetupTitleBar();                                                   // 标题栏信号绑定
    void CreatePages();                                                     // 创建所有页面并加入 QStackedWidget
    void CleanupPages();                                                    // 清理非启动页的所有页面
    void UpdateTitleBar();                                                  // 根据当前页更新标题和返回按钮

    PlayerApp* app_{nullptr};

    TitleBar*       title_bar_{nullptr};
    QStackedWidget* stack_{nullptr};

    // 页面（按 QStackedWidget 索引）
    LaunchPage*     launch_page_{nullptr};                                  // [0]
    PlayerPage*     player_page_{nullptr};                                  // [1]
    ConnectDialog*  connect_dialog_{nullptr};                               // [2]
    StreamWindow*   stream_window_{nullptr};                                // [3]
    ServerPanel*    server_panel_{nullptr};                                 // [4]

    bool fullscreen_{false};
};