#include "main_window.h"

#include <QAction>
#include <QComboBox>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSplitter>
#include <QStatusBar>
#include <QTextEdit>
#include <QToolBar>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QWidget>

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    buildUi();
    buildMenu();
    setWindowTitle("ofd-gui");
    resize(1200, 760);
}

void MainWindow::buildUi() {
    auto* central = new QWidget(this);
    auto* mainLayout = new QVBoxLayout(central);
    auto* splitter = new QSplitter(Qt::Horizontal, central);

    page_tree_ = new QTreeWidget(splitter);
    page_tree_->setHeaderLabel("文档结构");
    connect(page_tree_, &QTreeWidget::itemSelectionChanged, this, &MainWindow::onPageSelectionChanged);

    auto* rightPanel = new QWidget(splitter);
    auto* rightLayout = new QVBoxLayout(rightPanel);

    auto* topRow = new QHBoxLayout();
    convert_mode_combo_ = new QComboBox(rightPanel);
    convert_mode_combo_->addItem("自动", 0);
    convert_mode_combo_->addItem("结构优先", 1);
    convert_mode_combo_->addItem("视觉优先", 2);
    topRow->addWidget(new QLabel("PDF->OFD 模式:", rightPanel));
    topRow->addWidget(convert_mode_combo_);
    topRow->addStretch();
    rightLayout->addLayout(topRow);

    page_text_edit_ = new QTextEdit(rightPanel);
    page_text_edit_->setPlaceholderText("页面文本（V1 先支持文本编辑）");
    rightLayout->addWidget(page_text_edit_, 1);

    auto* editButtons = new QHBoxLayout();
    auto* addPageBtn = new QPushButton("新增页面", rightPanel);
    apply_button_ = new QPushButton("应用到当前页", rightPanel);
    editButtons->addWidget(addPageBtn);
    editButtons->addWidget(apply_button_);
    editButtons->addStretch();
    rightLayout->addLayout(editButtons);
    connect(addPageBtn, &QPushButton::clicked, this, &MainWindow::onAddPage);
    connect(apply_button_, &QPushButton::clicked, this, &MainWindow::onApplyPageText);

    log_edit_ = new QPlainTextEdit(rightPanel);
    log_edit_->setReadOnly(true);
    log_edit_->setMaximumHeight(150);
    rightLayout->addWidget(log_edit_);

    splitter->addWidget(page_tree_);
    splitter->addWidget(rightPanel);
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({320, 860});

    mainLayout->addWidget(splitter);
    setCentralWidget(central);
}

void MainWindow::buildMenu() {
    QMenu* fileMenu = menuBar()->addMenu("文件");
    QAction* newAction = fileMenu->addAction("新建");
    QAction* openAction = fileMenu->addAction("打开...");
    QAction* saveAsAction = fileMenu->addAction("另存为...");
    fileMenu->addSeparator();
    QAction* quitAction = fileMenu->addAction("退出");

    QMenu* convertMenu = menuBar()->addMenu("转换");
    QAction* pdfToOfdAction = convertMenu->addAction("PDF -> OFD");
    QAction* ofdToPdfAction = convertMenu->addAction("OFD -> PDF");

    connect(newAction, &QAction::triggered, this, &MainWindow::onNewDocument);
    connect(openAction, &QAction::triggered, this, &MainWindow::onOpenDocument);
    connect(saveAsAction, &QAction::triggered, this, &MainWindow::onSaveDocumentAs);
    connect(pdfToOfdAction, &QAction::triggered, this, &MainWindow::onConvertPdfToOfd);
    connect(ofdToPdfAction, &QAction::triggered, this, &MainWindow::onConvertOfdToPdf);
    connect(quitAction, &QAction::triggered, this, &QWidget::close);

    QToolBar* toolbar = addToolBar("主工具栏");
    toolbar->addAction(newAction);
    toolbar->addAction(openAction);
    toolbar->addAction(saveAsAction);
    toolbar->addSeparator();
    toolbar->addAction(pdfToOfdAction);
    toolbar->addAction(ofdToPdfAction);
}

void MainWindow::onNewDocument() {
    bool ok = false;
    const QString docId = QInputDialog::getText(this, "新建 OFD", "DocID:", QLineEdit::Normal, "ofd-gui-doc", &ok);
    if (!ok) {
        return;
    }
    QString error;
    if (!facade_.createEmpty(docId, "ofd-gui", &error)) {
        QMessageBox::warning(this, "新建失败", error);
        return;
    }
    facade_.addPageText("", &error);
    current_path_.clear();
    refreshPageTree();
    logMessage("已新建文档: " + docId);
}

void MainWindow::onOpenDocument() {
    QString path = QFileDialog::getOpenFileName(this, "打开 OFD 文件", QString(), "OFD Files (*.ofd)");
    if (path.isEmpty()) {
        path = QFileDialog::getExistingDirectory(this, "打开 OFD 解压目录");
    }
    if (path.isEmpty()) {
        return;
    }
    QString error;
    if (!facade_.openPath(path, &error)) {
        QMessageBox::warning(this, "打开失败", error);
        return;
    }
    current_path_ = path;
    refreshPageTree();
    logMessage("已打开: " + path);
}

void MainWindow::onSaveDocumentAs() {
    QString path = QFileDialog::getSaveFileName(this, "另存为 OFD", current_path_, "OFD Files (*.ofd)");
    if (path.isEmpty()) {
        path = QFileDialog::getExistingDirectory(this, "保存为 OFD 解压目录");
    }
    if (path.isEmpty()) {
        return;
    }
    QString error;
    if (!facade_.savePath(path, &error)) {
        QMessageBox::warning(this, "保存失败", error);
        return;
    }
    current_path_ = path;
    statusBar()->showMessage("保存成功: " + path, 3000);
    logMessage("已保存: " + path);
}

void MainWindow::onConvertPdfToOfd() {
    const QString inputPdf = QFileDialog::getOpenFileName(this, "选择 PDF", QString(), "PDF Files (*.pdf)");
    if (inputPdf.isEmpty()) {
        return;
    }
    const QString outputOfd = QFileDialog::getSaveFileName(this, "输出 OFD", QString(), "OFD Files (*.ofd)");
    if (outputOfd.isEmpty()) {
        return;
    }
    QString error;
    const int mode = convert_mode_combo_->currentData().toInt();
    if (!facade_.convertPdfToOfd(inputPdf, outputOfd, mode, &error)) {
        QMessageBox::warning(this, "转换失败", error);
        return;
    }
    logMessage(QString("PDF->OFD 成功: %1 -> %2").arg(inputPdf, outputOfd));
    statusBar()->showMessage("PDF->OFD 成功", 3000);
}

void MainWindow::onConvertOfdToPdf() {
    const QString inputOfd = QFileDialog::getOpenFileName(this, "选择 OFD", QString(), "OFD Files (*.ofd)");
    if (inputOfd.isEmpty()) {
        return;
    }
    const QString outputPdf = QFileDialog::getSaveFileName(this, "输出 PDF", QString(), "PDF Files (*.pdf)");
    if (outputPdf.isEmpty()) {
        return;
    }
    QString error;
    if (!facade_.convertOfdToPdf(inputOfd, outputPdf, &error)) {
        QMessageBox::warning(this, "转换失败", error);
        return;
    }
    logMessage(QString("OFD->PDF 成功: %1 -> %2").arg(inputOfd, outputPdf));
    statusBar()->showMessage("OFD->PDF 成功", 3000);
}

void MainWindow::onAddPage() {
    QString error;
    if (!facade_.addPageText("", &error)) {
        QMessageBox::warning(this, "新增页面失败", error);
        return;
    }
    refreshPageTree();
    logMessage("已新增页面");
}

void MainWindow::onApplyPageText() {
    const int idx = selectedPageIndex();
    if (idx < 0) {
        QMessageBox::information(this, "提示", "请先选择页面");
        return;
    }
    QString error;
    if (!facade_.setPageText(idx, page_text_edit_->toPlainText(), &error)) {
        QMessageBox::warning(this, "保存页面失败", error);
        return;
    }
    logMessage(QString("已更新第 %1 页文本").arg(idx + 1));
}

void MainWindow::onPageSelectionChanged() {
    const int idx = selectedPageIndex();
    if (idx < 0) {
        page_text_edit_->clear();
        return;
    }
    QString error;
    const QString text = facade_.pageText(idx, &error);
    if (!error.isEmpty()) {
        logMessage("读取页面失败: " + error);
    }
    page_text_edit_->setPlainText(text);
}

void MainWindow::refreshPageTree() {
    page_tree_->clear();
    const int count = facade_.pageCount();
    for (int i = 0; i < count; ++i) {
        auto* item = new QTreeWidgetItem(page_tree_);
        item->setText(0, QString("Page %1").arg(i + 1));
        item->setData(0, Qt::UserRole, i);
    }
    if (count > 0) {
        page_tree_->setCurrentItem(page_tree_->topLevelItem(0));
    }
}

int MainWindow::selectedPageIndex() const {
    const auto items = page_tree_->selectedItems();
    if (items.isEmpty()) {
        return -1;
    }
    return items.first()->data(0, Qt::UserRole).toInt();
}

void MainWindow::logMessage(const QString& message) {
    log_edit_->appendPlainText(message);
}

