#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "libofd/libofd.h"

namespace fs = std::filesystem;

static bool WriteFile(const fs::path& path, const std::string& data) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary);
    if (!out.is_open()) {
        return false;
    }
    out << data;
    return true;
}

static std::string ReadWhole(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

static bool WriteSimplePdf(const fs::path& path) {
    const std::string content = "BT /F1 18 Tf 1 0 0 1 15 40 Tm [(AB) 200 (CD)] TJ ET\n";
    std::vector<std::string> objs;
    objs.push_back("<< /Type /Catalog /Pages 2 0 R >>");
    objs.push_back("<< /Type /Pages /Kids [3 0 R] /Count 1 >>");
    objs.push_back("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 100 100] /Resources << /Font << /F1 5 0 R >> >> /Contents 4 0 R >>");
    objs.push_back("<< /Length " + std::to_string(content.size()) + " >>\nstream\n" + content + "endstream");
    objs.push_back("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>");

    std::ostringstream out;
    out << "%PDF-1.4\n";
    std::vector<long> offsets(6, 0);
    for (size_t i = 0; i < objs.size(); ++i) {
        offsets[i + 1] = static_cast<long>(out.tellp());
        out << (i + 1) << " 0 obj\n" << objs[i] << "\nendobj\n";
    }
    const long xref = static_cast<long>(out.tellp());
    out << "xref\n0 6\n";
    out << "0000000000 65535 f \n";
    for (size_t i = 1; i < offsets.size(); ++i) {
        out << std::setw(10) << std::setfill('0') << offsets[i] << " 00000 n \n";
    }
    out << "trailer\n<< /Size 6 /Root 1 0 R >>\nstartxref\n" << xref << "\n%%EOF\n";
    return WriteFile(path, out.str());
}

int main() {
#ifndef LIBOFD_SOURCE_DIR
#define LIBOFD_SOURCE_DIR "."
#endif
    const fs::path src_dir(LIBOFD_SOURCE_DIR);
    const fs::path work_dir = src_dir / "tests" / "tmp_pdf_text_geometry";
    std::error_code ec;
    fs::remove_all(work_dir, ec);
    fs::create_directories(work_dir, ec);

    const fs::path pdf_path = work_dir / "in.pdf";
    const fs::path ofd_dir = work_dir / "out_ofd";
    if (!WriteSimplePdf(pdf_path)) {
        std::cerr << "write pdf failed\n";
        return EXIT_FAILURE;
    }

    libofd_handle_t* h = libofd_create();
    if (h == nullptr) {
        return EXIT_FAILURE;
    }
    const libofd_status_t status = libofd_convert_pdf_to_ofd(h, pdf_path.string().c_str(), ofd_dir.string().c_str());
    libofd_destroy(h);
    if (status != LIBOFD_OK) {
        std::cerr << "convert failed: " << libofd_status_message(status) << "\n";
        return EXIT_FAILURE;
    }

    const std::string page_xml = ReadWhole(ofd_dir / "Doc_0" / "Pages" / "Page_0" / "Content.xml");
    if (page_xml.find("ABCD") == std::string::npos) {
        std::cerr << "text missing\n";
        return EXIT_FAILURE;
    }
    // 18pt ~= 6.35mm; this ensures we persist text geometry instead of fixed default size.
    if (page_xml.find("Size=\"6.") == std::string::npos && page_xml.find("Size=\"7.") == std::string::npos) {
        std::cerr << "text geometry size mapping missing\n";
        return EXIT_FAILURE;
    }
    if (page_xml.find("DeltaX=\"") == std::string::npos) {
        std::cerr << "text geometry DeltaX mapping missing\n";
        return EXIT_FAILURE;
    }
    const size_t dx = page_xml.find("DeltaX=\"");
    const size_t de = page_xml.find("\"", dx + 8U);
    if (dx == std::string::npos || de == std::string::npos) {
        std::cerr << "DeltaX parse failed\n";
        return EXIT_FAILURE;
    }
    const std::string vals = page_xml.substr(dx + 8U, de - (dx + 8U));
    std::stringstream ss(vals);
    std::vector<double> nums;
    double v = 0.0;
    while (ss >> v) {
        nums.push_back(v);
    }
    if (nums.size() < 3U) {
        std::cerr << "DeltaX count too small\n";
        return EXIT_FAILURE;
    }
    bool has_variation = false;
    for (size_t i = 1; i < nums.size(); ++i) {
        if (std::abs(nums[i] - nums[0]) > 0.05) {
            has_variation = true;
            break;
        }
    }
    if (!has_variation) {
        std::cerr << "DeltaX variation missing for TJ spacing\n";
        return EXIT_FAILURE;
    }

    fs::remove_all(work_dir, ec);
    return EXIT_SUCCESS;
}
