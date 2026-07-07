#pragma once

#include <QtWidgets/QMainWindow>
#include <QLabel>
#include <QImage>
#include "ui_Player.h"

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    MainWindow(QWidget* parent = nullptr);
    ~MainWindow();

    void SetVideoRect(int x, int y, int w, int h);              // 设置画面位置和尺寸

public slots:
    void OnFrameReady(const QImage& image);                     // 接收解码线程发来的一帧画面

private:
    Ui::MainWindowClass ui;
    QLabel* video_label_{ nullptr };                            // 用于显示视频画面的标签
};