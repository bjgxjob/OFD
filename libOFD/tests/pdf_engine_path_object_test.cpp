#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include "libofd/libofd.h"

namespace fs = std::filesystem;

static std::string ReadWhole(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

int main() {
#ifndef LIBOFD_SOURCE_DIR
#define LIBOFD_SOURCE_DIR "."
#endif
    const fs::path src_dir(LIBOFD_SOURCE_DIR);
    const fs::path work_dir = src_dir / "tests" / "tmp_pdf_path";
    std::error_code ec;
    fs::remove_all(work_dir, ec);
    fs::create_directories(work_dir / "Doc_0" / "Pages" / "Page_0", ec);

    {
        std::ofstream ofd(work_dir / "OFD.xml", std::ios::binary);
        ofd << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
        ofd << "<ofd:OFD xmlns:ofd=\"http://www.ofdspec.org/2016\"><ofd:DocBody><ofd:DocRoot>Doc_0/Document.xml</ofd:DocRoot></ofd:DocBody></ofd:OFD>\n";
    }
    {
        std::ofstream doc(work_dir / "Doc_0" / "Document.xml", std::ios::binary);
        doc << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
        doc << "<ofd:Document xmlns:ofd=\"http://www.ofdspec.org/2016\">\n";
        doc << "  <ofd:CommonData><ofd:MaxUnitID>5</ofd:MaxUnitID><ofd:PageArea><ofd:PhysicalBox>0 0 210 297</ofd:PhysicalBox></ofd:PageArea></ofd:CommonData>\n";
        doc << "  <ofd:Pages><ofd:Page ID=\"1\" BaseLoc=\"Doc_0/Pages/Page_0/Content.xml\" /></ofd:Pages>\n";
        doc << "</ofd:Document>\n";
    }
    {
        std::ofstream page(work_dir / "Doc_0" / "Pages" / "Page_0" / "Content.xml", std::ios::binary);
        page << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
        page << "<ofd:Page xmlns:ofd=\"http://www.ofdspec.org/2016\"><ofd:Content>\n";
        page << "  <ofd:PathObject ID=\"1\" Boundary=\"10 10 80 80\" LineWidth=\"0.4\" Stroke=\"true\" Fill=\"false\">\n";
        page << "    <ofd:AbbreviatedData>M 0 0 L 40 0 L 40 40 B</ofd:AbbreviatedData>\n";
        page << "  </ofd:PathObject>\n";
        page << "</ofd:Content></ofd:Page>\n";
    }

    libofd_handle_t* h = libofd_create();
    if (h == nullptr) {
        return EXIT_FAILURE;
    }

    const fs::path pdf_out = work_dir / "path.pdf";
    libofd_status_t status = libofd_convert_ofd_to_pdf(h, work_dir.string().c_str(), pdf_out.string().c_str());
    if (status != LIBOFD_OK || !fs::exists(pdf_out)) {
        libofd_destroy(h);
        return EXIT_FAILURE;
    }

    const std::string pdf = ReadWhole(pdf_out);
    if (pdf.find(" m\n") == std::string::npos || pdf.find(" l\n") == std::string::npos ||
        pdf.find("\nS\n") == std::string::npos) {
        std::cerr << "expected path commands not found in pdf stream\n";
        libofd_destroy(h);
        return EXIT_FAILURE;
    }

    libofd_destroy(h);
    fs::remove_all(work_dir, ec);
    return EXIT_SUCCESS;
}

