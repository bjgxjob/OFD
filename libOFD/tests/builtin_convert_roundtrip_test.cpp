#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

#include "libofd/libofd.h"

namespace fs = std::filesystem;

int main() {
#ifndef LIBOFD_SOURCE_DIR
#define LIBOFD_SOURCE_DIR "."
#endif
    const fs::path src_dir(LIBOFD_SOURCE_DIR);
    const fs::path work_dir = src_dir / "tests" / "tmp_builtin_convert";
    std::error_code ec;
    fs::remove_all(work_dir, ec);
    fs::create_directories(work_dir, ec);

    libofd_handle_t* handle = libofd_create();
    if (handle == nullptr) {
        return EXIT_FAILURE;
    }

    // Create OFD document with two pages.
    if (libofd_create_empty(handle, "builtin-convert-doc", "test") != LIBOFD_OK ||
        libofd_add_page_text(handle, "first page line A") != LIBOFD_OK ||
        libofd_add_page_text(handle, "second page line B") != LIBOFD_OK) {
        libofd_destroy(handle);
        return EXIT_FAILURE;
    }

    const fs::path ofd_input = work_dir / "input_ofd";
    if (libofd_save_exploded_package(handle, ofd_input.string().c_str()) != LIBOFD_OK) {
        libofd_destroy(handle);
        return EXIT_FAILURE;
    }

    const fs::path pdf_out = work_dir / "output.pdf";
    if (libofd_convert_ofd_to_pdf(handle, ofd_input.string().c_str(), pdf_out.string().c_str()) != LIBOFD_OK) {
        std::cerr << "convert ofd->pdf failed\n";
        libofd_destroy(handle);
        return EXIT_FAILURE;
    }
    if (!fs::exists(pdf_out)) {
        libofd_destroy(handle);
        return EXIT_FAILURE;
    }

    const fs::path ofd_roundtrip = work_dir / "roundtrip_ofd";
    if (libofd_convert_pdf_to_ofd(handle, pdf_out.string().c_str(), ofd_roundtrip.string().c_str()) != LIBOFD_OK) {
        std::cerr << "convert pdf->ofd failed\n";
        libofd_destroy(handle);
        return EXIT_FAILURE;
    }
    if (!fs::exists(ofd_roundtrip / "OFD.xml")) {
        libofd_destroy(handle);
        return EXIT_FAILURE;
    }

    libofd_handle_t* verify = libofd_create();
    if (verify == nullptr) {
        libofd_destroy(handle);
        return EXIT_FAILURE;
    }
    if (libofd_load_exploded_package(verify, ofd_roundtrip.string().c_str()) != LIBOFD_OK) {
        libofd_destroy(verify);
        libofd_destroy(handle);
        return EXIT_FAILURE;
    }
    if (libofd_get_page_count(verify) == 0U) {
        libofd_destroy(verify);
        libofd_destroy(handle);
        return EXIT_FAILURE;
    }

    char text_buf[1024] = {0};
    if (libofd_get_page_text(verify, 0, text_buf, sizeof(text_buf)) != LIBOFD_OK) {
        libofd_destroy(verify);
        libofd_destroy(handle);
        return EXIT_FAILURE;
    }
    const std::string first_text(text_buf);
    if (first_text.find("first page") == std::string::npos) {
        std::cerr << "roundtrip text mismatch: " << first_text << "\n";
        libofd_destroy(verify);
        libofd_destroy(handle);
        return EXIT_FAILURE;
    }

    libofd_destroy(verify);
    libofd_destroy(handle);
    fs::remove_all(work_dir, ec);
    return EXIT_SUCCESS;
}

