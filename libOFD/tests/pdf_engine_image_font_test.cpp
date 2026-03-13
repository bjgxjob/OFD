#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

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
    const fs::path work_dir = src_dir / "tests" / "tmp_pdf_image_font";
    std::error_code ec;
    fs::remove_all(work_dir, ec);
    fs::create_directories(work_dir / "Doc_0" / "Pages" / "Page_0", ec);
    fs::create_directories(work_dir / "Doc_0" / "Res", ec);

    // Write minimal 1x1 jpeg bytes.
    const std::vector<unsigned char> jpeg = {
        0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x10, 0x4A, 0x46, 0x49, 0x46, 0x00, 0x01, 0x01, 0x00, 0x00, 0x01,
        0x00, 0x01, 0x00, 0x00, 0xFF, 0xDB, 0x00, 0x43, 0x00, 0x06, 0x04, 0x05, 0x06, 0x05, 0x04, 0x06,
        0x06, 0x05, 0x06, 0x07, 0x07, 0x06, 0x08, 0x0A, 0x10, 0x0A, 0x0A, 0x09, 0x09, 0x0A, 0x14, 0x0E,
        0x0F, 0x0C, 0x10, 0x17, 0x14, 0x18, 0x18, 0x17, 0x14, 0x16, 0x16, 0x1A, 0x1D, 0x25, 0x1F, 0x1A,
        0x1B, 0x23, 0x1C, 0x16, 0x16, 0x20, 0x2C, 0x20, 0x23, 0x26, 0x27, 0x29, 0x2A, 0x29, 0x19, 0x1F,
        0x2D, 0x30, 0x2D, 0x28, 0x30, 0x25, 0x28, 0x29, 0x28, 0xFF, 0xDB, 0x00, 0x43, 0x01, 0x07, 0x07,
        0x07, 0x0A, 0x08, 0x0A, 0x13, 0x0A, 0x0A, 0x13, 0x28, 0x1A, 0x16, 0x1A, 0x28, 0x28, 0x28, 0x28,
        0x28, 0x28, 0x28, 0x28, 0x28, 0x28, 0x28, 0x28, 0x28, 0x28, 0x28, 0x28, 0x28, 0x28, 0x28, 0x28,
        0x28, 0x28, 0x28, 0x28, 0x28, 0x28, 0x28, 0x28, 0x28, 0x28, 0x28, 0x28, 0x28, 0x28, 0x28, 0x28,
        0x28, 0x28, 0x28, 0x28, 0x28, 0x28, 0x28, 0x28, 0x28, 0x28, 0x28, 0x28, 0x28, 0xFF, 0xC0, 0x00,
        0x11, 0x08, 0x00, 0x01, 0x00, 0x01, 0x03, 0x01, 0x22, 0x00, 0x02, 0x11, 0x01, 0x03, 0x11, 0x01,
        0xFF, 0xC4, 0x00, 0x1F, 0x00, 0x00, 0x01, 0x05, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A,
        0x0B, 0xFF, 0xDA, 0x00, 0x0C, 0x03, 0x01, 0x00, 0x02, 0x11, 0x03, 0x11, 0x00, 0x3F, 0x00, 0xD2,
        0xCF, 0x20, 0xFF, 0xD9};
    {
        std::ofstream jpg(work_dir / "Doc_0" / "Res" / "tiny.jpg", std::ios::binary);
        jpg.write(reinterpret_cast<const char*>(jpeg.data()), static_cast<std::streamsize>(jpeg.size()));
    }

    {
        std::ofstream ofd(work_dir / "OFD.xml", std::ios::binary);
        ofd << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
        ofd << "<ofd:OFD xmlns:ofd=\"http://www.ofdspec.org/2016\"><ofd:DocBody><ofd:DocRoot>Doc_0/Document.xml</ofd:DocRoot></ofd:DocBody></ofd:OFD>\n";
    }
    {
        std::ofstream doc(work_dir / "Doc_0" / "Document.xml", std::ios::binary);
        doc << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
        doc << "<ofd:Document xmlns:ofd=\"http://www.ofdspec.org/2016\">\n";
        doc << "  <ofd:CommonData>\n";
        doc << "    <ofd:MaxUnitID>6</ofd:MaxUnitID>\n";
        doc << "    <ofd:PageArea><ofd:PhysicalBox>0 0 210 297</ofd:PhysicalBox></ofd:PageArea>\n";
        doc << "    <ofd:DocumentRes>Doc_0/DocumentRes.xml</ofd:DocumentRes>\n";
        doc << "  </ofd:CommonData>\n";
        doc << "  <ofd:Pages><ofd:Page ID=\"1\" BaseLoc=\"Doc_0/Pages/Page_0/Content.xml\" /></ofd:Pages>\n";
        doc << "</ofd:Document>\n";
    }
    {
        std::ofstream res(work_dir / "Doc_0" / "DocumentRes.xml", std::ios::binary);
        res << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
        res << "<ofd:Res xmlns:ofd=\"http://www.ofdspec.org/2016\">\n";
        res << "  <ofd:Fonts><ofd:Font ID=\"2\" FontName=\"Times New Roman\" /></ofd:Fonts>\n";
        res << "  <ofd:MultiMedias>\n";
        res << "    <ofd:MultiMedia ID=\"9\" Type=\"Image\"><ofd:MediaFile>Res/tiny.jpg</ofd:MediaFile></ofd:MultiMedia>\n";
        res << "  </ofd:MultiMedias>\n";
        res << "</ofd:Res>\n";
    }
    {
        std::ofstream page(work_dir / "Doc_0" / "Pages" / "Page_0" / "Content.xml", std::ios::binary);
        page << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
        page << "<ofd:Page xmlns:ofd=\"http://www.ofdspec.org/2016\"><ofd:Content>\n";
        page << "  <ofd:TextObject ID=\"1\" Font=\"2\" Size=\"4\" Boundary=\"10 20 90 10\">\n";
        page << "    <ofd:TextCode>LineOne</ofd:TextCode>\n";
        page << "    <ofd:TextCode>LineTwo</ofd:TextCode>\n";
        page << "  </ofd:TextObject>\n";
        page << "  <ofd:ImageObject ID=\"3\" ResourceID=\"9\" Boundary=\"20 40 30 30\" />\n";
        page << "</ofd:Content></ofd:Page>\n";
    }

    libofd_handle_t* h = libofd_create();
    if (h == nullptr) {
        return EXIT_FAILURE;
    }
    const fs::path pdf_out = work_dir / "image_font_layout.pdf";
    const libofd_status_t status = libofd_convert_ofd_to_pdf(h, work_dir.string().c_str(), pdf_out.string().c_str());
    if (status != LIBOFD_OK || !fs::exists(pdf_out)) {
        libofd_destroy(h);
        return EXIT_FAILURE;
    }
    const std::string pdf = ReadWhole(pdf_out);
    if (pdf.find("/Times-Roman") == std::string::npos || pdf.find("/Subtype /Image") == std::string::npos ||
        pdf.find("/XObject") == std::string::npos || pdf.find("LineOne\\nLineTwo") == std::string::npos) {
        std::cerr << "expected font/image/layout markers not found in pdf\n";
        libofd_destroy(h);
        return EXIT_FAILURE;
    }

    libofd_destroy(h);
    fs::remove_all(work_dir, ec);
    return EXIT_SUCCESS;
}

