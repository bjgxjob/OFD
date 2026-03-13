#include <cstdlib>
#include <cctype>
#include <filesystem>
#include <iostream>
#include <string>
#include <unistd.h>

#include "libofd/libofd.h"

namespace fs = std::filesystem;

namespace {

enum class ConvertMode {
    kAuto = 0,
    kOfdToPdf,
    kPdfToOfd
};

struct CliOptions {
    std::string input;
    std::string output;
    ConvertMode mode = ConvertMode::kAuto;
    libofd_pdf_to_ofd_mode_t pdf_to_ofd_mode = LIBOFD_PDF_TO_OFD_MODE_AUTO;
    bool keep_temp = false;
};

std::string ToLower(std::string value) {
    for (char& c : value) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return value;
}

std::string ExtLower(const fs::path& path) {
    return ToLower(path.extension().string());
}

std::string ShellQuote(const std::string& value) {
    std::string out = "'";
    for (char c : value) {
        if (c == '\'') {
            out += "'\\''";
        } else {
            out.push_back(c);
        }
    }
    out.push_back('\'');
    return out;
}

void PrintUsage(const char* argv0) {
    std::cout << "Usage:\n"
              << "  " << argv0
              << " --input <path> --output <path> [--mode auto|ofd2pdf|pdf2ofd]\n"
              << "      [--pdf2ofd-mode auto|structured|visual-raster] [--keep-temp]\n"
              << "  " << argv0 << " --help\n\n"
              << "Examples:\n"
              << "  " << argv0 << " --input tests/data/bank.ofd --output out.pdf\n"
              << "  " << argv0 << " --input tests/data/a.pdf --output out.ofd\n";
}

bool ParseArgs(int argc, char** argv, CliOptions* out, bool* out_help_requested) {
    if (out == nullptr) {
        return false;
    }
    if (out_help_requested != nullptr) {
        *out_help_requested = false;
    }
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            if (out_help_requested != nullptr) {
                *out_help_requested = true;
            }
            return false;
        }
        if (arg == "--input" && i + 1 < argc) {
            out->input = argv[++i];
            continue;
        }
        if (arg == "--output" && i + 1 < argc) {
            out->output = argv[++i];
            continue;
        }
        if (arg == "--mode" && i + 1 < argc) {
            const std::string mode = ToLower(argv[++i]);
            if (mode == "auto") {
                out->mode = ConvertMode::kAuto;
            } else if (mode == "ofd2pdf") {
                out->mode = ConvertMode::kOfdToPdf;
            } else if (mode == "pdf2ofd") {
                out->mode = ConvertMode::kPdfToOfd;
            } else {
                std::cerr << "Unknown mode: " << mode << "\n";
                return false;
            }
            continue;
        }
        if (arg == "--keep-temp") {
            out->keep_temp = true;
            continue;
        }
        if (arg == "--pdf2ofd-mode" && i + 1 < argc) {
            const std::string mode = ToLower(argv[++i]);
            if (mode == "auto") {
                out->pdf_to_ofd_mode = LIBOFD_PDF_TO_OFD_MODE_AUTO;
            } else if (mode == "structured") {
                out->pdf_to_ofd_mode = LIBOFD_PDF_TO_OFD_MODE_STRUCTURED;
            } else if (mode == "visual-raster" || mode == "visual") {
                out->pdf_to_ofd_mode = LIBOFD_PDF_TO_OFD_MODE_VISUAL_RASTER;
            } else {
                std::cerr << "Unknown --pdf2ofd-mode: " << mode << "\n";
                return false;
            }
            continue;
        }
        std::cerr << "Unknown option: " << arg << "\n";
        return false;
    }
    if (out->input.empty() || out->output.empty()) {
        std::cerr << "Missing --input or --output\n";
        return false;
    }
    return true;
}

ConvertMode DetectMode(const CliOptions& opts) {
    if (opts.mode != ConvertMode::kAuto) {
        return opts.mode;
    }
    const fs::path input(opts.input);
    const fs::path output(opts.output);
    const std::string in_ext = ExtLower(input);
    const std::string out_ext = ExtLower(output);
    if ((in_ext == ".ofd" && out_ext == ".ofd") || (in_ext == ".pdf" && out_ext == ".pdf")) {
        return ConvertMode::kAuto;
    }

    if (in_ext == ".pdf") {
        return ConvertMode::kPdfToOfd;
    }
    if (out_ext == ".pdf") {
        return ConvertMode::kOfdToPdf;
    }
    if (in_ext == ".ofd") {
        return ConvertMode::kOfdToPdf;
    }
    if (out_ext == ".ofd") {
        return ConvertMode::kPdfToOfd;
    }
    return ConvertMode::kOfdToPdf;
}

bool UnzipOfdToDir(const std::string& ofd_file, const fs::path& out_dir) {
    std::error_code ec;
    fs::create_directories(out_dir, ec);
    const std::string cmd = "unzip -qq -o " + ShellQuote(ofd_file) + " -d " + ShellQuote(out_dir.string());
    return std::system(cmd.c_str()) == 0;
}

bool ZipDirToOfd(const fs::path& in_dir, const std::string& ofd_file) {
    const std::string cmd = "sh -c \"cd " + ShellQuote(in_dir.string()) + " && zip -qr " + ShellQuote(ofd_file) + " .\"";
    return std::system(cmd.c_str()) == 0;
}

int RunOfdToPdf(libofd_handle_t* h, const CliOptions& opts, const fs::path& temp_root) {
    fs::path source = opts.input;
    const fs::path input_path(opts.input);
    if (fs::is_regular_file(input_path) && ExtLower(input_path) == ".ofd") {
        const fs::path extracted = temp_root / "ofd_extracted";
        if (!UnzipOfdToDir(opts.input, extracted)) {
            std::cerr << "Failed to unzip OFD: " << opts.input << "\n";
            return 2;
        }
        source = extracted;
    }
    const libofd_status_t status = libofd_convert_ofd_to_pdf(h, source.string().c_str(), opts.output.c_str());
    if (status != LIBOFD_OK) {
        std::cerr << "OFD->PDF convert failed: " << libofd_status_message(status) << "\n";
        return 2;
    }
    return 0;
}

int RunPdfToOfd(libofd_handle_t* h, const CliOptions& opts, const fs::path& temp_root) {
    fs::path out_target = opts.output;
    fs::path out_ofd_zip = fs::absolute(opts.output);
    bool need_zip = false;
    if (ExtLower(out_target) == ".ofd") {
        out_target = temp_root / "ofd_output_dir";
        need_zip = true;
    }
    if (fs::exists(out_target)) {
        std::error_code ec;
        fs::remove_all(out_target, ec);
    }

    libofd_set_pdf_to_ofd_mode(h, opts.pdf_to_ofd_mode);
    const libofd_status_t status = libofd_convert_pdf_to_ofd(h, opts.input.c_str(), out_target.string().c_str());
    if (status != LIBOFD_OK) {
        std::cerr << "PDF->OFD convert failed: " << libofd_status_message(status) << "\n";
        return 2;
    }
    if (need_zip) {
        if (fs::exists(out_ofd_zip)) {
            std::error_code ec;
            fs::remove(out_ofd_zip, ec);
        }
        if (!ZipDirToOfd(out_target, out_ofd_zip.string())) {
            std::cerr << "Failed to package OFD zip: " << out_ofd_zip << "\n";
            return 2;
        }
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    CliOptions opts;
    bool help_requested = false;
    if (!ParseArgs(argc, argv, &opts, &help_requested)) {
        PrintUsage(argv[0]);
        return help_requested ? 0 : 1;
    }
    const ConvertMode mode = DetectMode(opts);
    if (mode == ConvertMode::kAuto) {
        std::cerr << "Cannot infer conversion direction. Please specify --mode ofd2pdf or --mode pdf2ofd.\n";
        return 1;
    }

    libofd_handle_t* h = libofd_create();
    if (h == nullptr) {
        std::cerr << "Failed to create libofd handle\n";
        return 2;
    }

    const fs::path temp_root = fs::temp_directory_path() / ("libofd_cli_" + std::to_string(static_cast<long long>(::getpid())));
    std::error_code ec;
    fs::create_directories(temp_root, ec);

    int rc = 0;
    if (mode == ConvertMode::kOfdToPdf) {
        rc = RunOfdToPdf(h, opts, temp_root);
    } else {
        rc = RunPdfToOfd(h, opts, temp_root);
    }

    if (!opts.keep_temp) {
        fs::remove_all(temp_root, ec);
    } else {
        std::cout << "Temp kept at: " << temp_root << "\n";
    }
    if (rc == 0) {
        std::cout << "Convert success: " << opts.input << " -> " << opts.output << "\n";
    }
    libofd_destroy(h);
    return rc;
}

