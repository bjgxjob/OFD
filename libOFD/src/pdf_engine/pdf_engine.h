#ifndef LIBOFD_PDF_ENGINE_H
#define LIBOFD_PDF_ENGINE_H

#include <cstddef>
#include <string>

#include "libofd/status.h"

namespace libofd::pdf_engine {

typedef libofd_status_t (*ExternalImageDecodeFn)(
    const char* image_path, int* out_width, int* out_height, int* out_bits_per_component, int* out_color_components,
    unsigned char* out_pixels, size_t* inout_pixels_len, void* user_data);

struct ConvertOptions {
    ExternalImageDecodeFn external_image_decode_fn = nullptr;
    void* external_image_decode_user_data = nullptr;
};

enum class PdfToOfdMode {
    kAuto = 0,
    kStructured = 1,
    kVisualRaster = 2
};

struct PdfToOfdOptions {
    PdfToOfdMode mode = PdfToOfdMode::kAuto;
};

libofd_status_t ConvertOfdToPdf(
    const std::string& input_ofd_path, const std::string& output_pdf_path, const ConvertOptions* options = nullptr);
libofd_status_t ConvertPdfToOfd(
    const std::string& input_pdf_path, const std::string& output_ofd_path, const PdfToOfdOptions* options = nullptr);

} // namespace libofd::pdf_engine

#endif

