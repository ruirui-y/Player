#include "FileBrowser.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFileDialog>
#include <QHeaderView>
#include <QFileInfo>

// 支持的视频扩展名
static const QStringList VIDEO_FILTERS =
{
    "*.mp4", "*.mkv", "*.avi", "*.mov", "*.flv",
    "*.wmv", "*.ts",  "*.webm","*.m4v", "*.3gp",
    "*.mpeg", "*.mpg", "*.rmvb", "*.vob"
};

FileBrowser::FileBrowser(QWidget* parent)
    : QWidget(parent)
{
    setObjectName("FileBrowser");

    // ---- 顶部：选择文件夹按钮 + 路径标签 ----
    select_folder_btn_ = new QPushButton(tr("选择文件夹"), this);
    select_folder_btn_->setObjectName("SelectFolderBtn");

    path_label_ = new QLabel(tr("未选择文件夹"), this);
    path_label_->setObjectName("PathLabel");
    path_label_->setWordWrap(true);

    QHBoxLayout* top_layout = new QHBoxLayout();
    top_layout->setContentsMargins(0, 0, 0, 4);
    top_layout->addWidget(select_folder_btn_);
    top_layout->addWidget(path_label_, 1);

    // ---- 文件系统模型 ----
    file_model_ = new QFileSystemModel(this);
    file_model_->setNameFilters(VIDEO_FILTERS);
    file_model_->setNameFilterDisables(false);                                              // 非视频文件直接隐藏
    file_model_->setOption(QFileSystemModel::DontWatchForChanges, true);

    // ---- 树形视图 ----
    tree_view_ = new QTreeView(this);
    tree_view_->setObjectName("FileTree");
    tree_view_->setModel(file_model_);
    tree_view_->setDragEnabled(true);                                                       // 支持拖拽
    tree_view_->setAnimated(true);
    tree_view_->setSortingEnabled(true);
    tree_view_->setRootIndex(file_model_->setRootPath(""));                                 // 显示整个磁盘
    tree_view_->header()->setStretchLastSection(true);
    tree_view_->header()->setSectionResizeMode(QHeaderView::ResizeToContents);
    tree_view_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    tree_view_->hideColumn(2);                                                              // 隐藏 "类型"
    tree_view_->hideColumn(3);                                                              // 隐藏 "修改日期"

    // 双击文件 → 播放
    QObject::connect(tree_view_, &QTreeView::doubleClicked,
        this, &FileBrowser::OnDoubleClicked);

    // 选择文件夹按钮
    QObject::connect(select_folder_btn_, &QPushButton::clicked,
        this, &FileBrowser::OnSelectFolder);

    // ---- 整体布局 ----
    QVBoxLayout* main_layout = new QVBoxLayout(this);
    main_layout->setContentsMargins(6, 6, 6, 6);
    main_layout->setSpacing(4);
    main_layout->addLayout(top_layout);
    main_layout->addWidget(tree_view_, 1);

    // 允许用户通过 QSplitter 拉伸宽度
    setMinimumWidth(200);
    setMaximumWidth(600);
}

void FileBrowser::SetRootPath(const QString& path)
{
    QModelIndex idx = file_model_->setRootPath(path);
    tree_view_->setRootIndex(idx);
    path_label_->setText(path);
}

void FileBrowser::OnDoubleClicked(const QModelIndex& index)
{
    if (!index.isValid()) return;
    if (file_model_->isDir(index)) return;           // 只处理文件

    QString file_path = file_model_->filePath(index);
    emit SigFileSelected(file_path);
}

void FileBrowser::OnSelectFolder()
{
    QString dir = QFileDialog::getExistingDirectory(this,
        u8"选择视频文件夹", QString(),
        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
    if (dir.isEmpty()) return;

    SetRootPath(dir);
}
