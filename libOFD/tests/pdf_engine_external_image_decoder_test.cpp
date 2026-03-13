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

static libofd_status_t DummyDecodeWebp(
    const char* image_path, int* out_w, int* out_h, int* out_bpc, int* out_components, unsigned char* out_pixels,
    size_t* inout_pixels_len, void* user_data) {
    (void)user_data;
    if (image_path == nullptr || out_w == nullptr || out_h == nullptr || out_bpc == nullptr || out_components == nullptr ||
        inout_pixels_len == nullptr) {
        return LIBOFD_ERR_INVALID_ARGUMENT;
    }
    const std::string p(image_path);
    if (p.find(".webp") == std::string::npos) {
        return LIBOFD_ERR_UNSUPPORTED;
    }
    *out_w = 1;
    *out_h = 1;
    *out_bpc = 8;
    *out_components = 3;
    if (out_pixels == nullptr) {
        *inout_pixels_len = 3U;
        return LIBOFD_OK;
    }
    if (*inout_pixels_len < 3U) {
        return LIBOFD_ERR_INVALID_ARGUMENT;
    }
    out_pixels[0] = 0x00;
    out_pixels[1] = 0xFF;
    out_pixels[2] = 0x00;
    *inout_pixels_len = 3U;
    return LIBOFD_OK;
}

int main() {
#ifndef LIBOFD_SOURCE_DIR
#define LIBOFD_SOURCE_DIR "."
#endif
    const fs::path src_dir(LIBOFD_SOURCE_DIR);
    const fs::path work_dir = src_dir / "tests" / "tmp_pdf_external_decoder";
    std::error_code ec;
    fs::remove_all(work_dir, ec);
    fs::create_directories(work_dir / "Doc_0" / "Pages" / "Page_0", ec);
    fs::create_directories(work_dir / "Doc_0" / "Res", ec);

    // fake webp file bytes
    {
        std::ofstream webp(work_dir / "Doc_0" / "Res" / "tiny.webp", std::ios::binary);
        webp << "RIFFxxxxWEBPVP8 ";
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
        doc << "  <ofd:CommonData><ofd:MaxUnitID>4</ofd:MaxUnitID><ofd:DocumentRes>Doc_0/DocumentRes.xml</ofd:DocumentRes></ofd:CommonData>\n";
        doc << "  <ofd:Pages><ofd:Page ID=\"1\" BaseLoc=\"Doc_0/Pages/Page_0/Content.xml\" /></ofd:Pages>\n";
        doc << "</ofd:Document>\n";
    }
    {
        std::ofstream res(work_dir / "Doc_0" / "DocumentRes.xml", std::ios::binary);
        res << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
        res << "<ofd:Res xmlns:ofd=\"http://www.ofdspec.org/2016\"><ofd:MultiMedias>\n";
        res << "  <ofd:MultiMedia ID=\"20\" Type=\"Image\"><ofd:MediaFile>Res/tiny.webp</ofd:MediaFile></ofd:MultiMedia>\n";
        res << "</ofd:MultiMedias></ofd:Res>\n";
    }
    {
        std::ofstream page(work_dir / "Doc_0" / "Pages" / "Page_0" / "Content.xml", std::ios::binary);
        page << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
        page << "<ofd:Page xmlns:ofd=\"http://www.ofdspec.org/2016\"><ofd:Content>\n";
        page << "  <ofd:ImageObject ID=\"1\" ResourceID=\"20\" Boundary=\"10 10 20 20\" />\n";
        page << "</ofd:Content></ofd:Page>\n";
    }

    libofd_handle_t* h = libofd_create();
    if (h == nullptr) {
        return EXIT_FAILURE;
    }

    // Without decoder provider, unsupported image won't be emitted.
    const fs::path no_decoder_pdf = work_dir / "no_decoder.pdf";
    if (libofd_convert_ofd_to_pdf(h, work_dir.string().c_str(), no_decoder_pdf.string().c_str()) != LIBOFD_OK) {
        libofd_destroy(h);
        return EXIT_FAILURE;
    }
    std::string pdf = ReadWhole(no_decoder_pdf);
    if (pdf.find("/Subtype /Image") != std::string::npos) {
        std::cerr << "unexpected image without external decoder\n";
        libofd_destroy(h);
        return EXIT_FAILURE;
    }

    libofd_external_image_decode_provider_t decoder{};
    decoder.decode_fn = DummyDecodeWebp;
    decoder.user_data = nullptr;
    if (libofd_set_external_image_decode_provider(h, &decoder) != LIBOFD_OK) {
        libofd_destroy(h);
        return EXIT_FAILURE;
    }
    const fs::path with_decoder_pdf = work_dir / "with_decoder.pdf";
    if (libofd_convert_ofd_to_pdf(h, work_dir.string().c_str(), with_decoder_pdf.string().c_str()) != LIBOFD_OK) {
        libofd_destroy(h);
        return EXIT_FAILURE;
    }
    pdf = ReadWhole(with_decoder_pdf);
    if (pdf.find("/Subtype /Image") == std::string::npos) {
        std::cerr << "expected image object from external decoder not found\n";
        libofd_destroy(h);
        return EXIT_FAILURE;
    }

    libofd_clear_external_image_decode_provider(h);
    libofd_destroy(h);
    fs::remove_all(work_dir, ec);
    return EXIT_SUCCESS;
}

