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

static size_t CountSubstr(const std::string& text, const std::string& token) {
    size_t count = 0;
    size_t pos = 0;
    while ((pos = text.find(token, pos)) != std::string::npos) {
        ++count;
        pos += token.size();
    }
    return count;
}

int main() {
#ifndef LIBOFD_SOURCE_DIR
#define LIBOFD_SOURCE_DIR "."
#endif
    const fs::path src_dir(LIBOFD_SOURCE_DIR);
    const fs::path work_dir = src_dir / "tests" / "tmp_pdf_png_bmp";
    std::error_code ec;
    fs::remove_all(work_dir, ec);
    fs::create_directories(work_dir / "Doc_0" / "Pages" / "Page_0", ec);
    fs::create_directories(work_dir / "Doc_0" / "Res", ec);

    // tiny 1x1 PNG (RGB)
    const std::vector<unsigned char> png = {
        0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52,
        0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x08, 0x02, 0x00, 0x00, 0x00, 0x90, 0x77, 0x53, 0xDE,
        0x00, 0x00, 0x00, 0x0C, 0x49, 0x44, 0x41, 0x54, 0x08, 0xD7, 0x63, 0xF8, 0xCF, 0xC0, 0x00, 0x00, 0x03,
        0x01, 0x01, 0x00, 0x18, 0xDD, 0x8D, 0xB1, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4E, 0x44, 0xAE, 0x42,
        0x60, 0x82};
    {
        std::ofstream p(work_dir / "Doc_0" / "Res" / "tiny.png", std::ios::binary);
        p.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
    }

    // tiny 1x1 BMP (24-bit, red)
    {
        unsigned char bmp[58] = {0};
        bmp[0] = 'B';
        bmp[1] = 'M';
        bmp[2] = 58; // file size
        bmp[10] = 54; // pixel offset
        bmp[14] = 40; // DIB size
        bmp[18] = 1;  // width
        bmp[22] = 1;  // height
        bmp[26] = 1;  // planes
        bmp[28] = 24; // bpp
        bmp[34] = 4;  // image size with row padding
        // Pixel B,G,R + pad
        bmp[54] = 0x00;
        bmp[55] = 0x00;
        bmp[56] = 0xFF;
        bmp[57] = 0x00;
        std::ofstream b(work_dir / "Doc_0" / "Res" / "tiny.bmp", std::ios::binary);
        b.write(reinterpret_cast<const char*>(bmp), static_cast<std::streamsize>(sizeof(bmp)));
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
        doc << "  <ofd:CommonData><ofd:MaxUnitID>8</ofd:MaxUnitID><ofd:DocumentRes>Doc_0/DocumentRes.xml</ofd:DocumentRes></ofd:CommonData>\n";
        doc << "  <ofd:Pages><ofd:Page ID=\"1\" BaseLoc=\"Doc_0/Pages/Page_0/Content.xml\" /></ofd:Pages>\n";
        doc << "</ofd:Document>\n";
    }
    {
        std::ofstream res(work_dir / "Doc_0" / "DocumentRes.xml", std::ios::binary);
        res << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
        res << "<ofd:Res xmlns:ofd=\"http://www.ofdspec.org/2016\"><ofd:MultiMedias>\n";
        res << "  <ofd:MultiMedia ID=\"10\" Type=\"Image\"><ofd:MediaFile>Res/tiny.png</ofd:MediaFile></ofd:MultiMedia>\n";
        res << "  <ofd:MultiMedia ID=\"11\" Type=\"Image\"><ofd:MediaFile>Res/tiny.bmp</ofd:MediaFile></ofd:MultiMedia>\n";
        res << "</ofd:MultiMedias></ofd:Res>\n";
    }
    {
        std::ofstream page(work_dir / "Doc_0" / "Pages" / "Page_0" / "Content.xml", std::ios::binary);
        page << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
        page << "<ofd:Page xmlns:ofd=\"http://www.ofdspec.org/2016\"><ofd:Content>\n";
        page << "  <ofd:ImageObject ID=\"1\" ResourceID=\"10\" Boundary=\"10 10 20 20\" />\n";
        page << "  <ofd:ImageObject ID=\"2\" ResourceID=\"11\" Boundary=\"40 10 20 20\" />\n";
        page << "</ofd:Content></ofd:Page>\n";
    }

    libofd_handle_t* h = libofd_create();
    if (h == nullptr) {
        return EXIT_FAILURE;
    }
    const fs::path pdf_out = work_dir / "png_bmp.pdf";
    const libofd_status_t status = libofd_convert_ofd_to_pdf(h, work_dir.string().c_str(), pdf_out.string().c_str());
    if (status != LIBOFD_OK || !fs::exists(pdf_out)) {
        libofd_destroy(h);
        return EXIT_FAILURE;
    }
    const std::string pdf = ReadWhole(pdf_out);
    if (CountSubstr(pdf, "/Subtype /Image") < 2U || pdf.find("/Filter /FlateDecode") == std::string::npos) {
        std::cerr << "expected png/bmp image markers not found in pdf\n";
        libofd_destroy(h);
        return EXIT_FAILURE;
    }

    libofd_destroy(h);
    fs::remove_all(work_dir, ec);
    return EXIT_SUCCESS;
}

