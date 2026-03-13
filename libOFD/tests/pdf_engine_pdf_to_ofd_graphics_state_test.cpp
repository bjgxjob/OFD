#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <regex>
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
    const std::string content =
        "q 2 0 0 2 10 20 cm 1 0 0 RG 1 w 0 0 m 10 0 l S Q\n"
        "0 1 0 rg 20 30 5 5 re f\n";
    std::vector<std::string> objs;
    objs.push_back("<< /Type /Catalog /Pages 2 0 R >>");
    objs.push_back("<< /Type /Pages /Kids [3 0 R] /Count 1 >>");
    objs.push_back("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 100 100] /Resources << >> /Contents 4 0 R >>");
    objs.push_back("<< /Length " + std::to_string(content.size()) + " >>\nstream\n" + content + "endstream");

    std::ostringstream out;
    out << "%PDF-1.4\n";
    std::vector<long> offsets(5, 0);
    for (size_t i = 0; i < objs.size(); ++i) {
        offsets[i + 1] = static_cast<long>(out.tellp());
        out << (i + 1) << " 0 obj\n" << objs[i] << "\nendobj\n";
    }
    const long xref = static_cast<long>(out.tellp());
    out << "xref\n0 5\n";
    out << "0000000000 65535 f \n";
    for (size_t i = 1; i < offsets.size(); ++i) {
        out << std::setw(10) << std::setfill('0') << offsets[i] << " 00000 n \n";
    }
    out << "trailer\n<< /Size 5 /Root 1 0 R >>\nstartxref\n" << xref << "\n%%EOF\n";
    return WriteFile(path, out.str());
}

int main() {
#ifndef LIBOFD_SOURCE_DIR
#define LIBOFD_SOURCE_DIR "."
#endif
    const fs::path src_dir(LIBOFD_SOURCE_DIR);
    const fs::path work_dir = src_dir / "tests" / "tmp_pdf_graphics_state";
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
    if (status != LIBOFD_OK) {
        std::cerr << "convert failed: " << libofd_status_message(status) << "\n";
        libofd_destroy(h);
        return EXIT_FAILURE;
    }
    libofd_destroy(h);

    const std::string page_xml = ReadWhole(ofd_dir / "Doc_0" / "Pages" / "Page_0" / "Content.xml");
    if (page_xml.find("<ofd:PathObject") == std::string::npos) {
        std::cerr << "no path object generated\n";
        return EXIT_FAILURE;
    }
    if (page_xml.find("<ofd:StrokeColor Value=\"255 0 0\"/>") == std::string::npos) {
        std::cerr << "stroke color mapping missing\n";
        return EXIT_FAILURE;
    }
    if (page_xml.find("<ofd:FillColor Value=\"0 255 0\"/>") == std::string::npos) {
        std::cerr << "fill color mapping missing\n";
        return EXIT_FAILURE;
    }
    std::smatch lw_match;
    const std::regex lw_re("LineWidth=\"([0-9.]+)\"");
    if (!std::regex_search(page_xml, lw_match, lw_re)) {
        std::cerr << "line width missing\n";
        return EXIT_FAILURE;
    }
    const double lw = std::stod(lw_match[1].str());
    if (lw < 0.6) {
        std::cerr << "line width transform not applied\n";
        return EXIT_FAILURE;
    }

    fs::remove_all(work_dir, ec);
    return EXIT_SUCCESS;
}
