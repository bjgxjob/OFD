#ifndef OFD_GUI_MAIN_WINDOW_H
#define OFD_GUI_MAIN_WINDOW_H

#include <QMainWindow>

class QComboBox;
class QPlainTextEdit;
class QPushButton;
class QTextEdit;
class QTreeWidget;
class QTreeWidgetItem;

#include "ofd_facade.h"

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);

private slots:
    void onNewDocument();
    void onOpenDocument();
    void onSaveDocumentAs();
    void onConvertPdfToOfd();
    void onConvertOfdToPdf();
    void onAddPage();
    void onApplyPageText();
    void onPageSelectionChanged();

private:
    void buildUi();
    void buildMenu();
    void refreshPageTree();
    int selectedPageIndex() const;
    void logMessage(const QString& message);

    OfdFacade facade_;
    QString current_path_;

    QTreeWidget* page_tree_ = nullptr;
    QTextEdit* page_text_edit_ = nullptr;
    QPlainTextEdit* log_edit_ = nullptr;
    QComboBox* convert_mode_combo_ = nullptr;
    QPushButton* apply_button_ = nullptr;
};

#endif

