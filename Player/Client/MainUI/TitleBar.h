#ifndef TITLEBAR_H
#define TITLEBAR_H

#include <QWidget>
#include <QLabel>
#include <QPushButton>

class TitleBar : public QWidget
{
    Q_OBJECT

public:
    explicit TitleBar(const QString& title, QWidget* parent = nullptr);

    void SetTitle(const QString& title);
    void ShowBackButton(bool show);             // 显示/隐藏返回按钮

signals:
    void SigMinClicked();
    void SigMaxClicked();
    void SigCloseClicked();
    void SigBackClicked();                      // 返回按钮

protected:
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;

private:
    QLabel*      title_label_{nullptr};
    QPushButton* back_btn_{nullptr};            // 返回按钮
    QPushButton* min_btn_{nullptr};
    QPushButton* max_btn_{nullptr};
    QPushButton* close_btn_{nullptr};
    QPoint       drag_start_pos_;
};

#endif // TITLEBAR_H
