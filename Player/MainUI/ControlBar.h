#ifndef CONTROLBAR_H
#define CONTROLBAR_H

#include <QWidget>
#include <QPushButton>
#include <QSlider>
#include <QLabel>

// 底部控制栏
class ControlBar : public QWidget
{
    Q_OBJECT

public:
    explicit ControlBar(QWidget* parent = nullptr);

    QPushButton* GetPlayBtn()     const { return play_btn_; }
    QSlider* GetSeekBar()     const { return seek_bar_; }
    QLabel* GetTimeLabel()   const { return time_label_; }
    QSlider* GetVolSlider()   const { return vol_slider_; }
    QPushButton* GetFolderBtn()   const { return folder_btn_; }

signals:
    void SigSeekRequested(int pos_ms);
    void SigToggleFileBrowser();                        // 切换文件浏览器

protected:
    bool eventFilter(QObject* obj, QEvent* event) override;

private:
    QPushButton* play_btn_{ nullptr };
    QSlider* seek_bar_{ nullptr };
    QLabel* time_label_{ nullptr };
    QSlider* vol_slider_{ nullptr };
    QPushButton* folder_btn_{ nullptr };                // 文件浏览器切换按钮
};

#endif // CONTROLBAR_H
