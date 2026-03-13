#include "ofd_facade.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QTemporaryDir>
#include <QUuid>

namespace {
constexpr int kTextBufSize = 256 * 1024;
}

OfdFacade::OfdFacade() {
    handle_ = libofd_create();
}

OfdFacade::~OfdFacade() {
    if (handle_ != nullptr) {
        libofd_destroy(handle_);
        handle_ = nullptr;
    }
}

bool OfdFacade::createEmpty(const QString& docId, const QString& creator, QString* error) {
    if (handle_ == nullptr) {
        if (error != nullptr) {
            *error = "libofd handle is null";
        }
        return false;
    }
    const QByteArray id = docId.toUtf8();
    const QByteArray c = creator.toUtf8();
    const libofd_status_t status = libofd_create_empty(handle_, id.constData(), c.constData());
    if (status != LIBOFD_OK) {
        if (error != nullptr) {
            *error = statusMessage(status);
        }
        return false;
    }
    loaded_exploded_root_.clear();
    return true;
}

bool OfdFacade::openPath(const QString& path, QString* error) {
    if (handle_ == nullptr) {
        if (error != nullptr) {
            *error = "libofd handle is null";
        }
        return false;
    }
    const QFileInfo fi(path);
    if (!fi.exists()) {
        if (error != nullptr) {
            *error = "path does not exist: " + path;
        }
        return false;
    }

    libofd_status_t status = LIBOFD_ERR_INVALID_ARGUMENT;
    if (fi.isDir()) {
        const QByteArray root = QDir::cleanPath(path).toUtf8();
        status = libofd_load_exploded_package(handle_, root.constData());
        loaded_exploded_root_ = QDir::cleanPath(path);
    } else if (fi.suffix().toLower() == "ofd") {
        const QString tempRoot = newTempDirPath("ofd_gui_open_");
        QDir().mkpath(tempRoot);
        if (!runShellCommand("unzip", {"-qq", "-o", path, "-d", tempRoot}, QString(), error)) {
            return false;
        }
        const QByteArray root = tempRoot.toUtf8();
        status = libofd_load_exploded_package(handle_, root.constData());
        loaded_exploded_root_ = tempRoot;
    } else {
        if (error != nullptr) {
            *error = "unsupported input, expected .ofd or exploded directory";
        }
        return false;
    }
    if (status != LIBOFD_OK) {
        if (error != nullptr) {
            *error = statusMessage(status);
        }
        return false;
    }
    return true;
}

bool OfdFacade::savePath(const QString& path, QString* error) const {
    if (handle_ == nullptr) {
        if (error != nullptr) {
            *error = "libofd handle is null";
        }
        return false;
    }

    const QFileInfo fi(path);
    if (fi.suffix().toLower() == "ofd") {
        const QString tempRoot = newTempDirPath("ofd_gui_save_");
        QDir().mkpath(tempRoot);
        const QByteArray root = tempRoot.toUtf8();
        const libofd_status_t status = libofd_save_exploded_package(handle_, root.constData());
        if (status != LIBOFD_OK) {
            if (error != nullptr) {
                *error = statusMessage(status);
            }
            return false;
        }
        QFile::remove(path);
        if (!runShellCommand("zip", {"-qr", path, "."}, tempRoot, error)) {
            return false;
        }
        return true;
    }

    const QByteArray out = QDir::cleanPath(path).toUtf8();
    const libofd_status_t status = libofd_save_exploded_package(handle_, out.constData());
    if (status != LIBOFD_OK) {
        if (error != nullptr) {
            *error = statusMessage(status);
        }
        return false;
    }
    return true;
}

int OfdFacade::pageCount() const {
    if (handle_ == nullptr) {
        return 0;
    }
    return static_cast<int>(libofd_get_page_count(handle_));
}

QString OfdFacade::pageText(int pageIndex, QString* error) const {
    if (handle_ == nullptr || pageIndex < 0) {
        if (error != nullptr) {
            *error = "invalid state or page index";
        }
        return {};
    }
    std::string buffer;
    buffer.resize(kTextBufSize);
    const libofd_status_t status =
        libofd_get_page_text(handle_, static_cast<size_t>(pageIndex), buffer.data(), buffer.size());
    if (status != LIBOFD_OK) {
        if (error != nullptr) {
            *error = statusMessage(status);
        }
        return {};
    }
    return QString::fromUtf8(buffer.c_str());
}

bool OfdFacade::setPageText(int pageIndex, const QString& text, QString* error) {
    if (handle_ == nullptr || pageIndex < 0) {
        if (error != nullptr) {
            *error = "invalid state or page index";
        }
        return false;
    }
    const QByteArray t = text.toUtf8();
    const libofd_status_t status = libofd_set_page_text(handle_, static_cast<size_t>(pageIndex), t.constData());
    if (status != LIBOFD_OK) {
        if (error != nullptr) {
            *error = statusMessage(status);
        }
        return false;
    }
    return true;
}

bool OfdFacade::addPageText(const QString& text, QString* error) {
    if (handle_ == nullptr) {
        if (error != nullptr) {
            *error = "invalid state";
        }
        return false;
    }
    const QByteArray t = text.toUtf8();
    const libofd_status_t status = libofd_add_page_text(handle_, t.constData());
    if (status != LIBOFD_OK) {
        if (error != nullptr) {
            *error = statusMessage(status);
        }
        return false;
    }
    return true;
}

bool OfdFacade::convertPdfToOfd(const QString& inputPdf, const QString& outputOfd, int mode, QString* error) {
    if (handle_ == nullptr) {
        if (error != nullptr) {
            *error = "invalid state";
        }
        return false;
    }
    const auto m = static_cast<libofd_pdf_to_ofd_mode_t>(mode);
    libofd_status_t status = libofd_set_pdf_to_ofd_mode(handle_, m);
    if (status != LIBOFD_OK) {
        if (error != nullptr) {
            *error = statusMessage(status);
        }
        return false;
    }

    QString output = outputOfd;
    bool zipOutput = QFileInfo(outputOfd).suffix().toLower() == "ofd";
    if (zipOutput) {
        output = newTempDirPath("ofd_gui_convert_");
        QDir().mkpath(output);
    }

    const QByteArray in = inputPdf.toUtf8();
    const QByteArray out = output.toUtf8();
    status = libofd_convert_pdf_to_ofd(handle_, in.constData(), out.constData());
    if (status != LIBOFD_OK) {
        if (error != nullptr) {
            *error = statusMessage(status);
        }
        return false;
    }

    if (zipOutput) {
        QFile::remove(outputOfd);
        if (!runShellCommand("zip", {"-qr", outputOfd, "."}, output, error)) {
            return false;
        }
    }
    return true;
}

bool OfdFacade::convertOfdToPdf(const QString& inputOfd, const QString& outputPdf, QString* error) {
    if (handle_ == nullptr) {
        if (error != nullptr) {
            *error = "invalid state";
        }
        return false;
    }
    QString input = inputOfd;
    bool inputIsZip = QFileInfo(inputOfd).isFile() && QFileInfo(inputOfd).suffix().toLower() == "ofd";
    if (inputIsZip) {
        input = newTempDirPath("ofd_gui_pdf_");
        QDir().mkpath(input);
        if (!runShellCommand("unzip", {"-qq", "-o", inputOfd, "-d", input}, QString(), error)) {
            return false;
        }
    }

    const QByteArray in = input.toUtf8();
    const QByteArray out = outputPdf.toUtf8();
    const libofd_status_t status = libofd_convert_ofd_to_pdf(handle_, in.constData(), out.constData());
    if (status != LIBOFD_OK) {
        if (error != nullptr) {
            *error = statusMessage(status);
        }
        return false;
    }
    return true;
}

bool OfdFacade::runShellCommand(
    const QString& program, const QStringList& arguments, const QString& workingDir, QString* error) {
    QProcess process;
    if (!workingDir.isEmpty()) {
        process.setWorkingDirectory(workingDir);
    }
    process.start(program, arguments);
    if (!process.waitForStarted(3000)) {
        if (error != nullptr) {
            *error = "failed to start command: " + program;
        }
        return false;
    }
    if (!process.waitForFinished(-1)) {
        if (error != nullptr) {
            *error = "command timeout: " + program;
        }
        return false;
    }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        if (error != nullptr) {
            *error = QString("command failed: %1 %2\n%3")
                         .arg(program, arguments.join(" "), QString::fromLocal8Bit(process.readAllStandardError()));
        }
        return false;
    }
    return true;
}

QString OfdFacade::newTempDirPath(const QString& prefix) {
    return QDir::tempPath() + "/" + prefix + QUuid::createUuid().toString(QUuid::WithoutBraces);
}

QString OfdFacade::statusMessage(libofd_status_t status) {
    const char* message = libofd_status_message(status);
    return message == nullptr ? QString("unknown") : QString::fromUtf8(message);
}

