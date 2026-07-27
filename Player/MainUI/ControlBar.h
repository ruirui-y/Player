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

    QPushButton* GetPlayBtn()    const { return play_btn_; }
    QSlider* GetSeekBar()    const { return seek_bar_; }
    QLabel* GetTimeLabel()  const { return time_label_; }
    QSlider* GetVolSlider()  const { return vol_slider_; }

signals:
    void SigSeekRequested(int pos_ms);                                      // 点击轨道时触发 seek

protected:
    bool eventFilter(QObject* obj, QEvent* event) override;                 // 拦截 seek_bar 鼠标事件

private:
    QPushButton* play_btn_{ nullptr };
    QSlider* seek_bar_{ nullptr };
    QLabel* time_label_{ nullptr };
    QSlider* vol_slider_{ nullptr };
};

#endif // CONTROLBAR_H