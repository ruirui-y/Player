#ifndef LAUNCHPAGE_H
#define LAUNCHPAGE_H

#include <QWidget>
#include <QPushButton>
#include <QLabel>

// 启动页：三个大按钮选择模式
// 信号连接到 PlayerApp 的模式切换槽函数
class LaunchPage : public QWidget
{
    Q_OBJECT

public:
    explicit LaunchPage(QWidget* parent = nullptr);

signals:
    void SigPlayerMode();           // 点击"播放视频"
    void SigControllerMode();       // 点击"远程控制"
    void SigServerMode();           // 点击"被控端"

private:
    QPushButton* CreateModeButton(const QString& emoji, const QString& title,
                                  const QString& desc);
    QPushButton* btn_player_;
    QPushButton* btn_controller_;
    QPushButton* btn_server_;
    QLabel* version_label_;
};

#endif // LAUNCHPAGE_H
