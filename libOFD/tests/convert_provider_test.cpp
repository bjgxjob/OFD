#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include "libofd/libofd.h"

namespace fs = std::filesystem;

static libofd_status_t DummyOfdToPdf(const char* input_ofd_path, const char* output_pdf_path, void* user_data) {
    (void)user_data;
    if (input_ofd_path == nullptr || output_pdf_path == nullptr) {
        return LIBOFD_ERR_INVALID_ARGUMENT;
    }
    if (!fs::exists(input_ofd_path)) {
        return LIBOFD_ERR_NOT_FOUND;
    }
    std::error_code ec;
    fs::create_directories(fs::path(output_pdf_path).parent_path(), ec);
    std::ofstream out(output_pdf_path, std::ios::binary);
    if (!out.is_open()) {
        return LIBOFD_ERR_IO;
    }
    out << "%PDF-1.4\n% libofd mock conversion\n";
    return LIBOFD_OK;
}

static libofd_status_t DummyPdfToOfd(const char* input_pdf_path, const char* output_ofd_path, void* user_data) {
    (void)user_data;
    if (input_pdf_path == nullptr || output_ofd_path == nullptr) {
        return LIBOFD_ERR_INVALID_ARGUMENT;
    }
    if (!fs::exists(input_pdf_path)) {
        return LIBOFD_ERR_NOT_FOUND;
    }
    const fs::path out_dir(output_ofd_path);
    std::error_code ec;
    fs::create_directories(out_dir / "Doc_0", ec);
    std::ofstream ofd_xml(out_dir / "OFD.xml", std::ios::binary);
    if (!ofd_xml.is_open()) {
        return LIBOFD_ERR_IO;
    }
    ofd_xml << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    ofd_xml << "<ofd:OFD xmlns:ofd=\"http://www.ofdspec.org/2016\"><ofd:DocBody><ofd:DocRoot>Doc_0/Document.xml</ofd:DocRoot></ofd:DocBody></ofd:OFD>\n";
    std::ofstream doc_xml(out_dir / "Doc_0" / "Document.xml", std::ios::binary);
    if (!doc_xml.is_open()) {
        return LIBOFD_ERR_IO;
    }
    doc_xml << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    doc_xml << "<ofd:Document xmlns:ofd=\"http://www.ofdspec.org/2016\"><ofd:CommonData><ofd:MaxUnitID>1</ofd:MaxUnitID></ofd:CommonData></ofd:Document>\n";
    return LIBOFD_OK;
}

int main() {
#ifndef LIBOFD_SOURCE_DIR
#define LIBOFD_SOURCE_DIR "."
#endif
    const fs::path src_dir(LIBOFD_SOURCE_DIR);
    const fs::path work_dir = src_dir / "tests" / "tmp_convert_provider";
    std::error_code ec;
    fs::remove_all(work_dir, ec);
    fs::create_directories(work_dir, ec);

    libofd_handle_t* handle = libofd_create();
    if (handle == nullptr) {
        return EXIT_FAILURE;
    }

    // Built-in engine should work even without external provider.
    libofd_status_t status = LIBOFD_OK;
    const fs::path builtin_pdf = work_dir / "builtin.pdf";
    const fs::path builtin_ofd = work_dir / "builtin_ofd";
    const fs::path input_ofd = src_dir / "tests" / "data";
    status = libofd_convert_ofd_to_pdf(handle, input_ofd.string().c_str(), builtin_pdf.string().c_str());
    if (status != LIBOFD_OK || !fs::exists(builtin_pdf)) {
        std::cerr << "built-in ofd->pdf conversion failed\n";
        libofd_destroy(handle);
        return EXIT_FAILURE;
    }
    status = libofd_convert_pdf_to_ofd(handle, builtin_pdf.string().c_str(), builtin_ofd.string().c_str());
    if (status != LIBOFD_OK || !fs::exists(builtin_ofd / "OFD.xml")) {
        std::cerr << "built-in pdf->ofd conversion failed\n";
        libofd_destroy(handle);
        return EXIT_FAILURE;
    }

    libofd_external_convert_provider_t provider{};
    provider.ofd_to_pdf_fn = DummyOfdToPdf;
    provider.pdf_to_ofd_fn = DummyPdfToOfd;
    provider.user_data = nullptr;
    status = libofd_set_external_convert_provider(handle, &provider);
    if (status != LIBOFD_OK) {
        libofd_destroy(handle);
        return EXIT_FAILURE;
    }

    const fs::path output_pdf = work_dir / "out.pdf";
    status = libofd_convert_ofd_to_pdf(handle, input_ofd.string().c_str(), output_pdf.string().c_str());
    if (status != LIBOFD_OK || !fs::exists(output_pdf)) {
        std::cerr << "ofd->pdf conversion failed\n";
        libofd_destroy(handle);
        return EXIT_FAILURE;
    }

    const fs::path output_ofd = work_dir / "converted_ofd";
    status = libofd_convert_pdf_to_ofd(handle, output_pdf.string().c_str(), output_ofd.string().c_str());
    if (status != LIBOFD_OK || !fs::exists(output_ofd / "OFD.xml")) {
        std::cerr << "pdf->ofd conversion failed\n";
        libofd_destroy(handle);
        return EXIT_FAILURE;
    }

    status = libofd_clear_external_convert_provider(handle);
    if (status != LIBOFD_OK) {
        libofd_destroy(handle);
        return EXIT_FAILURE;
    }
    libofd_destroy(handle);
    fs::remove_all(work_dir, ec);
    return EXIT_SUCCESS;
}

