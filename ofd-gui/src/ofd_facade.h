#ifndef OFD_GUI_OFD_FACADE_H
#define OFD_GUI_OFD_FACADE_H

#include <QString>

#include "libofd/libofd.h"

class OfdFacade {
public:
    OfdFacade();
    ~OfdFacade();

    bool createEmpty(const QString& docId, const QString& creator, QString* error);
    bool openPath(const QString& path, QString* error);
    bool savePath(const QString& path, QString* error) const;

    int pageCount() const;
    QString pageText(int pageIndex, QString* error) const;
    bool setPageText(int pageIndex, const QString& text, QString* error);
    bool addPageText(const QString& text, QString* error);

    bool convertPdfToOfd(const QString& inputPdf, const QString& outputOfd, int mode, QString* error);
    bool convertOfdToPdf(const QString& inputOfd, const QString& outputPdf, QString* error);

private:
    libofd_handle_t* handle_ = nullptr;
    QString loaded_exploded_root_;

    static bool runShellCommand(
        const QString& program, const QStringList& arguments, const QString& workingDir, QString* error);
    static QString newTempDirPath(const QString& prefix);
    static QString statusMessage(libofd_status_t status);
};

#endif

