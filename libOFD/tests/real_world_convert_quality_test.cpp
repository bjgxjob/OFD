#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include "libofd/libofd.h"

namespace fs = std::filesystem;

static bool Contains(const std::string& text, const std::string& needle) {
    return text.find(needle) != std::string::npos;
}

static bool ReadFileToString(const std::string& path, std::string* out) {
    if (out == nullptr) {
        return false;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        return false;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    *out = ss.str();
    return true;
}

static bool UnzipOfdToDir(const fs::path& ofd_file, const fs::path& out_dir) {
    std::error_code ec;
    fs::remove_all(out_dir, ec);
    fs::create_directories(out_dir, ec);
    const std::string cmd = "unzip -qq -o " + ofd_file.string() + " -d " + out_dir.string();
    return std::system(cmd.c_str()) == 0;
}

int main() {
#ifndef LIBOFD_SOURCE_DIR
#define LIBOFD_SOURCE_DIR "."
#endif
    const fs::path src_dir(LIBOFD_SOURCE_DIR);
    const fs::path data_dir = src_dir / "tests" / "data";
    const fs::path work_dir = src_dir / "tests" / "tmp_real_world_quality";
    std::error_code ec;
    fs::remove_all(work_dir, ec);
    fs::create_directories(work_dir, ec);

    libofd_handle_t* h = libofd_create();
    if (h == nullptr) {
        return EXIT_FAILURE;
    }

    // Case 1: a.pdf -> ofd quality checks.
    const fs::path a_pdf = data_dir / "a.pdf";
    const fs::path a_ofd = work_dir / "a_ofd";
    if (libofd_convert_pdf_to_ofd(h, a_pdf.string().c_str(), a_ofd.string().c_str()) != LIBOFD_OK) {
        std::cerr << "a.pdf convert failed\n";
        libofd_destroy(h);
        return EXIT_FAILURE;
    }
    libofd_handle_t* a_loaded = libofd_create();
    if (a_loaded == nullptr || libofd_load_exploded_package(a_loaded, a_ofd.string().c_str()) != LIBOFD_OK) {
        std::cerr << "a.ofd load failed\n";
        if (a_loaded != nullptr) {
            libofd_destroy(a_loaded);
        }
        libofd_destroy(h);
        return EXIT_FAILURE;
    }
    char a_text[32768] = {0};
    if (libofd_get_page_text(a_loaded, 0, a_text, sizeof(a_text)) != LIBOFD_OK) {
        std::cerr << "a.ofd page text failed\n";
        libofd_destroy(a_loaded);
        libofd_destroy(h);
        return EXIT_FAILURE;
    }
    const std::string a_page(a_text);
    const bool has_text_keywords = Contains(a_page, "税务事项通知书") && Contains(a_page, "北京小雷科技有限公司");
    bool has_raster_fallback = false;
    for (int i = 0; i < 8; ++i) {
        const fs::path raster_png = a_ofd / "Doc_0" / "Res" / ("RasterPage_" + std::to_string(i) + ".png");
        const fs::path raster_jpg = a_ofd / "Doc_0" / "Res" / ("RasterPage_" + std::to_string(i) + ".jpg");
        if (fs::exists(raster_png) || fs::exists(raster_jpg)) {
            has_raster_fallback = true;
            break;
        }
    }
    if (!has_text_keywords && !has_raster_fallback) {
        std::cerr << "a.ofd quality mismatch\n";
        libofd_destroy(a_loaded);
        libofd_destroy(h);
        return EXIT_FAILURE;
    }
    std::string a_page_xml;
    if (ReadFileToString((a_ofd / "Doc_0" / "Pages" / "Page_0" / "Content.xml").string(), &a_page_xml) &&
        a_page_xml.find("ImageObject") == std::string::npos) {
        std::cerr << "a.ofd stamp/image object missing\n";
        libofd_destroy(a_loaded);
        libofd_destroy(h);
        return EXIT_FAILURE;
    }
    if (!fs::exists(a_ofd / "Doc_0" / "DocumentRes.xml")) {
        std::cerr << "a.ofd document resource missing\n";
        libofd_destroy(a_loaded);
        libofd_destroy(h);
        return EXIT_FAILURE;
    }
    libofd_destroy(a_loaded);

    // Case 2: bank.ofd -> pdf -> ofd quality checks.
    const fs::path bank_ofd = data_dir / "bank.ofd";
    const fs::path bank_unzip = work_dir / "bank_unzip";
    if (!UnzipOfdToDir(bank_ofd, bank_unzip)) {
        std::cerr << "bank.ofd unzip failed\n";
        libofd_destroy(h);
        return EXIT_FAILURE;
    }
    const fs::path bank_pdf = work_dir / "bank.pdf";
    const fs::path bank_rt_ofd = work_dir / "bank_rt_ofd";
    if (libofd_convert_ofd_to_pdf(h, bank_unzip.string().c_str(), bank_pdf.string().c_str()) != LIBOFD_OK) {
        std::cerr << "bank.ofd->pdf failed\n";
        libofd_destroy(h);
        return EXIT_FAILURE;
    }
    if (libofd_convert_pdf_to_ofd(h, bank_pdf.string().c_str(), bank_rt_ofd.string().c_str()) != LIBOFD_OK) {
        std::cerr << "bank.pdf->ofd failed\n";
        libofd_destroy(h);
        return EXIT_FAILURE;
    }
    libofd_handle_t* bank_loaded = libofd_create();
    if (bank_loaded == nullptr || libofd_load_exploded_package(bank_loaded, bank_rt_ofd.string().c_str()) != LIBOFD_OK) {
        std::cerr << "bank roundtrip load failed\n";
        if (bank_loaded != nullptr) {
            libofd_destroy(bank_loaded);
        }
        libofd_destroy(h);
        return EXIT_FAILURE;
    }
    char bank_text[131072] = {0};
    if (libofd_get_page_text(bank_loaded, 0, bank_text, sizeof(bank_text)) != LIBOFD_OK) {
        std::cerr << "bank roundtrip text failed\n";
        libofd_destroy(bank_loaded);
        libofd_destroy(h);
        return EXIT_FAILURE;
    }
    const std::string bank_page(bank_text);
    size_t line_count = 1;
    for (char c : bank_page) {
        if (c == '\n') {
            ++line_count;
        }
    }
    if (!Contains(bank_page, "北京清河支行") || !Contains(bank_page, "94,297.62") || line_count < 80U) {
        std::cerr << "bank roundtrip quality mismatch, lines=" << line_count << "\n";
        libofd_destroy(bank_loaded);
        libofd_destroy(h);
        return EXIT_FAILURE;
    }
    std::string bank_page_xml;
    if (ReadFileToString((bank_rt_ofd / "Doc_0" / "Pages" / "Page_0" / "Content.xml").string(), &bank_page_xml) &&
        bank_page_xml.find("ImageObject") == std::string::npos) {
        std::cerr << "bank roundtrip image object missing\n";
        libofd_destroy(bank_loaded);
        libofd_destroy(h);
        return EXIT_FAILURE;
    }
    if (!fs::exists(bank_rt_ofd / "Doc_0" / "Signs" / "Signatures.xml") ||
        !fs::exists(bank_rt_ofd / "Doc_0" / "Signs" / "Sign_0" / "Signature.xml")) {
        std::cerr << "bank roundtrip sign artifacts missing\n";
        libofd_destroy(bank_loaded);
        libofd_destroy(h);
        return EXIT_FAILURE;
    }
    libofd_destroy(bank_loaded);

    libofd_destroy(h);
    fs::remove_all(work_dir, ec);
    return EXIT_SUCCESS;
}
