#ifndef FILEBROWSER_H
#define FILEBROWSER_H

#include <QWidget>
#include <QTreeView>
#include <QFileSystemModel>
#include <QPushButton>
#include <QLabel>

class FileBrowser : public QWidget
{
    Q_OBJECT

public:
    explicit FileBrowser(QWidget* parent = nullptr);

    void SetRootPath(const QString& path);              // 设置浏览目录

signals:
    void SigFileSelected(const QString& file_path);     // 用户双击视频文件

private slots:
    void OnDoubleClicked(const QModelIndex& index);     // 双击播放
    void OnSelectFolder();                              // 选择文件夹按钮

private:
    QTreeView* tree_view_{ nullptr };
    QFileSystemModel* file_model_{ nullptr };
    QPushButton* select_folder_btn_{ nullptr };
    QLabel* path_label_{ nullptr };
};

#endif // FILEBROWSER_H
