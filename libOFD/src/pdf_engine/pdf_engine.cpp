#include "pdf_engine.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <fstream>
#include <iomanip>
#include <regex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "libofd/document.h"

#ifdef LIBOFD_HAVE_ZLIB
#include <zlib.h>
#endif

namespace fs = std::filesystem;

namespace libofd::pdf_engine {

struct ResourceTable {
    std::unordered_map<std::string, std::string> fonts;
    std::unordered_map<std::string, fs::path> images;
};

struct OfdTextObject {
    std::string text;
    double x = 50.0;
    double y = 50.0;
    double font_size = 11.0;
    std::string ofd_font_id;
    std::vector<double> glyph_offsets_x;
};

struct OfdImageObject {
    fs::path file;
    double x = 0.0;
    double y = 0.0;
    double width = 0.0;
    double height = 0.0;
};

struct OfdPathObject {
    double boundary_x = 0.0;
    double boundary_y = 0.0;
    double line_width = 1.0;
    bool stroke = true;
    bool fill = false;
    bool has_stroke_color = false;
    bool has_fill_color = false;
    int stroke_r = 0;
    int stroke_g = 0;
    int stroke_b = 0;
    int fill_r = 0;
    int fill_g = 0;
    int fill_b = 0;
    std::string abbreviated_data;
};

struct OfdPageLayout {
    double width_pt = 595.0;
    double height_pt = 842.0;
    std::vector<OfdTextObject> text_objects;
    std::vector<OfdImageObject> image_objects;
    std::vector<OfdPathObject> path_objects;
    std::string fallback_text;
};

struct PdfToOfdImageAsset {
    int object_id = 0;
    int resource_id = 0;
    std::string file_name;
    std::string format;
    std::vector<unsigned char> data;
};

struct PdfEmbeddedFontAsset {
    std::string file_name;
    std::string font_name;
    std::vector<unsigned char> data;
};

struct PdfToOfdImageUse {
    int object_id = 0;
    double x_mm = 0.0;
    double y_mm = 0.0;
    double w_mm = 0.0;
    double h_mm = 0.0;
    bool is_stamp_annotation = false;
    bool is_page_raster = false;
};

struct PdfToOfdPathUse {
    double line_width_mm = 0.35;
    bool stroke = true;
    bool fill = false;
    bool has_stroke_color = false;
    bool has_fill_color = false;
    int stroke_r = 0;
    int stroke_g = 0;
    int stroke_b = 0;
    int fill_r = 0;
    int fill_g = 0;
    int fill_b = 0;
    std::string abbreviated_data;
};

struct PdfToOfdTextUse {
    std::string text;
    double x_mm = 0.0;
    double y_mm = 0.0;
    double size_mm = 3.5;
    std::vector<double> delta_x_mm;
    bool has_fill_color = false;
    int fill_r = 0;
    int fill_g = 0;
    int fill_b = 0;
};

struct PdfToOfdPageGraphics {
    double width_pt = 595.0;
    double height_pt = 842.0;
    std::vector<PdfToOfdTextUse> texts;
    std::vector<PdfToOfdImageUse> images;
    std::vector<PdfToOfdPathUse> paths;
};

static bool RenderPdfPagesToImages(const fs::path& pdf_path, std::vector<fs::path>* out_images) {
    if (out_images == nullptr) {
        return false;
    }
    out_images->clear();
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path tmp_dir = fs::temp_directory_path() / ("libofd_wps_raster_" + std::to_string(now));
    std::error_code ec;
    fs::create_directories(tmp_dir, ec);
    if (ec) {
        return false;
    }
    const fs::path prefix = tmp_dir / "page";
    bool rendered = false;
    {
        const std::string cmd = "pdftoppm -jpeg -r 180 \"" + pdf_path.string() + "\" \"" + prefix.string() + "\" > /dev/null 2>&1";
        rendered = (std::system(cmd.c_str()) == 0);
    }
    if (!rendered) {
        const std::string cmd = "gs -q -dSAFER -dBATCH -dNOPAUSE -sDEVICE=png16m -r180 -sOutputFile=\"" +
                                (tmp_dir / "page-%d.png").string() + "\" \"" + pdf_path.string() + "\" > /dev/null 2>&1";
        rendered = (std::system(cmd.c_str()) == 0);
    }
    if (!rendered) {
        const std::string cmd =
            "mutool draw -F jpeg -r 180 -o \"" + (tmp_dir / "page-%d.jpg").string() + "\" \"" + pdf_path.string() + "\" > /dev/null 2>&1";
        rendered = (std::system(cmd.c_str()) == 0);
    }
    if (!rendered) {
        const std::string py_cmd =
            "python3 - <<'PY'\n"
            "import sys,os\n"
            "try:\n"
            "  import fitz\n"
            "except Exception:\n"
            "  raise SystemExit(1)\n"
            "pdf=r'''"+ pdf_path.string() + "'''\n"
            "out=r'''"+ tmp_dir.string() + "'''\n"
            "doc=fitz.open(pdf)\n"
            "mat=fitz.Matrix(2.5,2.5)\n"
            "for i,p in enumerate(doc):\n"
            "  pix=p.get_pixmap(matrix=mat,alpha=False)\n"
            "  pix.save(os.path.join(out,f'page-{i+1}.jpg'))\n"
            "PY";
        rendered = (std::system(py_cmd.c_str()) == 0);
    }
    if (!rendered) {
        fs::remove_all(tmp_dir, ec);
        return false;
    }
    struct PagePng {
        int index = 0;
        fs::path path;
    };
    std::vector<PagePng> files;
    const std::regex re(R"(page-(\d+)\.(?:png|jpg|jpeg)$)");
    for (const auto& de : fs::directory_iterator(tmp_dir, ec)) {
        if (ec || !de.is_regular_file()) {
            continue;
        }
        const std::string name = de.path().filename().string();
        std::smatch m;
        if (!std::regex_search(name, m, re)) {
            continue;
        }
        files.push_back(PagePng{std::atoi(m[1].str().c_str()), de.path()});
    }
    std::sort(files.begin(), files.end(), [](const PagePng& a, const PagePng& b) { return a.index < b.index; });
    for (const auto& f : files) {
        out_images->push_back(f.path);
    }
    return !out_images->empty();
}

static void AppendRasterFallbackAssets(
    const std::vector<fs::path>& page_images, std::vector<PdfToOfdPageGraphics>* pages, std::vector<PdfToOfdImageAsset>* assets) {
    if (pages == nullptr || assets == nullptr || page_images.empty() || pages->empty()) {
        return;
    }
    int next_res_id = 1000;
    for (const auto& a : *assets) {
        next_res_id = std::max(next_res_id, a.resource_id + 1);
    }
    int synthetic_obj_id = 90000000;
    const size_t n = std::min(page_images.size(), pages->size());
    for (size_t i = 0; i < n; ++i) {
        std::ifstream in(page_images[i], std::ios::binary);
        if (!in.is_open()) {
            continue;
        }
        std::vector<unsigned char> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        std::string format;
        std::string ext;
        if (data.size() >= 2U && data[0] == 0xFFU && data[1] == 0xD8U) {
            format = "JPEG";
            ext = "jpg";
        } else if (data.size() >= 8U && data[0] == 0x89U && data[1] == 'P' && data[2] == 'N' && data[3] == 'G') {
            format = "PNG";
            ext = "png";
        } else {
            continue;
        }
        const int obj_id = synthetic_obj_id++;
        const int res_id = next_res_id++;
        const std::string file_name = "RasterPage_" + std::to_string(i) + "." + ext;
        assets->push_back(PdfToOfdImageAsset{obj_id, res_id, file_name, format, std::move(data)});
        PdfToOfdImageUse u;
        u.object_id = obj_id;
        u.x_mm = 0.0;
        u.y_mm = 0.0;
        constexpr double kMmToPtLocal = 2.834645669291339;
        u.w_mm = std::max(1.0, (*pages)[i].width_pt / kMmToPtLocal);
        u.h_mm = std::max(1.0, (*pages)[i].height_pt / kMmToPtLocal);
        u.is_page_raster = true;
        // Append last so it is emitted on top for WPS visual compatibility.
        (*pages)[i].images.push_back(std::move(u));
    }
}

struct PdfParseGraphicState {
    std::array<double, 6> ctm = {1.0, 0.0, 0.0, 1.0, 0.0, 0.0};
    double line_width_pt = 1.0;
    int stroke_r = 0;
    int stroke_g = 0;
    int stroke_b = 0;
    int fill_r = 0;
    int fill_g = 0;
    int fill_b = 0;
    bool has_stroke_color = false;
    bool has_fill_color = false;
};

static constexpr double kMmToPt = 72.0 / 25.4;

static bool ParseDouble(const std::string& token, double* out_value);

static std::string EscapePdfString(const std::string& src) {
    std::string out;
    out.reserve(src.size() + 8);
    for (char c : src) {
        switch (c) {
            case '(':
            case ')':
            case '\\':
                out.push_back('\\');
                out.push_back(c);
                break;
            case '\r':
            case '\n':
                out += "\\n";
                break;
            default:
                out.push_back(c);
                break;
        }
    }
    return out;
}

static std::string Lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

static bool HasNonAscii(const std::string& s) {
    for (unsigned char c : s) {
        if (c >= 0x80U) {
            return true;
        }
    }
    return false;
}

static std::string TrimAscii(const std::string& s) {
    size_t begin = 0;
    size_t end = s.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(s[begin]))) {
        ++begin;
    }
    while (end > begin && std::isspace(static_cast<unsigned char>(s[end - 1U]))) {
        --end;
    }
    return s.substr(begin, end - begin);
}

static std::string NormalizeExtractedToken(const std::string& token) {
    std::string t = TrimAscii(token);
    if (t.empty() || t == "(" || t == ")") {
        return "";
    }
    std::string out;
    out.reserve(t.size());
    bool in_space = false;
    for (unsigned char c : t) {
        if (std::isspace(c)) {
            if (!in_space) {
                out.push_back(' ');
                in_space = true;
            }
            continue;
        }
        in_space = false;
        out.push_back(static_cast<char>(c));
    }
    out = TrimAscii(out);
    // Collapse accidental spaces inside numeric tokens (e.g. "9 4,2 9 7.6 2" -> "94,297.62").
    std::string compact;
    compact.reserve(out.size());
    auto is_num_sym = [](char c) {
        return std::isdigit(static_cast<unsigned char>(c)) || c == '.' || c == ',' || c == '-' || c == ':' || c == '/';
    };
    for (size_t i = 0; i < out.size(); ++i) {
        const char c = out[i];
        if (c == ' ' && i > 0U && i + 1U < out.size() && is_num_sym(out[i - 1U]) && is_num_sym(out[i + 1U])) {
            continue;
        }
        compact.push_back(c);
    }
    return TrimAscii(compact);
}

static std::string EscapeXmlText(const std::string& input) {
    std::string out;
    out.reserve(input.size() + 16);
    for (char c : input) {
        switch (c) {
            case '&':
                out += "&amp;";
                break;
            case '<':
                out += "&lt;";
                break;
            case '>':
                out += "&gt;";
                break;
            case '"':
                out += "&quot;";
                break;
            case '\'':
                out += "&apos;";
                break;
            default:
                out.push_back(c);
                break;
        }
    }
    return out;
}

static bool IsCjkCodePoint(uint32_t cp) {
    return (cp >= 0x4E00U && cp <= 0x9FFFU) || (cp >= 0x3400U && cp <= 0x4DBFU);
}

static uint32_t FirstUtf8CodePoint(const std::string& s) {
    if (s.empty()) {
        return 0;
    }
    const unsigned char c0 = static_cast<unsigned char>(s[0]);
    if ((c0 & 0x80U) == 0) {
        return c0;
    }
    if ((c0 & 0xE0U) == 0xC0U && s.size() >= 2U) {
        return ((c0 & 0x1FU) << 6U) | (static_cast<unsigned char>(s[1]) & 0x3FU);
    }
    if ((c0 & 0xF0U) == 0xE0U && s.size() >= 3U) {
        return ((c0 & 0x0FU) << 12U) | ((static_cast<unsigned char>(s[1]) & 0x3FU) << 6U) |
               (static_cast<unsigned char>(s[2]) & 0x3FU);
    }
    if ((c0 & 0xF8U) == 0xF0U && s.size() >= 4U) {
        return ((c0 & 0x07U) << 18U) | ((static_cast<unsigned char>(s[1]) & 0x3FU) << 12U) |
               ((static_cast<unsigned char>(s[2]) & 0x3FU) << 6U) | (static_cast<unsigned char>(s[3]) & 0x3FU);
    }
    return 0;
}

static uint32_t LastUtf8CodePoint(const std::string& s) {
    if (s.empty()) {
        return 0;
    }
    size_t i = s.size() - 1U;
    while (i > 0U && (static_cast<unsigned char>(s[i]) & 0xC0U) == 0x80U) {
        --i;
    }
    return FirstUtf8CodePoint(s.substr(i));
}

static std::vector<std::string> MergeContinuationLines(const std::vector<std::string>& lines) {
    std::vector<std::string> out;
    for (const auto& raw_line : lines) {
        const std::string line = NormalizeExtractedToken(raw_line);
        if (line.empty()) {
            continue;
        }
        if (out.empty()) {
            out.push_back(line);
            continue;
        }
        std::string& prev = out.back();
        const uint32_t prev_last = LastUtf8CodePoint(prev);
        const uint32_t cur_first = FirstUtf8CodePoint(line);
        const bool prev_cjk = IsCjkCodePoint(prev_last);
        const bool cur_cjk = IsCjkCodePoint(cur_first);
        const bool prev_stop = prev_last == u'。' || prev_last == u'；' || prev_last == u'：' || prev_last == u'！' || prev_last == u'？';
        if (prev_cjk && cur_cjk && !prev_stop && prev.size() <= 12U && line.size() < 80U) {
            prev += line;
            continue;
        }
        out.push_back(line);
    }
    return out;
}

static std::string UnescapePdfString(const std::string& src) {
    std::string out;
    out.reserve(src.size());
    bool escaping = false;
    for (char c : src) {
        if (escaping) {
            switch (c) {
                case 'n':
                    out.push_back('\n');
                    break;
                case 'r':
                    out.push_back('\r');
                    break;
                case 't':
                    out.push_back('\t');
                    break;
                default:
                    out.push_back(c);
                    break;
            }
            escaping = false;
            continue;
        }
        if (c == '\\') {
            escaping = true;
        } else {
            out.push_back(c);
        }
    }
    if (escaping) {
        out.push_back('\\');
    }
    std::string cleaned;
    cleaned.reserve(out.size());
    for (unsigned char c : out) {
        if (c == 0) {
            continue;
        }
        if (c < 0x20 && c != '\n' && c != '\r' && c != '\t') {
            continue;
        }
        cleaned.push_back(static_cast<char>(c));
    }
    return cleaned;
}

static std::string TryInflateZlibStream(const std::string& compressed) {
#ifndef LIBOFD_HAVE_ZLIB
    if (compressed.empty()) {
        return "";
    }
    const auto stamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    const fs::path in_tmp = fs::temp_directory_path() / ("libofd_pdf_inflate_" + std::to_string(stamp) + ".bin");
    const fs::path out_tmp = fs::temp_directory_path() / ("libofd_pdf_inflate_" + std::to_string(stamp) + ".out");

    {
        std::ofstream out(in_tmp, std::ios::binary);
        if (!out.is_open()) {
            return "";
        }
        out.write(compressed.data(), static_cast<std::streamsize>(compressed.size()));
    }

    const std::string cmd = "python3 -c \"import zlib,sys;sys.stdout.buffer.write(zlib.decompress(open(sys.argv[1],'rb').read()))\" " +
                            in_tmp.string() + " > " + out_tmp.string() + " 2>/dev/null";
    const int rc = std::system(cmd.c_str());
    std::string out_text;
    if (rc == 0) {
        std::ifstream in(out_tmp, std::ios::binary);
        if (in.is_open()) {
            std::ostringstream ss;
            ss << in.rdbuf();
            out_text = ss.str();
        }
    }
    std::error_code ec;
    fs::remove(in_tmp, ec);
    fs::remove(out_tmp, ec);
    return out_text;
#else
    if (compressed.empty()) {
        return compressed;
    }
    z_stream zs{};
    zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(compressed.data()));
    zs.avail_in = static_cast<uInt>(compressed.size());
    if (inflateInit(&zs) != Z_OK) {
        return compressed;
    }
    std::string out;
    out.reserve(compressed.size() * 2U);
    int rc = Z_OK;
    char buffer[4096];
    while (rc == Z_OK) {
        zs.next_out = reinterpret_cast<Bytef*>(buffer);
        zs.avail_out = sizeof(buffer);
        rc = inflate(&zs, Z_NO_FLUSH);
        if (rc == Z_OK || rc == Z_STREAM_END) {
            const size_t produced = sizeof(buffer) - zs.avail_out;
            out.append(buffer, produced);
        }
    }
    inflateEnd(&zs);
    if (rc != Z_STREAM_END || out.empty()) {
        return compressed;
    }
    return out;
#endif
}

static std::vector<std::string> SplitLines(const std::string& text) {
    std::vector<std::string> lines;
    std::stringstream ss(text);
    std::string line;
    while (std::getline(ss, line)) {
        lines.push_back(line);
    }
    if (lines.empty()) {
        lines.emplace_back("");
    }
    return lines;
}

static std::string EncodeUtf8ToUtf16BeHex(const std::string& text) {
    std::ostringstream hex;
    hex << std::uppercase << std::hex;
    for (size_t i = 0; i < text.size();) {
        uint32_t cp = 0;
        const unsigned char c0 = static_cast<unsigned char>(text[i]);
        if ((c0 & 0x80U) == 0U) {
            cp = c0;
            ++i;
        } else if ((c0 & 0xE0U) == 0xC0U && i + 1U < text.size()) {
            cp = ((c0 & 0x1FU) << 6U) | (static_cast<unsigned char>(text[i + 1U]) & 0x3FU);
            i += 2U;
        } else if ((c0 & 0xF0U) == 0xE0U && i + 2U < text.size()) {
            cp = ((c0 & 0x0FU) << 12U) | ((static_cast<unsigned char>(text[i + 1U]) & 0x3FU) << 6U) |
                 (static_cast<unsigned char>(text[i + 2U]) & 0x3FU);
            i += 3U;
        } else if ((c0 & 0xF8U) == 0xF0U && i + 3U < text.size()) {
            cp = ((c0 & 0x07U) << 18U) | ((static_cast<unsigned char>(text[i + 1U]) & 0x3FU) << 12U) |
                 ((static_cast<unsigned char>(text[i + 2U]) & 0x3FU) << 6U) | (static_cast<unsigned char>(text[i + 3U]) & 0x3FU);
            i += 4U;
        } else {
            ++i;
            continue;
        }
        if (cp <= 0xFFFFU) {
            hex << std::setw(4) << std::setfill('0') << static_cast<unsigned int>(cp);
        } else {
            cp -= 0x10000U;
            const uint16_t hi = static_cast<uint16_t>(0xD800U + ((cp >> 10U) & 0x3FFU));
            const uint16_t lo = static_cast<uint16_t>(0xDC00U + (cp & 0x3FFU));
            hex << std::setw(4) << std::setfill('0') << static_cast<unsigned int>(hi);
            hex << std::setw(4) << std::setfill('0') << static_cast<unsigned int>(lo);
        }
    }
    return hex.str();
}

static std::string BuildPdfTextOperand(const std::string& text, bool use_cjk_hex) {
    if (!use_cjk_hex) {
        return "(" + EscapePdfString(text) + ")";
    }
    return "<" + EncodeUtf8ToUtf16BeHex(text) + ">";
}

static std::vector<std::string> SplitUtf8Codepoints(const std::string& text) {
    std::vector<std::string> out;
    for (size_t i = 0; i < text.size();) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        size_t len = 1;
        if ((c & 0x80U) == 0U) {
            len = 1;
        } else if ((c & 0xE0U) == 0xC0U) {
            len = 2;
        } else if ((c & 0xF0U) == 0xE0U) {
            len = 3;
        } else if ((c & 0xF8U) == 0xF0U) {
            len = 4;
        }
        if (i + len > text.size()) {
            len = 1;
        }
        out.push_back(text.substr(i, len));
        i += len;
    }
    return out;
}

static std::string BuildFallbackPageContentStream(
    const std::string& text, const std::string& default_alias, bool default_alias_is_cjk) {
    std::ostringstream ss;
    ss << "BT\n";
    ss << "/" << default_alias << " 11 Tf\n";
    ss << "50 800 Td\n";
    const auto lines = SplitLines(text);
    for (size_t i = 0; i < lines.size(); ++i) {
        ss << BuildPdfTextOperand(lines[i], default_alias_is_cjk) << " Tj\n";
        if (i + 1U < lines.size()) {
            ss << "0 -14 Td\n";
        }
    }
    ss << "ET\n";
    return ss.str();
}

static std::string BuildObjectPageContentStream(
    const OfdPageLayout& page, const std::string& default_alias, bool default_alias_is_cjk) {
    if (page.text_objects.empty()) {
        return BuildFallbackPageContentStream(page.fallback_text, default_alias, default_alias_is_cjk);
    }
    std::ostringstream ss;
    for (const auto& obj : page.text_objects) {
        const double y_pdf = std::max(0.0, page.height_pt - obj.y - obj.font_size);
        ss << "BT\n";
        ss << "/" << default_alias << " " << obj.font_size << " Tf\n";
        ss << "1 0 0 1 " << obj.x << " " << y_pdf << " Tm\n";
        ss << BuildPdfTextOperand(obj.text, default_alias_is_cjk) << " Tj\n";
        ss << "ET\n";
    }
    return ss.str();
}

static std::string BuildTextObjectsContentStream(
    const OfdPageLayout& page, const std::unordered_map<std::string, std::string>& font_alias_by_ofd_font_id,
    const std::unordered_map<std::string, std::string>& font_alias_by_base_font,
    const std::unordered_set<std::string>& cjk_font_aliases) {
    std::ostringstream ss;
    for (const auto& obj : page.text_objects) {
        std::string alias = "F1";
        if (!obj.ofd_font_id.empty()) {
            auto it = font_alias_by_ofd_font_id.find(obj.ofd_font_id);
            if (it != font_alias_by_ofd_font_id.end()) {
                alias = it->second;
            }
        }
        if (alias.empty()) {
            auto it = font_alias_by_base_font.find("Helvetica");
            if (it != font_alias_by_base_font.end()) {
                alias = it->second;
            }
        }
        const double y_pdf = std::max(0.0, page.height_pt - obj.y - obj.font_size);
        ss << "BT\n";
        ss << "/" << alias << " " << obj.font_size << " Tf\n";
        const bool use_cjk_hex = cjk_font_aliases.find(alias) != cjk_font_aliases.end();
        if (!obj.glyph_offsets_x.empty()) {
            const auto glyphs = SplitUtf8Codepoints(obj.text);
            const size_t n = std::min(glyphs.size(), obj.glyph_offsets_x.size());
            for (size_t i = 0; i < n; ++i) {
                const double x = obj.x + obj.glyph_offsets_x[i];
                ss << "1 0 0 1 " << x << " " << y_pdf << " Tm\n";
                ss << BuildPdfTextOperand(glyphs[i], use_cjk_hex) << " Tj\n";
            }
            if (glyphs.size() > n && n > 0U) {
                double x = obj.x + obj.glyph_offsets_x[n - 1U];
                for (size_t i = n; i < glyphs.size(); ++i) {
                    x += obj.font_size * 0.95;
                    ss << "1 0 0 1 " << x << " " << y_pdf << " Tm\n";
                    ss << BuildPdfTextOperand(glyphs[i], use_cjk_hex) << " Tj\n";
                }
            }
        } else {
            ss << "1 0 0 1 " << obj.x << " " << y_pdf << " Tm\n";
            ss << BuildPdfTextOperand(obj.text, use_cjk_hex) << " Tj\n";
        }
        ss << "ET\n";
    }
    return ss.str();
}

static bool ParseBoolAttr(const std::string& value, bool default_value) {
    if (value.empty()) {
        return default_value;
    }
    const std::string lower = Lower(value);
    if (lower == "true" || lower == "1") {
        return true;
    }
    if (lower == "false" || lower == "0") {
        return false;
    }
    return default_value;
}

static bool ParseRgbFromOfdValue(const std::string& value, int* out_r, int* out_g, int* out_b) {
    if (out_r == nullptr || out_g == nullptr || out_b == nullptr) {
        return false;
    }
    std::stringstream ss(value);
    int r = 0;
    int g = 0;
    int b = 0;
    if (!(ss >> r >> g >> b)) {
        return false;
    }
    r = std::max(0, std::min(255, r));
    g = std::max(0, std::min(255, g));
    b = std::max(0, std::min(255, b));
    *out_r = r;
    *out_g = g;
    *out_b = b;
    return true;
}

static std::string BuildPathObjectsContentStream(const OfdPageLayout& page) {
    std::ostringstream out;
    for (const auto& p : page.path_objects) {
        if (p.abbreviated_data.empty()) {
            continue;
        }
        if (p.has_stroke_color) {
            out << (p.stroke_r / 255.0) << " " << (p.stroke_g / 255.0) << " " << (p.stroke_b / 255.0) << " RG\n";
        }
        if (p.has_fill_color) {
            out << (p.fill_r / 255.0) << " " << (p.fill_g / 255.0) << " " << (p.fill_b / 255.0) << " rg\n";
        }
        out << p.line_width << " w\n";
        std::stringstream ss(p.abbreviated_data);
        std::string token;
        std::vector<std::string> tokens;
        while (ss >> token) {
            tokens.push_back(token);
        }
        size_t i = 0;
        while (i < tokens.size()) {
            const std::string op = tokens[i++];
            auto read_num = [&](double* v) -> bool {
                if (i >= tokens.size()) {
                    return false;
                }
                return ParseDouble(tokens[i++], v);
            };
            if (op == "M" || op == "m") {
                double x = 0.0;
                double y = 0.0;
                if (!read_num(&x) || !read_num(&y)) {
                    break;
                }
                const double px = p.boundary_x + x * kMmToPt;
                const double py = std::max(0.0, page.height_pt - (p.boundary_y + y * kMmToPt));
                out << px << " " << py << " m\n";
            } else if (op == "L" || op == "l") {
                double x = 0.0;
                double y = 0.0;
                if (!read_num(&x) || !read_num(&y)) {
                    break;
                }
                const double px = p.boundary_x + x * kMmToPt;
                const double py = std::max(0.0, page.height_pt - (p.boundary_y + y * kMmToPt));
                out << px << " " << py << " l\n";
            } else if (op == "Q" || op == "q") {
                double x1 = 0.0;
                double y1 = 0.0;
                double x2 = 0.0;
                double y2 = 0.0;
                double x3 = 0.0;
                double y3 = 0.0;
                if (!read_num(&x1) || !read_num(&y1) || !read_num(&x2) || !read_num(&y2) || !read_num(&x3) ||
                    !read_num(&y3)) {
                    break;
                }
                const double px1 = p.boundary_x + x1 * kMmToPt;
                const double py1 = std::max(0.0, page.height_pt - (p.boundary_y + y1 * kMmToPt));
                const double px2 = p.boundary_x + x2 * kMmToPt;
                const double py2 = std::max(0.0, page.height_pt - (p.boundary_y + y2 * kMmToPt));
                const double px3 = p.boundary_x + x3 * kMmToPt;
                const double py3 = std::max(0.0, page.height_pt - (p.boundary_y + y3 * kMmToPt));
                out << px1 << " " << py1 << " " << px2 << " " << py2 << " " << px3 << " " << py3 << " c\n";
            } else if (op == "B" || op == "b") {
                out << "h\n";
            }
        }
        if (p.fill && p.stroke) {
            out << "B\n";
        } else if (p.fill) {
            out << "f\n";
        } else if (p.stroke) {
            out << "S\n";
        }
    }
    return out.str();
}

struct PdfObject {
    int id = 0;
    std::string data;
};

struct ImageWriteItem {
    int object_id = 0;
    std::string image_name;
    enum class Encoding {
        kJpegDct,
        kPngFlate,
        kRaw
    } encoding = Encoding::kJpegDct;
    std::vector<unsigned char> data;
    int width_px = 0;
    int height_px = 0;
    int bits_per_component = 8;
    int color_components = 3;
    double x = 0.0;
    double y = 0.0;
    double width = 0.0;
    double height = 0.0;
};

struct PageWriteContext {
    int page_id = 0;
    int content_id = 0;
    std::vector<ImageWriteItem> images;
};

static bool ParseJpegSize(const std::vector<unsigned char>& data, int* out_w, int* out_h) {
    if (out_w == nullptr || out_h == nullptr) {
        return false;
    }
    if (data.size() < 4U || data[0] != 0xFF || data[1] != 0xD8) {
        return false;
    }
    size_t i = 2;
    while (i + 9U < data.size()) {
        if (data[i] != 0xFF) {
            ++i;
            continue;
        }
        const unsigned char marker = data[i + 1U];
        i += 2U;
        if (marker == 0xD9 || marker == 0xDA) {
            break;
        }
        if (i + 2U > data.size()) {
            break;
        }
        const size_t seg_len = (static_cast<size_t>(data[i]) << 8U) | static_cast<size_t>(data[i + 1U]);
        if (seg_len < 2U || i + seg_len > data.size()) {
            break;
        }
        if ((marker >= 0xC0 && marker <= 0xC3) || (marker >= 0xC5 && marker <= 0xC7) || (marker >= 0xC9 && marker <= 0xCB) ||
            (marker >= 0xCD && marker <= 0xCF)) {
            if (seg_len >= 7U) {
                *out_h = (static_cast<int>(data[i + 3U]) << 8) | static_cast<int>(data[i + 4U]);
                *out_w = (static_cast<int>(data[i + 5U]) << 8) | static_cast<int>(data[i + 6U]);
                return (*out_w > 0 && *out_h > 0);
            }
            return false;
        }
        i += seg_len;
    }
    // Fallback for non-standard or minimized JPEG streams:
    // keep conversion available even if exact dimensions are unavailable.
    *out_w = 1;
    *out_h = 1;
    return true;
}

static bool ReadBinaryFile(const fs::path& path, std::vector<unsigned char>* out_data) {
    if (out_data == nullptr) {
        return false;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        return false;
    }
    out_data->assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

static uint32_t ReadU32BE(const unsigned char* p) {
    return (static_cast<uint32_t>(p[0]) << 24U) | (static_cast<uint32_t>(p[1]) << 16U) | (static_cast<uint32_t>(p[2]) << 8U) |
           static_cast<uint32_t>(p[3]);
}

static fs::path NormalizeOfdPath(const std::string& raw_path) {
    std::string v = raw_path;
    while (!v.empty() && (v.front() == '/' || v.front() == '\\')) {
        v.erase(v.begin());
    }
    return fs::path(v).lexically_normal();
}

static fs::path ResolveOfdPath(const fs::path& package_root, const fs::path& document_dir, const std::string& raw_path) {
    const fs::path normalized = NormalizeOfdPath(raw_path);
    const fs::path from_doc = document_dir / normalized;
    if (fs::exists(from_doc)) {
        return from_doc;
    }
    return package_root / normalized;
}

static uint32_t ReadU32LE(const unsigned char* p) {
    return (static_cast<uint32_t>(p[3]) << 24U) | (static_cast<uint32_t>(p[2]) << 16U) | (static_cast<uint32_t>(p[1]) << 8U) |
           static_cast<uint32_t>(p[0]);
}

static int32_t ReadS32LE(const unsigned char* p) {
    return static_cast<int32_t>(ReadU32LE(p));
}

static bool ParsePngToPdfFlate(
    const std::vector<unsigned char>& png, int* out_w, int* out_h, int* out_bpc, int* out_components,
    std::vector<unsigned char>* out_idat) {
    if (out_w == nullptr || out_h == nullptr || out_bpc == nullptr || out_components == nullptr || out_idat == nullptr) {
        return false;
    }
    static const unsigned char sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    if (png.size() < 8U || !std::equal(std::begin(sig), std::end(sig), png.begin())) {
        return false;
    }
    size_t offset = 8U;
    int width = 0;
    int height = 0;
    int bit_depth = 0;
    int color_type = -1;
    int compression = -1;
    int filter = -1;
    int interlace = -1;
    out_idat->clear();

    while (offset + 12U <= png.size()) {
        const uint32_t len = ReadU32BE(&png[offset]);
        offset += 4U;
        if (offset + 4U > png.size()) {
            return false;
        }
        const std::string type(reinterpret_cast<const char*>(&png[offset]), 4U);
        offset += 4U;
        if (offset + len + 4U > png.size()) {
            return false;
        }
        const unsigned char* payload = &png[offset];
        if (type == "IHDR") {
            if (len < 13U) {
                return false;
            }
            width = static_cast<int>(ReadU32BE(payload));
            height = static_cast<int>(ReadU32BE(payload + 4U));
            bit_depth = static_cast<int>(payload[8U]);
            color_type = static_cast<int>(payload[9U]);
            compression = static_cast<int>(payload[10U]);
            filter = static_cast<int>(payload[11U]);
            interlace = static_cast<int>(payload[12U]);
        } else if (type == "IDAT") {
            out_idat->insert(out_idat->end(), payload, payload + len);
        } else if (type == "IEND") {
            break;
        }
        offset += len + 4U; // skip payload + crc
    }

    if (width <= 0 || height <= 0 || out_idat->empty()) {
        return false;
    }
    if (compression != 0 || filter != 0 || interlace != 0) {
        return false;
    }
    if (!(bit_depth == 8 || bit_depth == 16)) {
        return false;
    }
    int comps = 0;
    if (color_type == 0) {
        comps = 1;
    } else if (color_type == 2) {
        comps = 3;
    } else {
        // Keep scope manageable for now (no palette/alpha PNG here).
        return false;
    }
    *out_w = width;
    *out_h = height;
    *out_bpc = bit_depth;
    *out_components = comps;
    return true;
}

static bool ParsePdfLiteralBytes(const std::string& src, size_t start_pos, std::vector<unsigned char>* out_bytes, size_t* out_next_pos) {
    if (out_bytes == nullptr || start_pos >= src.size() || src[start_pos] != '(') {
        return false;
    }
    out_bytes->clear();
    bool escaping = false;
    int nested = 1;
    for (size_t i = start_pos + 1U; i < src.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(src[i]);
        if (escaping) {
            escaping = false;
            if (c >= '0' && c <= '7') {
                int val = c - '0';
                size_t j = i + 1U;
                for (int k = 0; k < 2 && j < src.size(); ++k, ++j) {
                    const unsigned char oc = static_cast<unsigned char>(src[j]);
                    if (oc < '0' || oc > '7') {
                        break;
                    }
                    val = (val * 8) + (oc - '0');
                    i = j;
                }
                out_bytes->push_back(static_cast<unsigned char>(val & 0xFF));
                continue;
            }
            switch (c) {
                case 'n':
                    out_bytes->push_back('\n');
                    break;
                case 'r':
                    out_bytes->push_back('\r');
                    break;
                case 't':
                    out_bytes->push_back('\t');
                    break;
                case 'b':
                    out_bytes->push_back('\b');
                    break;
                case 'f':
                    out_bytes->push_back('\f');
                    break;
                default:
                    out_bytes->push_back(c);
                    break;
            }
            continue;
        }
        if (c == '\\') {
            escaping = true;
            continue;
        }
        if (c == '(') {
            ++nested;
            out_bytes->push_back(c);
            continue;
        }
        if (c == ')') {
            --nested;
            if (nested == 0) {
                if (out_next_pos != nullptr) {
                    *out_next_pos = i + 1U;
                }
                return true;
            }
            out_bytes->push_back(c);
            continue;
        }
        out_bytes->push_back(c);
    }
    return false;
}

static bool ParsePdfImageIntAttr(const std::string& obj_body, const std::string& key, int* out_value) {
    if (out_value == nullptr) {
        return false;
    }
    std::smatch m;
    const std::regex re("/" + key + R"(\s+(\d+))");
    if (!std::regex_search(obj_body, m, re)) {
        return false;
    }
    *out_value = std::atoi(m[1].str().c_str());
    return *out_value > 0;
}

static bool EncodeBmp24(int w, int h, const std::vector<unsigned char>& rgb, std::vector<unsigned char>* out_bmp) {
    if (out_bmp == nullptr || w <= 0 || h <= 0) {
        return false;
    }
    const size_t row_bytes = static_cast<size_t>(w) * 3U;
    const size_t stride = (row_bytes + 3U) & ~3U;
    const size_t pixel_bytes = stride * static_cast<size_t>(h);
    const size_t total = 14U + 40U + pixel_bytes;
    if (rgb.size() < static_cast<size_t>(w) * static_cast<size_t>(h) * 3U) {
        return false;
    }
    out_bmp->assign(total, 0U);
    auto put_u16 = [&](size_t off, uint16_t v) {
        (*out_bmp)[off + 0U] = static_cast<unsigned char>(v & 0xFFU);
        (*out_bmp)[off + 1U] = static_cast<unsigned char>((v >> 8U) & 0xFFU);
    };
    auto put_u32 = [&](size_t off, uint32_t v) {
        (*out_bmp)[off + 0U] = static_cast<unsigned char>(v & 0xFFU);
        (*out_bmp)[off + 1U] = static_cast<unsigned char>((v >> 8U) & 0xFFU);
        (*out_bmp)[off + 2U] = static_cast<unsigned char>((v >> 16U) & 0xFFU);
        (*out_bmp)[off + 3U] = static_cast<unsigned char>((v >> 24U) & 0xFFU);
    };
    (*out_bmp)[0] = 'B';
    (*out_bmp)[1] = 'M';
    put_u32(2U, static_cast<uint32_t>(total));
    put_u32(10U, 54U);
    put_u32(14U, 40U);
    put_u32(18U, static_cast<uint32_t>(w));
    put_u32(22U, static_cast<uint32_t>(h));
    put_u16(26U, 1U);
    put_u16(28U, 24U);
    put_u32(34U, static_cast<uint32_t>(pixel_bytes));
    for (int y = 0; y < h; ++y) {
        const size_t src_row = static_cast<size_t>(h - 1 - y) * static_cast<size_t>(w) * 3U;
        const size_t dst_row = 54U + static_cast<size_t>(y) * stride;
        for (int x = 0; x < w; ++x) {
            const size_t s = src_row + static_cast<size_t>(x) * 3U;
            const size_t d = dst_row + static_cast<size_t>(x) * 3U;
            // BMP uses BGR order
            (*out_bmp)[d + 0U] = rgb[s + 2U];
            (*out_bmp)[d + 1U] = rgb[s + 1U];
            (*out_bmp)[d + 2U] = rgb[s + 0U];
        }
    }
    return true;
}

static bool DecodePdfRawImageToRgb(
    const std::string& obj_body, const std::vector<unsigned char>& raw_pixels, int* out_w, int* out_h,
    std::vector<unsigned char>* out_rgb) {
    if (out_w == nullptr || out_h == nullptr || out_rgb == nullptr || raw_pixels.empty()) {
        return false;
    }
    int w = 0;
    int h = 0;
    int bpc = 8;
    if (!ParsePdfImageIntAttr(obj_body, "Width", &w) || !ParsePdfImageIntAttr(obj_body, "Height", &h)) {
        return false;
    }
    ParsePdfImageIntAttr(obj_body, "BitsPerComponent", &bpc);
    if (bpc != 8) {
        return false;
    }
    *out_w = w;
    *out_h = h;
    // Case 1: /ColorSpace /DeviceRGB with raw RGB bytes
    if (obj_body.find("/ColorSpace/DeviceRGB") != std::string::npos || obj_body.find("/ColorSpace /DeviceRGB") != std::string::npos) {
        const size_t need = static_cast<size_t>(w) * static_cast<size_t>(h) * 3U;
        if (raw_pixels.size() < need) {
            return false;
        }
        out_rgb->assign(raw_pixels.begin(), raw_pixels.begin() + static_cast<std::ptrdiff_t>(need));
        return true;
    }
    // Case 2: /Indexed /DeviceRGB ... (palette + 1-byte indices)
    const size_t indexed_pos = obj_body.find("/Indexed/DeviceRGB");
    const size_t indexed_pos2 = obj_body.find("/Indexed /DeviceRGB");
    const size_t base_pos = indexed_pos != std::string::npos ? indexed_pos : indexed_pos2;
    if (base_pos != std::string::npos) {
        const size_t lpar = obj_body.find('(', base_pos);
        if (lpar == std::string::npos) {
            return false;
        }
        std::vector<unsigned char> palette;
        if (!ParsePdfLiteralBytes(obj_body, lpar, &palette, nullptr) || palette.size() < 3U) {
            return false;
        }
        const size_t need = static_cast<size_t>(w) * static_cast<size_t>(h);
        if (raw_pixels.size() < need) {
            return false;
        }
        out_rgb->assign(need * 3U, 0U);
        for (size_t i = 0; i < need; ++i) {
            const size_t idx = static_cast<size_t>(raw_pixels[i]) * 3U;
            const size_t o = i * 3U;
            if (idx + 2U < palette.size()) {
                (*out_rgb)[o + 0U] = palette[idx + 0U];
                (*out_rgb)[o + 1U] = palette[idx + 1U];
                (*out_rgb)[o + 2U] = palette[idx + 2U];
            }
        }
        return true;
    }
    return false;
}

static bool ConvertPdfRawImageToBmp(
    const std::string& obj_body, const std::vector<unsigned char>& raw_pixels, std::vector<unsigned char>* out_bmp) {
    if (out_bmp == nullptr) {
        return false;
    }
    int w = 0;
    int h = 0;
    std::vector<unsigned char> rgb;
    if (!DecodePdfRawImageToRgb(obj_body, raw_pixels, &w, &h, &rgb)) {
        return false;
    }
    return EncodeBmp24(w, h, rgb, out_bmp);
}

static bool EncodeJpegViaPythonPillow(int w, int h, const std::vector<unsigned char>& rgb, std::vector<unsigned char>* out_jpeg) {
    if (out_jpeg == nullptr || w <= 0 || h <= 0 || rgb.size() < static_cast<size_t>(w) * static_cast<size_t>(h) * 3U) {
        return false;
    }
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path tmp_dir = fs::temp_directory_path() / ("libofd_jpeg_" + std::to_string(now));
    std::error_code ec;
    fs::create_directories(tmp_dir, ec);
    if (ec) {
        return false;
    }
    const fs::path ppm_path = tmp_dir / "in.ppm";
    const fs::path jpg_path = tmp_dir / "out.jpg";
    {
        std::ofstream ppm(ppm_path, std::ios::binary);
        if (!ppm.is_open()) {
            fs::remove_all(tmp_dir, ec);
            return false;
        }
        ppm << "P6\n" << w << " " << h << "\n255\n";
        ppm.write(reinterpret_cast<const char*>(rgb.data()), static_cast<std::streamsize>(static_cast<size_t>(w) * static_cast<size_t>(h) * 3U));
    }
    const std::string cmd =
        "python3 - <<'PY'\n"
        "from PIL import Image\n"
        "im=Image.open(r'" + ppm_path.string() + "')\n"
        "im.save(r'" + jpg_path.string() + "', format='JPEG', quality=92)\n"
        "PY";
    const int rc = std::system(cmd.c_str());
    if (rc != 0 || !fs::exists(jpg_path)) {
        fs::remove_all(tmp_dir, ec);
        return false;
    }
    std::ifstream in(jpg_path, std::ios::binary);
    if (!in.is_open()) {
        fs::remove_all(tmp_dir, ec);
        return false;
    }
    out_jpeg->assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    fs::remove_all(tmp_dir, ec);
    return out_jpeg->size() >= 4U && (*out_jpeg)[0] == 0xFFU && (*out_jpeg)[1] == 0xD8U;
}

#ifdef LIBOFD_HAVE_ZLIB
static bool EncodePng24(int w, int h, const std::vector<unsigned char>& rgb, std::vector<unsigned char>* out_png) {
    if (out_png == nullptr || w <= 0 || h <= 0 || rgb.size() < static_cast<size_t>(w) * static_cast<size_t>(h) * 3U) {
        return false;
    }
    std::vector<unsigned char> raw;
    const size_t row_rgb = static_cast<size_t>(w) * 3U;
    raw.reserve((row_rgb + 1U) * static_cast<size_t>(h));
    for (int y = 0; y < h; ++y) {
        raw.push_back(0U);  // filter: none
        const size_t off = static_cast<size_t>(y) * row_rgb;
        raw.insert(raw.end(), rgb.begin() + static_cast<std::ptrdiff_t>(off), rgb.begin() + static_cast<std::ptrdiff_t>(off + row_rgb));
    }
    uLongf zlen = compressBound(static_cast<uLong>(raw.size()));
    std::vector<unsigned char> zbuf(static_cast<size_t>(zlen));
    if (compress2(zbuf.data(), &zlen, raw.data(), static_cast<uLong>(raw.size()), Z_BEST_COMPRESSION) != Z_OK) {
        return false;
    }
    zbuf.resize(static_cast<size_t>(zlen));
    out_png->clear();
    const unsigned char sig[8] = {0x89U, 'P', 'N', 'G', '\r', '\n', 0x1AU, '\n'};
    out_png->insert(out_png->end(), sig, sig + 8U);
    auto put_u32_be = [&](uint32_t v) {
        out_png->push_back(static_cast<unsigned char>((v >> 24U) & 0xFFU));
        out_png->push_back(static_cast<unsigned char>((v >> 16U) & 0xFFU));
        out_png->push_back(static_cast<unsigned char>((v >> 8U) & 0xFFU));
        out_png->push_back(static_cast<unsigned char>(v & 0xFFU));
    };
    auto append_chunk = [&](const char type[4], const std::vector<unsigned char>& data) {
        put_u32_be(static_cast<uint32_t>(data.size()));
        const size_t type_pos = out_png->size();
        out_png->insert(out_png->end(), type, type + 4);
        out_png->insert(out_png->end(), data.begin(), data.end());
        uLong crc = crc32(0L, Z_NULL, 0);
        crc = crc32(crc, out_png->data() + static_cast<std::ptrdiff_t>(type_pos), static_cast<uInt>(4 + data.size()));
        put_u32_be(static_cast<uint32_t>(crc));
    };
    std::vector<unsigned char> ihdr(13U, 0U);
    ihdr[0] = static_cast<unsigned char>((static_cast<uint32_t>(w) >> 24U) & 0xFFU);
    ihdr[1] = static_cast<unsigned char>((static_cast<uint32_t>(w) >> 16U) & 0xFFU);
    ihdr[2] = static_cast<unsigned char>((static_cast<uint32_t>(w) >> 8U) & 0xFFU);
    ihdr[3] = static_cast<unsigned char>(static_cast<uint32_t>(w) & 0xFFU);
    ihdr[4] = static_cast<unsigned char>((static_cast<uint32_t>(h) >> 24U) & 0xFFU);
    ihdr[5] = static_cast<unsigned char>((static_cast<uint32_t>(h) >> 16U) & 0xFFU);
    ihdr[6] = static_cast<unsigned char>((static_cast<uint32_t>(h) >> 8U) & 0xFFU);
    ihdr[7] = static_cast<unsigned char>(static_cast<uint32_t>(h) & 0xFFU);
    ihdr[8] = 8U;   // bit depth
    ihdr[9] = 2U;   // truecolor
    ihdr[10] = 0U;  // compression
    ihdr[11] = 0U;  // filter
    ihdr[12] = 0U;  // interlace
    append_chunk("IHDR", ihdr);
    append_chunk("IDAT", zbuf);
    append_chunk("IEND", {});
    return true;
}

static bool ConvertPdfRawImageToPng(
    const std::string& obj_body, const std::vector<unsigned char>& raw_pixels, std::vector<unsigned char>* out_png) {
    if (out_png == nullptr) {
        return false;
    }
    int w = 0;
    int h = 0;
    std::vector<unsigned char> rgb;
    if (!DecodePdfRawImageToRgb(obj_body, raw_pixels, &w, &h, &rgb)) {
        return false;
    }
    return EncodePng24(w, h, rgb, out_png);
}
#endif

static bool HasPdfNameEntry(const std::string& body, const std::string& key, const std::string& name) {
    const std::regex re("/" + key + R"(\s*/)" + name + R"(\b)");
    return std::regex_search(body, re);
}

static std::string ExtractDecodedPdfObjectStream(const std::string& obj_body);

static std::string StripPdfFontSubsetPrefix(const std::string& raw_name) {
    const size_t plus = raw_name.find('+');
    if (plus != std::string::npos && plus + 1U < raw_name.size()) {
        return raw_name.substr(plus + 1U);
    }
    return raw_name;
}

static std::string NormalizeOfdFontName(const std::string& raw_name) {
    std::string out = StripPdfFontSubsetPrefix(raw_name);
    const size_t comma = out.find(',');
    if (comma != std::string::npos) {
        out = out.substr(0, comma);
    }
    const size_t dash = out.find('-');
    if (dash != std::string::npos && dash + 1U < out.size()) {
        const std::string suffix = Lower(out.substr(dash + 1U));
        if (suffix == "bold" || suffix == "italic" || suffix == "regular") {
            out = out.substr(0, dash);
        }
    }
    if (out.empty()) {
        out = "SimSun";
    }
    return out;
}

static void ExtractEmbeddedFontsFromPdf(const std::string& pdf_content, std::vector<PdfEmbeddedFontAsset>* out_fonts) {
    if (out_fonts == nullptr) {
        return;
    }
    out_fonts->clear();

    struct PdfObj {
        int id = 0;
        std::string body;
    };
    std::vector<PdfObj> objects;
    std::unordered_map<int, std::string> object_map;
    const std::regex header_re(R"((\d+)\s+(\d+)\s+obj)");
    std::smatch m;
    std::string::const_iterator search_begin = pdf_content.cbegin();
    while (std::regex_search(search_begin, pdf_content.cend(), m, header_re)) {
        const int obj_id = std::atoi(m[1].str().c_str());
        const size_t header_offset = static_cast<size_t>(m.position(0) + (search_begin - pdf_content.cbegin()));
        const size_t body_begin = header_offset + static_cast<size_t>(m.length(0));
        const size_t body_end = pdf_content.find("endobj", body_begin);
        if (body_end == std::string::npos || body_end <= body_begin) {
            break;
        }
        const std::string obj_body = pdf_content.substr(body_begin, body_end - body_begin);
        objects.push_back(PdfObj{obj_id, obj_body});
        object_map[obj_id] = obj_body;
        search_begin = pdf_content.cbegin() + static_cast<std::ptrdiff_t>(body_end + 6U);
    }

    auto parse_objstm_int = [](const std::string& body, const std::string& key, int* out) -> bool {
        if (out == nullptr) {
            return false;
        }
        std::smatch sm;
        const std::regex re("/" + key + R"(\s+(\d+))");
        if (!std::regex_search(body, sm, re)) {
            return false;
        }
        *out = std::atoi(sm[1].str().c_str());
        return *out > 0;
    };
    {
        std::vector<PdfObj> embedded;
        for (const auto& obj : objects) {
            if (!HasPdfNameEntry(obj.body, "Type", "ObjStm")) {
                continue;
            }
            int n = 0;
            int first = 0;
            if (!parse_objstm_int(obj.body, "N", &n) || !parse_objstm_int(obj.body, "First", &first)) {
                continue;
            }
            const std::string stream = ExtractDecodedPdfObjectStream(obj.body);
            if (stream.empty() || first <= 0 || static_cast<size_t>(first) >= stream.size()) {
                continue;
            }
            const std::string header = stream.substr(0, static_cast<size_t>(first));
            std::stringstream hs(header);
            std::vector<int> nums;
            int v = 0;
            while (hs >> v) {
                nums.push_back(v);
            }
            if (nums.size() < static_cast<size_t>(n * 2)) {
                continue;
            }
            for (int i = 0; i < n; ++i) {
                const int eid = nums[static_cast<size_t>(i * 2)];
                const int off = nums[static_cast<size_t>(i * 2 + 1)];
                if (off < 0) {
                    continue;
                }
                const size_t st = static_cast<size_t>(first + off);
                const size_t ed =
                    (i + 1 < n) ? static_cast<size_t>(first + nums[static_cast<size_t>((i + 1) * 2 + 1)]) : stream.size();
                if (st >= stream.size() || ed <= st || ed > stream.size()) {
                    continue;
                }
                std::string emb = TrimAscii(stream.substr(st, ed - st));
                if (emb.empty()) {
                    continue;
                }
                object_map[eid] = emb;
                embedded.push_back(PdfObj{eid, std::move(emb)});
            }
        }
        objects.insert(objects.end(), embedded.begin(), embedded.end());
    }

    struct Candidate {
        std::string font_name;
        std::vector<unsigned char> data;
    };
    std::vector<Candidate> candidates;
    candidates.reserve(8);
    for (const auto& kv : object_map) {
        std::smatch fm;
        const std::regex ff2_re(R"(/FontFile2\s+(\d+)\s+\d+\s+R)");
        if (!std::regex_search(kv.second, fm, ff2_re)) {
            continue;
        }
        const int font_stream_id = std::atoi(fm[1].str().c_str());
        auto fit = object_map.find(font_stream_id);
        if (fit == object_map.end()) {
            continue;
        }
        std::string font_name = "EmbeddedPDF";
        std::smatch fnm;
        const std::regex fn_re(R"(/FontName\s*/([A-Za-z0-9_\-+]+))");
        if (std::regex_search(kv.second, fnm, fn_re)) {
            font_name = NormalizeOfdFontName(fnm[1].str());
        }
        const std::string font_blob = ExtractDecodedPdfObjectStream(fit->second);
        if (font_blob.size() < 1024U) {
            continue;
        }
        Candidate c;
        c.font_name = font_name;
        c.data.assign(font_blob.begin(), font_blob.end());
        candidates.push_back(std::move(c));
    }
    if (candidates.empty()) {
        return;
    }
    auto contains_ci = [](const std::string& s, const std::string& key) {
        std::string a = Lower(s);
        std::string b = Lower(key);
        return a.find(b) != std::string::npos;
    };
    struct ScoredCandidate {
        std::string font_name;
        std::vector<unsigned char> data;
        int score = 0;
    };
    std::vector<ScoredCandidate> scored;
    scored.reserve(candidates.size());
    for (size_t i = 0; i < candidates.size(); ++i) {
        const std::string& n = candidates[i].font_name;
        int score = 0;
        if (contains_ci(n, "simsun")) {
            score += 500;
        }
        if (contains_ci(n, "fangsong")) {
            score += 70;
        }
        if (contains_ci(n, "song") && !contains_ci(n, "simsun") && !contains_ci(n, "fangsong")) {
            score += 40;
        }
        if (contains_ci(n, "hei") || contains_ci(n, "kai")) {
            score += 20;
        }
        score += static_cast<int>(std::min<size_t>(candidates[i].data.size() / 1024U, 50U));
        scored.push_back(ScoredCandidate{candidates[i].font_name, std::move(candidates[i].data), score});
    }
    std::sort(
        scored.begin(), scored.end(), [](const ScoredCandidate& a, const ScoredCandidate& b) {
            if (a.score != b.score) {
                return a.score > b.score;
            }
            return a.data.size() > b.data.size();
        });

    auto same_family = [&](const std::string& a, const std::string& b) {
        return Lower(a) == Lower(b);
    };

    PdfEmbeddedFontAsset primary;
    primary.font_name = scored.front().font_name;
    primary.file_name = "Font_1.ttf";
    primary.data = std::move(scored.front().data);
    out_fonts->push_back(std::move(primary));

    for (size_t i = 1; i < scored.size(); ++i) {
        if (same_family(scored[i].font_name, out_fonts->front().font_name)) {
            continue;
        }
        PdfEmbeddedFontAsset fallback;
        fallback.font_name = scored[i].font_name;
        fallback.file_name = "Font_2.ttf";
        fallback.data = std::move(scored[i].data);
        out_fonts->push_back(std::move(fallback));
        break;
    }
}

static bool ParsePdfRect4(const std::string& body, const std::string& key, std::array<double, 4>* out_rect) {
    if (out_rect == nullptr) {
        return false;
    }
    std::smatch m;
    const std::regex re(
        "/" + key + R"(\s*\[\s*([-+]?[0-9]*\.?[0-9]+)\s+([-+]?[0-9]*\.?[0-9]+)\s+([-+]?[0-9]*\.?[0-9]+)\s+([-+]?[0-9]*\.?[0-9]+)\s*\])");
    if (!std::regex_search(body, m, re)) {
        return false;
    }
    (*out_rect)[0] = std::atof(m[1].str().c_str());
    (*out_rect)[1] = std::atof(m[2].str().c_str());
    (*out_rect)[2] = std::atof(m[3].str().c_str());
    (*out_rect)[3] = std::atof(m[4].str().c_str());
    return true;
}

static int PaethPredictor(int a, int b, int c) {
    const int p = a + b - c;
    const int pa = std::abs(p - a);
    const int pb = std::abs(p - b);
    const int pc = std::abs(p - c);
    if (pa <= pb && pa <= pc) {
        return a;
    }
    if (pb <= pc) {
        return b;
    }
    return c;
}

static bool ParsePngRgbaToRawRgb(
    const std::vector<unsigned char>& png, int* out_w, int* out_h, int* out_bpc, int* out_components,
    std::vector<unsigned char>* out_raw_rgb) {
#ifndef LIBOFD_HAVE_ZLIB
    (void)png;
    (void)out_w;
    (void)out_h;
    (void)out_bpc;
    (void)out_components;
    (void)out_raw_rgb;
    return false;
#else
    if (out_w == nullptr || out_h == nullptr || out_bpc == nullptr || out_components == nullptr || out_raw_rgb == nullptr) {
        return false;
    }
    static const unsigned char sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    if (png.size() < 8U || !std::equal(std::begin(sig), std::end(sig), png.begin())) {
        return false;
    }
    size_t offset = 8U;
    int width = 0;
    int height = 0;
    int bit_depth = 0;
    int color_type = -1;
    int compression = -1;
    int filter = -1;
    int interlace = -1;
    std::vector<unsigned char> idat;
    while (offset + 12U <= png.size()) {
        const uint32_t len = ReadU32BE(&png[offset]);
        offset += 4U;
        if (offset + 4U > png.size()) {
            return false;
        }
        const std::string type(reinterpret_cast<const char*>(&png[offset]), 4U);
        offset += 4U;
        if (offset + len + 4U > png.size()) {
            return false;
        }
        const unsigned char* payload = &png[offset];
        if (type == "IHDR") {
            if (len < 13U) {
                return false;
            }
            width = static_cast<int>(ReadU32BE(payload));
            height = static_cast<int>(ReadU32BE(payload + 4U));
            bit_depth = static_cast<int>(payload[8U]);
            color_type = static_cast<int>(payload[9U]);
            compression = static_cast<int>(payload[10U]);
            filter = static_cast<int>(payload[11U]);
            interlace = static_cast<int>(payload[12U]);
        } else if (type == "IDAT") {
            idat.insert(idat.end(), payload, payload + len);
        } else if (type == "IEND") {
            break;
        }
        offset += len + 4U;
    }
    if (width <= 0 || height <= 0 || bit_depth != 8 || color_type != 6 || compression != 0 || filter != 0 || interlace != 0 ||
        idat.empty()) {
        return false;
    }

    const size_t stride = static_cast<size_t>(width) * 4U;
    const size_t expected = static_cast<size_t>(height) * (1U + stride);
    std::vector<unsigned char> inflated(expected);
    uLongf out_len = static_cast<uLongf>(inflated.size());
    if (uncompress(inflated.data(), &out_len, idat.data(), static_cast<uLong>(idat.size())) != Z_OK || out_len < expected) {
        return false;
    }
    inflated.resize(static_cast<size_t>(out_len));
    if (inflated.size() < expected) {
        return false;
    }

    std::vector<unsigned char> rgba(static_cast<size_t>(width) * static_cast<size_t>(height) * 4U, 0U);
    std::vector<unsigned char> prev_row(stride, 0U);
    std::vector<unsigned char> cur_row(stride, 0U);
    size_t in_off = 0;
    for (int y = 0; y < height; ++y) {
        const int filter_type = inflated[in_off++];
        if (in_off + stride > inflated.size()) {
            return false;
        }
        const unsigned char* src = &inflated[in_off];
        in_off += stride;
        for (size_t x = 0; x < stride; ++x) {
            const int left = (x >= 4U) ? cur_row[x - 4U] : 0;
            const int up = prev_row[x];
            const int up_left = (x >= 4U) ? prev_row[x - 4U] : 0;
            int value = src[x];
            switch (filter_type) {
                case 0:
                    break;
                case 1:
                    value = (value + left) & 0xFF;
                    break;
                case 2:
                    value = (value + up) & 0xFF;
                    break;
                case 3:
                    value = (value + ((left + up) / 2)) & 0xFF;
                    break;
                case 4:
                    value = (value + PaethPredictor(left, up, up_left)) & 0xFF;
                    break;
                default:
                    return false;
            }
            cur_row[x] = static_cast<unsigned char>(value);
        }
        std::copy(cur_row.begin(), cur_row.end(), rgba.begin() + static_cast<size_t>(y) * stride);
        std::copy(cur_row.begin(), cur_row.end(), prev_row.begin());
    }

    out_raw_rgb->resize(static_cast<size_t>(width) * static_cast<size_t>(height) * 3U);
    for (size_t i = 0, j = 0; i + 3U < rgba.size(); i += 4U) {
        (*out_raw_rgb)[j++] = rgba[i];
        (*out_raw_rgb)[j++] = rgba[i + 1U];
        (*out_raw_rgb)[j++] = rgba[i + 2U];
    }
    *out_w = width;
    *out_h = height;
    *out_bpc = 8;
    *out_components = 3;
    return true;
#endif
}

static bool ParseBmpToRawRgb(
    const std::vector<unsigned char>& bmp, int* out_w, int* out_h, int* out_bpc, int* out_components,
    std::vector<unsigned char>* out_raw_rgb) {
    if (out_w == nullptr || out_h == nullptr || out_bpc == nullptr || out_components == nullptr || out_raw_rgb == nullptr) {
        return false;
    }
    if (bmp.size() < 54U || bmp[0] != 'B' || bmp[1] != 'M') {
        return false;
    }
    const uint32_t pixel_offset = ReadU32LE(&bmp[10]);
    const uint32_t dib_size = ReadU32LE(&bmp[14]);
    if (dib_size < 40U || bmp.size() < 14U + dib_size) {
        return false;
    }
    const int32_t width = ReadS32LE(&bmp[18]);
    const int32_t height_signed = ReadS32LE(&bmp[22]);
    const uint16_t planes = static_cast<uint16_t>(bmp[26] | (bmp[27] << 8U));
    const uint16_t bpp = static_cast<uint16_t>(bmp[28] | (bmp[29] << 8U));
    const uint32_t compression = ReadU32LE(&bmp[30]);
    if (width <= 0 || height_signed == 0 || planes != 1U || compression != 0U) {
        return false;
    }
    if (!(bpp == 24U || bpp == 32U)) {
        return false;
    }
    const int height = std::abs(height_signed);
    const bool bottom_up = (height_signed > 0);
    const size_t bytes_per_px = (bpp == 24U) ? 3U : 4U;
    const size_t src_stride = ((static_cast<size_t>(width) * bytes_per_px + 3U) / 4U) * 4U;
    if (pixel_offset >= bmp.size() || pixel_offset + src_stride * static_cast<size_t>(height) > bmp.size()) {
        return false;
    }
    out_raw_rgb->assign(static_cast<size_t>(width) * static_cast<size_t>(height) * 3U, 0U);

    for (int y = 0; y < height; ++y) {
        const size_t src_row_index = bottom_up ? static_cast<size_t>(height - 1 - y) : static_cast<size_t>(y);
        const unsigned char* row = &bmp[pixel_offset + src_row_index * src_stride];
        for (int x = 0; x < width; ++x) {
            const size_t src_i = static_cast<size_t>(x) * bytes_per_px;
            const unsigned char b = row[src_i];
            const unsigned char g = row[src_i + 1U];
            const unsigned char r = row[src_i + 2U];
            const size_t dst_i = (static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)) * 3U;
            (*out_raw_rgb)[dst_i] = r;
            (*out_raw_rgb)[dst_i + 1U] = g;
            (*out_raw_rgb)[dst_i + 2U] = b;
        }
    }
    *out_w = width;
    *out_h = height;
    *out_bpc = 8;
    *out_components = 3;
    return true;
}

static bool BuildEmbeddedImageFromFile(const fs::path& file, const ConvertOptions* options, ImageWriteItem* out_item) {
    if (out_item == nullptr) {
        return false;
    }
    std::vector<unsigned char> raw;
    if (!ReadBinaryFile(file, &raw)) {
        return false;
    }
    int w = 0;
    int h = 0;
    int bpc = 8;
    int comps = 3;
    std::vector<unsigned char> payload;

    if (ParseJpegSize(raw, &w, &h)) {
        out_item->encoding = ImageWriteItem::Encoding::kJpegDct;
        out_item->data = std::move(raw);
        out_item->width_px = w;
        out_item->height_px = h;
        out_item->bits_per_component = 8;
        out_item->color_components = 3;
        return true;
    }
    if (ParsePngToPdfFlate(raw, &w, &h, &bpc, &comps, &payload)) {
        out_item->encoding = ImageWriteItem::Encoding::kPngFlate;
        out_item->data = std::move(payload);
        out_item->width_px = w;
        out_item->height_px = h;
        out_item->bits_per_component = bpc;
        out_item->color_components = comps;
        return true;
    }
    if (ParsePngRgbaToRawRgb(raw, &w, &h, &bpc, &comps, &payload)) {
        out_item->encoding = ImageWriteItem::Encoding::kRaw;
        out_item->data = std::move(payload);
        out_item->width_px = w;
        out_item->height_px = h;
        out_item->bits_per_component = bpc;
        out_item->color_components = comps;
        return true;
    }
    if (ParseBmpToRawRgb(raw, &w, &h, &bpc, &comps, &payload)) {
        out_item->encoding = ImageWriteItem::Encoding::kRaw;
        out_item->data = std::move(payload);
        out_item->width_px = w;
        out_item->height_px = h;
        out_item->bits_per_component = bpc;
        out_item->color_components = comps;
        return true;
    }
    if (options != nullptr && options->external_image_decode_fn != nullptr) {
        int out_w = 0;
        int out_h = 0;
        int out_bpc = 8;
        int out_components = 3;
        size_t out_len = 0;
        libofd_status_t status = options->external_image_decode_fn(
            file.string().c_str(), &out_w, &out_h, &out_bpc, &out_components, nullptr, &out_len,
            options->external_image_decode_user_data);
        if (status == LIBOFD_OK && out_len > 0U && out_w > 0 && out_h > 0 && (out_components == 1 || out_components == 3) &&
            out_bpc == 8) {
            std::vector<unsigned char> raw_pixels(out_len);
            status = options->external_image_decode_fn(
                file.string().c_str(), &out_w, &out_h, &out_bpc, &out_components, raw_pixels.data(), &out_len,
                options->external_image_decode_user_data);
            if (status == LIBOFD_OK) {
                raw_pixels.resize(out_len);
                out_item->encoding = ImageWriteItem::Encoding::kRaw;
                out_item->data = std::move(raw_pixels);
                out_item->width_px = out_w;
                out_item->height_px = out_h;
                out_item->bits_per_component = out_bpc;
                out_item->color_components = out_components;
                return true;
            }
        }
    }
    return false;
}

static std::string MapFontNameToBaseFont(const std::string& ofd_font_name) {
    if (HasNonAscii(ofd_font_name)) {
        return "STSong-Light";
    }
    const std::string lower = Lower(ofd_font_name);
    if (lower.find("song") != std::string::npos || lower.find("hei") != std::string::npos ||
        lower.find("kai") != std::string::npos || lower.find("fang") != std::string::npos ||
        lower.find("yahei") != std::string::npos || lower.find("simsun") != std::string::npos) {
        return "STSong-Light";
    }
    if (lower.find("times") != std::string::npos || lower.find("serif") != std::string::npos) {
        return "Times-Roman";
    }
    if (lower.find("courier") != std::string::npos || lower.find("mono") != std::string::npos) {
        return "Courier";
    }
    return "Helvetica";
}

static libofd_status_t WritePdfDocument(
    const std::vector<OfdPageLayout>& pages, const ResourceTable& resources, const ConvertOptions* options,
    const std::string& output_pdf_path) {
    if (pages.empty()) {
        return LIBOFD_ERR_INVALID_ARGUMENT;
    }
    std::vector<PdfObject> objects;

    // 1: catalog, 2: pages
    objects.push_back({1, "<< /Type /Catalog /Pages 2 0 R >>"});
    objects.push_back({2, ""}); // fill later

    // Build font set.
    std::unordered_set<std::string> base_fonts;
    base_fonts.insert("Helvetica");
    for (const auto& page : pages) {
        for (const auto& t : page.text_objects) {
            auto it = resources.fonts.find(t.ofd_font_id);
            if (it == resources.fonts.end()) {
                continue;
            }
            base_fonts.insert(MapFontNameToBaseFont(it->second));
        }
    }

    int next_id = 3;
    std::unordered_map<std::string, int> base_font_obj_id;
    std::unordered_map<std::string, std::string> alias_by_base_font;
    std::unordered_set<std::string> cjk_font_aliases;
    int font_alias_index = 1;
    for (const auto& base_font : base_fonts) {
        const std::string alias = "F" + std::to_string(font_alias_index++);
        alias_by_base_font[base_font] = alias;
        if (base_font == "STSong-Light") {
            const int type0_font_obj_id = next_id++;
            const int cid_font_obj_id = next_id++;
            base_font_obj_id[base_font] = type0_font_obj_id;
            cjk_font_aliases.insert(alias);
            std::ostringstream type0;
            type0 << "<< /Type /Font /Subtype /Type0 /BaseFont /STSong-Light /Encoding /UniGB-UCS2-H "
                  << "/DescendantFonts [" << cid_font_obj_id << " 0 R] >>";
            std::ostringstream cid;
            cid << "<< /Type /Font /Subtype /CIDFontType0 /BaseFont /STSong-Light "
                << "/CIDSystemInfo << /Registry (Adobe) /Ordering (GB1) /Supplement 4 >> /DW 1000 >>";
            objects.push_back({type0_font_obj_id, type0.str()});
            objects.push_back({cid_font_obj_id, cid.str()});
            continue;
        }
        const int font_obj_id = next_id++;
        base_font_obj_id[base_font] = font_obj_id;
        objects.push_back({font_obj_id, "<< /Type /Font /Subtype /Type1 /BaseFont /" + base_font + " >>"});
    }
    std::unordered_map<std::string, std::string> alias_by_ofd_font_id;
    for (const auto& kv : resources.fonts) {
        const std::string base_font = MapFontNameToBaseFont(kv.second);
        auto it_alias = alias_by_base_font.find(base_font);
        if (it_alias != alias_by_base_font.end()) {
            alias_by_ofd_font_id[kv.first] = it_alias->second;
        }
    }

    std::vector<int> page_ids;
    std::vector<int> content_ids;
    for (size_t i = 0; i < pages.size(); ++i) {
        const int page_id = next_id++;
        const int content_id = next_id++;
        page_ids.push_back(page_id);
        content_ids.push_back(content_id);
    }

    std::vector<PageWriteContext> page_ctxs(pages.size());
    for (size_t i = 0; i < pages.size(); ++i) {
        page_ctxs[i].page_id = page_ids[i];
        page_ctxs[i].content_id = content_ids[i];
        int image_idx = 1;
        for (const auto& image : pages[i].image_objects) {
            ImageWriteItem item;
            if (!BuildEmbeddedImageFromFile(image.file, options, &item)) {
                continue;
            }
            item.object_id = next_id++;
            item.image_name = "Im" + std::to_string(image_idx++);
            item.x = image.x;
            item.y = image.y;
            item.width = image.width;
            item.height = image.height;
            page_ctxs[i].images.push_back(std::move(item));
        }
    }

    for (size_t i = 0; i < pages.size(); ++i) {
        std::ostringstream page_obj;
        page_obj << "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 " << pages[i].width_pt << " " << pages[i].height_pt << "] /Resources << /Font << ";
        for (const auto& base_font : base_fonts) {
            const std::string alias = alias_by_base_font[base_font];
            page_obj << "/" << alias << " " << base_font_obj_id[base_font] << " 0 R ";
        }
        page_obj << ">> ";
        if (!page_ctxs[i].images.empty()) {
            page_obj << "/XObject << ";
            for (const auto& img : page_ctxs[i].images) {
                page_obj << "/" << img.image_name << " " << img.object_id << " 0 R ";
            }
            page_obj << ">> ";
        }
        page_obj << ">> /Contents " << content_ids[i] << " 0 R >>";
        objects.push_back({page_ids[i], page_obj.str()});

        std::ostringstream content_stream;
        // OFD content order is important: draw background/images first, then vector paths, then text on top.
        for (const auto& img : page_ctxs[i].images) {
            const double y_pdf = std::max(0.0, pages[i].height_pt - img.y - img.height);
            content_stream << "q\n";
            content_stream << img.width << " 0 0 " << img.height << " " << img.x << " " << y_pdf << " cm\n";
            content_stream << "/" << img.image_name << " Do\n";
            content_stream << "Q\n";
        }
        content_stream << BuildPathObjectsContentStream(pages[i]);
        content_stream << BuildTextObjectsContentStream(pages[i], alias_by_ofd_font_id, alias_by_base_font, cjk_font_aliases);
        if (pages[i].text_objects.empty()) {
            std::string default_alias = "F1";
            bool default_alias_is_cjk = false;
            auto cjk_it = alias_by_base_font.find("STSong-Light");
            if (cjk_it != alias_by_base_font.end()) {
                default_alias = cjk_it->second;
                default_alias_is_cjk = true;
            } else {
                auto latin_it = alias_by_base_font.find("Helvetica");
                if (latin_it != alias_by_base_font.end()) {
                    default_alias = latin_it->second;
                }
            }
            content_stream << BuildObjectPageContentStream(pages[i], default_alias, default_alias_is_cjk);
        }
        std::ostringstream content_obj;
        const std::string content_stream_str = content_stream.str();
        content_obj << "<< /Length " << content_stream_str.size() << " >>\nstream\n" << content_stream_str << "endstream";
        objects.push_back({content_ids[i], content_obj.str()});

        for (const auto& img : page_ctxs[i].images) {
            std::ostringstream img_obj;
            const std::string color_space = (img.color_components == 1) ? "/DeviceGray" : "/DeviceRGB";
            img_obj << "<< /Type /XObject /Subtype /Image /Width " << img.width_px << " /Height " << img.height_px
                    << " /ColorSpace " << color_space << " /BitsPerComponent " << img.bits_per_component;
            if (img.encoding == ImageWriteItem::Encoding::kJpegDct) {
                img_obj << " /Filter /DCTDecode";
            } else if (img.encoding == ImageWriteItem::Encoding::kPngFlate) {
                img_obj << " /Filter /FlateDecode /DecodeParms << /Predictor 15 /Colors " << img.color_components
                        << " /BitsPerComponent " << img.bits_per_component << " /Columns " << img.width_px << " >>";
            }
            img_obj << " /Length " << img.data.size() << " >>\nstream\n";
            std::string obj_prefix = img_obj.str();
            std::string obj_suffix = "\nendstream";
            std::string binary(obj_prefix.begin(), obj_prefix.end());
            binary.append(reinterpret_cast<const char*>(img.data.data()), img.data.size());
            binary.append(obj_suffix);
            objects.push_back({img.object_id, binary});
        }
    }

    std::ostringstream kids;
    for (size_t i = 0; i < page_ids.size(); ++i) {
        kids << page_ids[i] << " 0 R ";
    }
    std::ostringstream pages_obj;
    pages_obj << "<< /Type /Pages /Kids [" << kids.str() << "] /Count " << page_ids.size() << " >>";
    objects[1].data = pages_obj.str();

    std::sort(objects.begin(), objects.end(), [](const PdfObject& a, const PdfObject& b) { return a.id < b.id; });
    const int max_id = objects.back().id;

    std::error_code ec;
    const fs::path out_path(output_pdf_path);
    if (!out_path.parent_path().empty()) {
        fs::create_directories(out_path.parent_path(), ec);
    }
    std::ofstream out(output_pdf_path, std::ios::binary);
    if (!out.is_open()) {
        return LIBOFD_ERR_IO;
    }
    out << "%PDF-1.4\n";

    std::vector<std::streamoff> offsets(static_cast<size_t>(max_id + 1), 0);
    for (const auto& obj : objects) {
        offsets[static_cast<size_t>(obj.id)] = out.tellp();
        out << obj.id << " 0 obj\n" << obj.data << "\nendobj\n";
    }

    const std::streamoff xref_offset = out.tellp();
    out << "xref\n";
    out << "0 " << (max_id + 1) << "\n";
    out << "0000000000 65535 f \n";
    for (int id = 1; id <= max_id; ++id) {
        const std::streamoff off = offsets[static_cast<size_t>(id)];
        out << std::setw(10) << std::setfill('0') << off << " 00000 n \n";
    }
    out << "trailer\n";
    out << "<< /Size " << (max_id + 1) << " /Root 1 0 R >>\n";
    out << "startxref\n" << xref_offset << "\n%%EOF\n";
    return LIBOFD_OK;
}

static libofd_status_t ReadTextFile(const std::string& path, std::string* out_content) {
    if (out_content == nullptr) {
        return LIBOFD_ERR_INVALID_ARGUMENT;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        return LIBOFD_ERR_NOT_FOUND;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    *out_content = ss.str();
    return LIBOFD_OK;
}

static bool ParseDouble(const std::string& token, double* out_value) {
    if (out_value == nullptr) {
        return false;
    }
    try {
        *out_value = std::stod(token);
        return true;
    } catch (...) {
        return false;
    }
}

static std::string ExtractAttributeValue(const std::string& attrs, const std::string& name) {
    const std::regex re(name + "=\"([^\"]*)\"");
    std::smatch m;
    if (!std::regex_search(attrs, m, re)) {
        return "";
    }
    return m[1].str();
}

static std::vector<std::string> ExtractTagTexts(const std::string& body, const std::string& tag_name) {
    std::vector<std::string> values;
    const std::regex re("<(?:ofd:)?" + tag_name + R"(\b[^>]*>\s*([\s\S]*?)\s*</(?:ofd:)?)" + tag_name + ">");
    auto begin = std::sregex_iterator(body.begin(), body.end(), re);
    auto end = std::sregex_iterator();
    for (auto it = begin; it != end; ++it) {
        values.push_back((*it)[1].str());
    }
    return values;
}

static void ParsePhysicalBoxToPt(const std::string& document_xml, double* out_w, double* out_h) {
    if (out_w == nullptr || out_h == nullptr) {
        return;
    }
    const std::regex re(R"(<(?:ofd:)?PhysicalBox>\s*([^<]+)\s*</(?:ofd:)?PhysicalBox>)");
    std::smatch m;
    if (!std::regex_search(document_xml, m, re)) {
        return;
    }
    std::stringstream ss(m[1].str());
    std::string v0;
    std::string v1;
    std::string v2;
    std::string v3;
    if (!(ss >> v0 >> v1 >> v2 >> v3)) {
        return;
    }
    double x = 0.0;
    double y = 0.0;
    double w = 0.0;
    double h = 0.0;
    if (!ParseDouble(v0, &x) || !ParseDouble(v1, &y) || !ParseDouble(v2, &w) || !ParseDouble(v3, &h)) {
        return;
    }
    (void)x;
    (void)y;
    *out_w = w * kMmToPt;
    *out_h = h * kMmToPt;
}

static std::vector<std::string> ParsePageBaseLocs(const std::string& document_xml) {
    std::vector<std::string> base_locs;
    const std::regex re(R"(<(?:ofd:)?Page\b([^>]*)/?>)");
    auto begin = std::sregex_iterator(document_xml.begin(), document_xml.end(), re);
    auto end = std::sregex_iterator();
    for (auto it = begin; it != end; ++it) {
        const std::string attrs = (*it)[1].str();
        const std::string base_loc = ExtractAttributeValue(attrs, "BaseLoc");
        if (!base_loc.empty()) {
            base_locs.push_back(base_loc);
        }
    }
    return base_locs;
}

static std::unordered_map<std::string, size_t> ParsePageIdToIndex(const std::string& document_xml) {
    std::unordered_map<std::string, size_t> out;
    const std::regex re(R"(<(?:ofd:)?Page\b([^>]*)/?>)");
    auto begin = std::sregex_iterator(document_xml.begin(), document_xml.end(), re);
    auto end = std::sregex_iterator();
    size_t idx = 0;
    for (auto it = begin; it != end; ++it) {
        const std::string attrs = (*it)[1].str();
        const std::string id = ExtractAttributeValue(attrs, "ID");
        if (!id.empty()) {
            out[id] = idx;
        }
        ++idx;
    }
    return out;
}

static void ParseTextObjects(const std::string& page_xml, std::vector<OfdTextObject>* out_objects) {
    if (out_objects == nullptr) {
        return;
    }
    out_objects->clear();
    const std::regex text_obj_re(R"(<(?:ofd:)?TextObject\b([^>]*)>([\s\S]*?)</(?:ofd:)?TextObject>)");
    auto begin = std::sregex_iterator(page_xml.begin(), page_xml.end(), text_obj_re);
    auto end = std::sregex_iterator();
    for (auto it = begin; it != end; ++it) {
        const std::string attrs = (*it)[1].str();
        const std::string body = (*it)[2].str();
        OfdTextObject obj;
        std::string size_str = ExtractAttributeValue(attrs, "Size");
        double size = 11.0;
        if (!size_str.empty()) {
            ParseDouble(size_str, &size);
        }
        obj.font_size = std::max(8.0, size * kMmToPt);

        const std::string boundary = ExtractAttributeValue(attrs, "Boundary");
        if (!boundary.empty()) {
            std::stringstream bs(boundary);
            double x = 0.0;
            double y = 0.0;
            double w = 0.0;
            double h = 0.0;
            if (bs >> x >> y >> w >> h) {
                obj.x = x * kMmToPt;
                obj.y = y * kMmToPt;
            }
        }
        obj.ofd_font_id = ExtractAttributeValue(attrs, "Font");

        const auto text_codes = ExtractTagTexts(body, "TextCode");
        if (!text_codes.empty()) {
            std::ostringstream merged;
            for (size_t i = 0; i < text_codes.size(); ++i) {
                if (i > 0U) {
                    merged << "\n";
                }
                merged << text_codes[i];
            }
            obj.text = merged.str();
        }
        const std::regex text_code_re(R"(<(?:ofd:)?TextCode\b([^>]*)>([\s\S]*?)</(?:ofd:)?TextCode>)");
        std::smatch tc_match;
        if (std::regex_search(body, tc_match, text_code_re)) {
            const std::string tc_attrs = tc_match[1].str();
            const std::string x_str = ExtractAttributeValue(tc_attrs, "X");
            const std::string y_str = ExtractAttributeValue(tc_attrs, "Y");
            double x_off_mm = 0.0;
            double y_off_mm = 0.0;
            if (!x_str.empty()) {
                ParseDouble(x_str, &x_off_mm);
            }
            if (!y_str.empty()) {
                ParseDouble(y_str, &y_off_mm);
            }
            obj.x += x_off_mm * kMmToPt;
            obj.y += y_off_mm * kMmToPt;

            const std::string delta_x = ExtractAttributeValue(tc_attrs, "DeltaX");
            if (!delta_x.empty() && !obj.text.empty()) {
                std::stringstream ds(delta_x);
                std::vector<double> delta_values_mm;
                double v = 0.0;
                while (ds >> v) {
                    delta_values_mm.push_back(v);
                }
                const auto glyphs = SplitUtf8Codepoints(obj.text);
                if (!glyphs.empty()) {
                    obj.glyph_offsets_x.clear();
                    obj.glyph_offsets_x.reserve(glyphs.size());
                    double cur_mm = 0.0;
                    obj.glyph_offsets_x.push_back(0.0);
                    for (size_t i = 1; i < glyphs.size(); ++i) {
                        if (i - 1U < delta_values_mm.size()) {
                            cur_mm += delta_values_mm[i - 1U];
                        } else if (!delta_values_mm.empty()) {
                            cur_mm += delta_values_mm.back();
                        } else {
                            cur_mm += (obj.font_size / kMmToPt) * 0.5;
                        }
                        obj.glyph_offsets_x.push_back(cur_mm * kMmToPt);
                    }
                }
            }
        }
        if (!obj.text.empty()) {
            out_objects->push_back(obj);
        }
    }
}

static void ParseImageObjects(
    const std::string& page_xml, const ResourceTable& resources, std::vector<OfdImageObject>* out_images) {
    if (out_images == nullptr) {
        return;
    }
    out_images->clear();
    const std::regex image_obj_re(R"(<(?:ofd:)?ImageObject\b([^>]*)/?>)");
    auto begin = std::sregex_iterator(page_xml.begin(), page_xml.end(), image_obj_re);
    auto end = std::sregex_iterator();
    for (auto it = begin; it != end; ++it) {
        const std::string attrs = (*it)[1].str();
        const std::string res_id = ExtractAttributeValue(attrs, "ResourceID");
        auto it_res = resources.images.find(res_id);
        if (it_res == resources.images.end()) {
            continue;
        }
        const std::string boundary = ExtractAttributeValue(attrs, "Boundary");
        if (boundary.empty()) {
            continue;
        }
        std::stringstream bs(boundary);
        double x = 0.0;
        double y = 0.0;
        double w = 0.0;
        double h = 0.0;
        if (!(bs >> x >> y >> w >> h)) {
            continue;
        }
        OfdImageObject image;
        image.file = it_res->second;
        image.x = x * kMmToPt;
        image.y = y * kMmToPt;
        image.width = std::max(1.0, w * kMmToPt);
        image.height = std::max(1.0, h * kMmToPt);
        out_images->push_back(std::move(image));
    }
}

static void ParsePathObjects(const std::string& page_xml, std::vector<OfdPathObject>* out_paths) {
    if (out_paths == nullptr) {
        return;
    }
    out_paths->clear();
    const std::regex path_obj_re(R"(<(?:ofd:)?PathObject\b([^>]*)>([\s\S]*?)</(?:ofd:)?PathObject>|<(?:ofd:)?PathObject\b([^>]*)/>)");
    auto begin = std::sregex_iterator(page_xml.begin(), page_xml.end(), path_obj_re);
    auto end = std::sregex_iterator();
    for (auto it = begin; it != end; ++it) {
        const std::string attrs = (*it)[1].matched ? (*it)[1].str() : (*it)[3].str();
        const std::string body = (*it)[2].matched ? (*it)[2].str() : "";
        OfdPathObject path;

        const std::string boundary = ExtractAttributeValue(attrs, "Boundary");
        if (!boundary.empty()) {
            std::stringstream bs(boundary);
            double x = 0.0;
            double y = 0.0;
            double w = 0.0;
            double h = 0.0;
            if (bs >> x >> y >> w >> h) {
                path.boundary_x = x * kMmToPt;
                path.boundary_y = y * kMmToPt;
            }
        }
        const std::string lw = ExtractAttributeValue(attrs, "LineWidth");
        double line_width_mm = 0.35;
        if (!lw.empty()) {
            ParseDouble(lw, &line_width_mm);
        }
        path.line_width = std::max(0.2, line_width_mm * kMmToPt);
        path.stroke = ParseBoolAttr(ExtractAttributeValue(attrs, "Stroke"), true);
        path.fill = ParseBoolAttr(ExtractAttributeValue(attrs, "Fill"), false);

        const std::regex stroke_color_re(R"(<(?:ofd:)?StrokeColor\b([^>]*)/?>)");
        std::smatch stroke_color_match;
        if (std::regex_search(body, stroke_color_match, stroke_color_re)) {
            const std::string cattrs = stroke_color_match[1].str();
            const std::string value = ExtractAttributeValue(cattrs, "Value");
            if (ParseRgbFromOfdValue(value, &path.stroke_r, &path.stroke_g, &path.stroke_b)) {
                path.has_stroke_color = true;
            }
        }
        const std::regex fill_color_re(R"(<(?:ofd:)?FillColor\b([^>]*)/?>)");
        std::smatch fill_color_match;
        if (std::regex_search(body, fill_color_match, fill_color_re)) {
            const std::string cattrs = fill_color_match[1].str();
            const std::string value = ExtractAttributeValue(cattrs, "Value");
            if (ParseRgbFromOfdValue(value, &path.fill_r, &path.fill_g, &path.fill_b)) {
                path.has_fill_color = true;
            }
        }

        path.abbreviated_data = ExtractAttributeValue(attrs, "AbbreviatedData");
        if (path.abbreviated_data.empty()) {
            const auto data_tags = ExtractTagTexts(body, "AbbreviatedData");
            if (!data_tags.empty()) {
                path.abbreviated_data = data_tags[0];
            }
        }
        if (!path.abbreviated_data.empty()) {
            out_paths->push_back(std::move(path));
        }
    }
}

static void ParseFontResources(const std::string& xml, ResourceTable* out_resources) {
    if (out_resources == nullptr) {
        return;
    }
    const std::regex font_re(R"(<(?:ofd:)?Font\b([^>]*)/?>)");
    auto begin = std::sregex_iterator(xml.begin(), xml.end(), font_re);
    auto end = std::sregex_iterator();
    for (auto it = begin; it != end; ++it) {
        const std::string attrs = (*it)[1].str();
        const std::string id = ExtractAttributeValue(attrs, "ID");
        const std::string font_name = ExtractAttributeValue(attrs, "FontName");
        if (!id.empty() && !font_name.empty()) {
            out_resources->fonts[id] = font_name;
        }
    }
}

static void ParseImageResources(const std::string& xml, const fs::path& base_dir, ResourceTable* out_resources) {
    if (out_resources == nullptr) {
        return;
    }
    fs::path media_base = base_dir;
    {
        const std::regex res_re(R"(<(?:ofd:)?Res\b([^>]*)>)");
        std::smatch m;
        if (std::regex_search(xml, m, res_re)) {
            const std::string res_attrs = m[1].str();
            const std::string base_loc = ExtractAttributeValue(res_attrs, "BaseLoc");
            if (!base_loc.empty()) {
                media_base /= NormalizeOfdPath(base_loc);
            }
        }
    }

    const std::regex mm_re(R"(<(?:ofd:)?MultiMedia\b([^>]*)>([\s\S]*?)</(?:ofd:)?MultiMedia>)");
    auto begin = std::sregex_iterator(xml.begin(), xml.end(), mm_re);
    auto end = std::sregex_iterator();
    for (auto it = begin; it != end; ++it) {
        const std::string attrs = (*it)[1].str();
        const std::string body = (*it)[2].str();
        const std::string id = ExtractAttributeValue(attrs, "ID");
        const std::string type = Lower(ExtractAttributeValue(attrs, "Type"));
        if (id.empty() || type != "image") {
            continue;
        }
        auto files = ExtractTagTexts(body, "MediaFile");
        if (files.empty()) {
            continue;
        }
        out_resources->images[id] = media_base / NormalizeOfdPath(files[0]);
    }
}

static void LoadDocumentResources(
    const fs::path& package_root, const fs::path& document_dir, const std::string& document_xml,
    ResourceTable* out_resources) {
    if (out_resources == nullptr) {
        return;
    }
    const std::regex public_res_re(R"(<(?:ofd:)?PublicRes>\s*([^<]+)\s*</(?:ofd:)?PublicRes>)");
    const std::regex doc_res_re(R"(<(?:ofd:)?DocumentRes>\s*([^<]+)\s*</(?:ofd:)?DocumentRes>)");
    std::array<std::regex, 2> rules = {public_res_re, doc_res_re};
    for (const auto& rule : rules) {
        std::smatch m;
        if (!std::regex_search(document_xml, m, rule)) {
            continue;
        }
        const fs::path res_path = ResolveOfdPath(package_root, document_dir, m[1].str());
        std::string res_xml;
        if (ReadTextFile(res_path.string(), &res_xml) != LIBOFD_OK) {
            continue;
        }
        ParseFontResources(res_xml, out_resources);
        ParseImageResources(res_xml, res_path.parent_path(), out_resources);
    }
}

static fs::path SignatureSidecarDirForPdf(const fs::path& pdf_path) {
    return fs::path(pdf_path.string() + ".libofd_signs");
}

static void RemoveDirIfExists(const fs::path& path) {
    std::error_code ec;
    fs::remove_all(path, ec);
}

static void CopyDirectoryRecursive(const fs::path& src, const fs::path& dst) {
    std::error_code ec;
    fs::create_directories(dst, ec);
    for (const auto& it : fs::recursive_directory_iterator(src, ec)) {
        if (ec) {
            return;
        }
        const fs::path rel = fs::relative(it.path(), src, ec);
        if (ec) {
            return;
        }
        const fs::path target = dst / rel;
        if (it.is_directory()) {
            fs::create_directories(target, ec);
            if (ec) {
                return;
            }
        } else if (it.is_regular_file()) {
            fs::create_directories(target.parent_path(), ec);
            if (ec) {
                return;
            }
            fs::copy_file(it.path(), target, fs::copy_options::overwrite_existing, ec);
            if (ec) {
                return;
            }
        }
    }
}

static void PreserveSignsToPdfSidecar(const fs::path& document_dir, const fs::path& output_pdf_path) {
    const fs::path signs_dir = document_dir / "Signs";
    if (!fs::exists(signs_dir) || !fs::is_directory(signs_dir)) {
        return;
    }
    const fs::path sidecar_dir = SignatureSidecarDirForPdf(output_pdf_path);
    RemoveDirIfExists(sidecar_dir);
    CopyDirectoryRecursive(signs_dir, sidecar_dir / "Signs");
    std::ofstream note(sidecar_dir / "README.txt", std::ios::binary);
    if (note.is_open()) {
        note << "This sidecar preserves OFD signature artifacts during OFD<->PDF conversion.\n";
        note << "Cryptographic validity is not guaranteed after document content conversion.\n";
    }
}

static void EnsureOfdSignaturesEntry(const fs::path& output_ofd_root) {
    const fs::path ofd_xml_path = output_ofd_root / "OFD.xml";
    std::string ofd_xml;
    if (ReadTextFile(ofd_xml_path.string(), &ofd_xml) != LIBOFD_OK) {
        return;
    }
    if (ofd_xml.find("<ofd:Signatures>") != std::string::npos || ofd_xml.find("<Signatures>") != std::string::npos) {
        return;
    }
    const std::string marker = "</ofd:DocBody>";
    const size_t pos = ofd_xml.find(marker);
    if (pos == std::string::npos) {
        return;
    }
    const std::string insert = "  <ofd:Signatures>/Doc_0/Signs/Signatures.xml</ofd:Signatures>\n";
    ofd_xml.insert(pos, insert);
    std::ofstream out(ofd_xml_path, std::ios::binary);
    if (out.is_open()) {
        out << ofd_xml;
    }
}

static void RestoreSignsFromPdfSidecar(const fs::path& input_pdf_path, const fs::path& output_ofd_root) {
    const fs::path sidecar_dir = SignatureSidecarDirForPdf(input_pdf_path);
    const fs::path sidecar_signs = sidecar_dir / "Signs";
    if (!fs::exists(sidecar_signs) || !fs::is_directory(sidecar_signs)) {
        return;
    }
    CopyDirectoryRecursive(sidecar_signs, output_ofd_root / "Doc_0" / "Signs");
    EnsureOfdSignaturesEntry(output_ofd_root);
}

static bool ExtractPngFromBlob(const std::vector<unsigned char>& blob, std::vector<unsigned char>* out_png) {
    if (out_png == nullptr || blob.size() < 8U) {
        return false;
    }
    static const unsigned char kPngSig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    size_t start = std::string::npos;
    for (size_t i = 0; i + 8U <= blob.size(); ++i) {
        if (std::memcmp(&blob[i], kPngSig, 8U) == 0) {
            start = i;
            break;
        }
    }
    if (start == std::string::npos) {
        return false;
    }
    size_t pos = start + 8U;
    while (pos + 12U <= blob.size()) {
        const uint32_t len = ReadU32BE(&blob[pos]);
        if (pos + 12U + len > blob.size()) {
            return false;
        }
        const char t0 = static_cast<char>(blob[pos + 4U]);
        const char t1 = static_cast<char>(blob[pos + 5U]);
        const char t2 = static_cast<char>(blob[pos + 6U]);
        const char t3 = static_cast<char>(blob[pos + 7U]);
        pos += 12U + len;
        if (t0 == 'I' && t1 == 'E' && t2 == 'N' && t3 == 'D') {
            out_png->assign(blob.begin() + static_cast<std::ptrdiff_t>(start), blob.begin() + static_cast<std::ptrdiff_t>(pos));
            return true;
        }
    }
    return false;
}

static bool ExtractJpegFromBlob(const std::vector<unsigned char>& blob, std::vector<unsigned char>* out_jpeg) {
    if (out_jpeg == nullptr || blob.size() < 4U) {
        return false;
    }
    size_t start = std::string::npos;
    for (size_t i = 0; i + 2U <= blob.size(); ++i) {
        if (blob[i] == 0xFFU && blob[i + 1U] == 0xD8U) {
            start = i;
            break;
        }
    }
    if (start == std::string::npos) {
        return false;
    }
    for (size_t i = start + 2U; i + 1U < blob.size(); ++i) {
        if (blob[i] == 0xFFU && blob[i + 1U] == 0xD9U) {
            out_jpeg->assign(blob.begin() + static_cast<std::ptrdiff_t>(start), blob.begin() + static_cast<std::ptrdiff_t>(i + 2U));
            return true;
        }
    }
    return false;
}

static fs::path WriteTempBinaryFile(const std::vector<unsigned char>& data, const std::string& ext) {
    const auto stamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    const fs::path out = fs::temp_directory_path() / ("libofd_seal_" + std::to_string(stamp) + ext);
    std::ofstream os(out, std::ios::binary);
    if (!os.is_open()) {
        return {};
    }
    os.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    return out;
}

static fs::path ResolveSealRenderableImage(const fs::path& seal_path) {
    const std::string ext = Lower(seal_path.extension().string());
    if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".bmp") {
        return seal_path;
    }
    std::vector<unsigned char> blob;
    if (!ReadBinaryFile(seal_path, &blob)) {
        return {};
    }
    std::vector<unsigned char> png;
    if (ExtractPngFromBlob(blob, &png)) {
        return WriteTempBinaryFile(png, ".png");
    }
    std::vector<unsigned char> jpg;
    if (ExtractJpegFromBlob(blob, &jpg)) {
        return WriteTempBinaryFile(jpg, ".jpg");
    }
    return {};
}

static void AppendSignatureStampImages(
    const fs::path& package_root, const fs::path& document_dir, const std::string& ofd_xml, const std::string& document_xml,
    std::vector<OfdPageLayout>* pages) {
    if (pages == nullptr || pages->empty()) {
        return;
    }
    const auto page_id_to_index = ParsePageIdToIndex(document_xml);
    std::smatch signs_match;
    const std::regex signs_path_re(R"(<(?:ofd:)?Signatures>\s*([^<]+)\s*</(?:ofd:)?Signatures>)");
    if (!std::regex_search(ofd_xml, signs_match, signs_path_re)) {
        return;
    }
    const fs::path signatures_xml_path = ResolveOfdPath(package_root, document_dir, signs_match[1].str());
    std::string signatures_xml;
    if (ReadTextFile(signatures_xml_path.string(), &signatures_xml) != LIBOFD_OK) {
        return;
    }

    const std::regex sign_entry_re(R"(<(?:ofd:)?Signature\b([^>]*)/?>)");
    auto begin = std::sregex_iterator(signatures_xml.begin(), signatures_xml.end(), sign_entry_re);
    auto end = std::sregex_iterator();
    for (auto it = begin; it != end; ++it) {
        const std::string attrs = (*it)[1].str();
        const std::string base_loc = ExtractAttributeValue(attrs, "BaseLoc");
        if (base_loc.empty()) {
            continue;
        }
        const fs::path signature_xml_path = ResolveOfdPath(package_root, document_dir, base_loc);
        std::string signature_xml;
        if (ReadTextFile(signature_xml_path.string(), &signature_xml) != LIBOFD_OK) {
            continue;
        }

        std::smatch seal_match;
        const std::regex seal_re(
            R"(<(?:ofd:)?Seal>\s*<(?:ofd:)?BaseLoc>\s*([^<]+)\s*</(?:ofd:)?BaseLoc>\s*</(?:ofd:)?Seal>)");
        if (!std::regex_search(signature_xml, seal_match, seal_re)) {
            continue;
        }
        const fs::path seal_path = ResolveOfdPath(package_root, document_dir, seal_match[1].str());
        const fs::path image_path = ResolveSealRenderableImage(seal_path);
        if (image_path.empty() || !fs::exists(image_path)) {
            continue;
        }

        const std::regex annot_re(R"(<(?:ofd:)?StampAnnot\b([^>]*)/?>)");
        std::string provider_name;
        std::string sign_method;
        std::string sign_time;
        {
            std::smatch m;
            const std::regex p_re(R"(<(?:ofd:)?Provider\b[^>]*ProviderName=\"([^\"]+)\"[^>]*/?>)");
            if (std::regex_search(signature_xml, m, p_re)) {
                provider_name = m[1].str();
            }
            const std::regex method_re(R"(<(?:ofd:)?SignatureMethod>\s*([^<]+)\s*</(?:ofd:)?SignatureMethod>)");
            if (std::regex_search(signature_xml, m, method_re)) {
                sign_method = m[1].str();
            }
            const std::regex time_re(R"(<(?:ofd:)?SignatureDateTime>\s*([^<]+)\s*</(?:ofd:)?SignatureDateTime>)");
            if (std::regex_search(signature_xml, m, time_re)) {
                sign_time = m[1].str();
            }
        }
        auto a_begin = std::sregex_iterator(signature_xml.begin(), signature_xml.end(), annot_re);
        auto a_end = std::sregex_iterator();
        for (auto ait = a_begin; ait != a_end; ++ait) {
            const std::string a_attrs = (*ait)[1].str();
            const std::string page_ref = ExtractAttributeValue(a_attrs, "PageRef");
            const std::string boundary = ExtractAttributeValue(a_attrs, "Boundary");
            if (boundary.empty()) {
                continue;
            }
            size_t page_idx = 0;
            auto map_it = page_id_to_index.find(page_ref);
            if (map_it != page_id_to_index.end()) {
                page_idx = map_it->second;
            }
            if (page_idx >= pages->size()) {
                continue;
            }
            std::stringstream bs(boundary);
            double x = 0.0;
            double y = 0.0;
            double w = 0.0;
            double h = 0.0;
            if (!(bs >> x >> y >> w >> h)) {
                continue;
            }
            OfdImageObject stamp;
            stamp.file = image_path;
            stamp.x = x * kMmToPt;
            stamp.y = y * kMmToPt;
            stamp.width = w * kMmToPt;
            stamp.height = h * kMmToPt;
            (*pages)[page_idx].image_objects.push_back(std::move(stamp));

            // Add visible signature attributes for easier manual inspection in converted PDF.
            OfdTextObject note;
            note.x = x * kMmToPt;
            note.y = (y + h + 1.5) * kMmToPt;
            note.font_size = 8.0;
            std::ostringstream ns;
            ns << "[OFD-Sign] " << (provider_name.empty() ? "unknown-provider" : provider_name);
            if (!sign_time.empty()) {
                ns << " " << sign_time;
            }
            if (!sign_method.empty()) {
                ns << " " << sign_method;
            }
            note.text = ns.str();
            note.ofd_font_id.clear();
            (*pages)[page_idx].text_objects.push_back(std::move(note));
        }
    }
}

static std::vector<std::string> ExtractStringsFromPdfStream(const std::string& stream) {
    std::vector<std::string> tokens;
    bool in_text = false;
    int nested = 0;
    std::string current;

    for (size_t i = 0; i < stream.size(); ++i) {
        const char c = stream[i];
        if (!in_text) {
            if (c == '(') {
                in_text = true;
                nested = 1;
                current.clear();
            }
            continue;
        }
        if (c == '\\' && i + 1U < stream.size()) {
            current.push_back(c);
            current.push_back(stream[++i]);
            continue;
        }
        if (c == '(') {
            ++nested;
            current.push_back(c);
            continue;
        }
        if (c == ')') {
            --nested;
            if (nested == 0) {
                tokens.push_back(UnescapePdfString(current));
                in_text = false;
            } else {
                current.push_back(c);
            }
            continue;
        }
        current.push_back(c);
    }
    return tokens;
}

static std::string KeepHexCharsOnly(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        if (std::isxdigit(c)) {
            out.push_back(static_cast<char>(c));
        }
    }
    if (out.size() % 2U != 0U) {
        out.push_back('0');
    }
    return out;
}

static int HexNibble(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

static std::vector<unsigned char> HexToBytes(const std::string& hex) {
    std::vector<unsigned char> out;
    if (hex.empty()) {
        return out;
    }
    out.reserve(hex.size() / 2U);
    for (size_t i = 0; i + 1U < hex.size(); i += 2U) {
        const int hi = HexNibble(hex[i]);
        const int lo = HexNibble(hex[i + 1U]);
        if (hi < 0 || lo < 0) {
            continue;
        }
        out.push_back(static_cast<unsigned char>((hi << 4) | lo));
    }
    return out;
}

static void AppendUtf8FromCodepoint(uint32_t cp, std::string* out) {
    if (out == nullptr) {
        return;
    }
    if (cp <= 0x7F) {
        out->push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FF) {
        out->push_back(static_cast<char>(0xC0U | ((cp >> 6U) & 0x1FU)));
        out->push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
    } else if (cp <= 0xFFFF) {
        out->push_back(static_cast<char>(0xE0U | ((cp >> 12U) & 0x0FU)));
        out->push_back(static_cast<char>(0x80U | ((cp >> 6U) & 0x3FU)));
        out->push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
    } else if (cp <= 0x10FFFF) {
        out->push_back(static_cast<char>(0xF0U | ((cp >> 18U) & 0x07U)));
        out->push_back(static_cast<char>(0x80U | ((cp >> 12U) & 0x3FU)));
        out->push_back(static_cast<char>(0x80U | ((cp >> 6U) & 0x3FU)));
        out->push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
    }
}

static std::string Utf16BeHexToUtf8(const std::string& hex) {
    const auto bytes = HexToBytes(KeepHexCharsOnly(hex));
    if (bytes.empty()) {
        return "";
    }
    std::string out;
    for (size_t i = 0; i + 1U < bytes.size();) {
        uint16_t u = static_cast<uint16_t>((bytes[i] << 8U) | bytes[i + 1U]);
        i += 2U;
        if (u >= 0xD800 && u <= 0xDBFF && i + 1U < bytes.size()) {
            const uint16_t low = static_cast<uint16_t>((bytes[i] << 8U) | bytes[i + 1U]);
            if (low >= 0xDC00 && low <= 0xDFFF) {
                const uint32_t cp = 0x10000U + (((u - 0xD800U) << 10U) | (low - 0xDC00U));
                AppendUtf8FromCodepoint(cp, &out);
                i += 2U;
                continue;
            }
        }
        AppendUtf8FromCodepoint(u, &out);
    }
    return out;
}

using PdfFontCMap = std::unordered_map<std::string, std::string>;
using PdfPageFontMaps = std::unordered_map<std::string, PdfFontCMap>;

static PdfFontCMap ParseToUnicodeMap(const std::string& cmap_text) {
    PdfFontCMap cmap;
    const std::regex bfchar_re(R"(<([0-9A-Fa-f]+)>\s*<([0-9A-Fa-f]+)>)");
    auto begin = std::sregex_iterator(cmap_text.begin(), cmap_text.end(), bfchar_re);
    auto end = std::sregex_iterator();
    for (auto it = begin; it != end; ++it) {
        const std::string src = KeepHexCharsOnly((*it)[1].str());
        const std::string dst = KeepHexCharsOnly((*it)[2].str());
        if (src.empty() || dst.empty()) {
            continue;
        }
        cmap[src] = Utf16BeHexToUtf8(dst);
    }
    return cmap;
}

static std::string DecodePdfHexToken(
    const std::string& hex_token, const PdfFontCMap* active_cmap, const PdfFontCMap* fallback_cmap) {
    std::string payload = hex_token;
    if (!payload.empty() && payload.front() == '<' && payload.back() == '>') {
        payload = payload.substr(1, payload.size() - 2U);
    }
    payload = KeepHexCharsOnly(payload);
    if (payload.empty()) {
        return "";
    }

    const PdfFontCMap* cmap = nullptr;
    if (active_cmap != nullptr && !active_cmap->empty()) {
        cmap = active_cmap;
    } else if (fallback_cmap != nullptr && !fallback_cmap->empty()) {
        cmap = fallback_cmap;
    }
    if (cmap == nullptr) {
        if (payload.size() >= 4U && payload.size() % 4U == 0U) {
            const std::string utf8 = Utf16BeHexToUtf8(payload);
            if (!utf8.empty()) {
                return utf8;
            }
        }
        std::string out;
        for (unsigned char c : HexToBytes(payload)) {
            if (c >= 0x20 || c == '\n' || c == '\r' || c == '\t') {
                out.push_back(static_cast<char>(c));
            }
        }
        return out;
    }

    size_t step = 4U;
    for (const auto& kv : *cmap) {
        if (kv.first.size() == 2U) {
            step = 2U;
            break;
        }
    }

    std::string out;
    for (size_t i = 0; i + step <= payload.size(); i += step) {
        const std::string code = payload.substr(i, step);
        auto it = cmap->find(code);
        if (it != cmap->end()) {
            out += it->second;
            continue;
        }
        if (step == 4U) {
            const std::string hi = code.substr(0, 2);
            const std::string lo = code.substr(2, 2);
            auto it_hi = cmap->find(hi);
            auto it_lo = cmap->find(lo);
            if (it_hi != cmap->end()) {
                out += it_hi->second;
            }
            if (it_lo != cmap->end()) {
                out += it_lo->second;
            }
        }
    }
    return out;
}

static PdfPageFontMaps ParsePageFontMaps(
    const std::string& page_obj_body, const std::unordered_map<int, std::string>& object_map,
    const std::function<std::string(const std::string&)>& extract_stream) {
    PdfPageFontMaps maps;
    std::smatch font_block_match;
    const std::regex font_block_re(R"(/Font\s*<<([\s\S]*?)>>)");
    if (!std::regex_search(page_obj_body, font_block_match, font_block_re)) {
        return maps;
    }
    const std::string font_block = font_block_match[1].str();
    const std::regex font_ref_re(R"(/([A-Za-z0-9_]+)\s+(\d+)\s+\d+\s+R)");
    auto begin = std::sregex_iterator(font_block.begin(), font_block.end(), font_ref_re);
    auto end = std::sregex_iterator();
    for (auto it = begin; it != end; ++it) {
        const std::string font_name = (*it)[1].str();
        const int font_obj_id = std::atoi((*it)[2].str().c_str());
        auto font_it = object_map.find(font_obj_id);
        if (font_it == object_map.end()) {
            continue;
        }
        const std::regex touni_re(R"(/ToUnicode\s+(\d+)\s+\d+\s+R)");
        std::smatch m;
        if (!std::regex_search(font_it->second, m, touni_re)) {
            continue;
        }
        const int tounicode_id = std::atoi(m[1].str().c_str());
        auto tu_it = object_map.find(tounicode_id);
        if (tu_it == object_map.end()) {
            continue;
        }
        const std::string cmap_stream = extract_stream(tu_it->second);
        if (cmap_stream.empty()) {
            continue;
        }
        maps[font_name] = ParseToUnicodeMap(cmap_stream);
    }
    return maps;
}

static std::vector<std::string> ExtractPageTextsFromPdf(const std::string& pdf_content, bool merge_lines = true) {
    struct PdfObj {
        int id = 0;
        std::string body;
    };

    auto extract_stream = [](const std::string& obj_body) -> std::string {
        const size_t stream_pos = obj_body.find("stream");
        if (stream_pos == std::string::npos) {
            return "";
        }
        size_t data_begin = stream_pos + 6U;
        if (data_begin < obj_body.size() && obj_body[data_begin] == '\r') {
            ++data_begin;
        }
        if (data_begin < obj_body.size() && obj_body[data_begin] == '\n') {
            ++data_begin;
        }
        const size_t endstream_pos = obj_body.find("endstream", data_begin);
        if (endstream_pos == std::string::npos || endstream_pos <= data_begin) {
            return "";
        }
        std::string raw = obj_body.substr(data_begin, endstream_pos - data_begin);
        if (obj_body.find("/FlateDecode") != std::string::npos) {
            raw = TryInflateZlibStream(raw);
        }
        return raw;
    };

    auto parse_content_refs = [](const std::string& page_obj_body) -> std::vector<int> {
        std::vector<int> refs;
        const std::regex array_re(R"(/Contents\s*\[([\s\S]*?)\])");
        std::smatch array_match;
        if (std::regex_search(page_obj_body, array_match, array_re)) {
            const std::string refs_body = array_match[1].str();
            const std::regex ref_re(R"((\d+)\s+\d+\s+R)");
            auto begin = std::sregex_iterator(refs_body.begin(), refs_body.end(), ref_re);
            auto end = std::sregex_iterator();
            for (auto it = begin; it != end; ++it) {
                refs.push_back(std::atoi((*it)[1].str().c_str()));
            }
            return refs;
        }
        const std::regex single_re(R"(/Contents\s+(\d+)\s+\d+\s+R)");
        std::smatch single_match;
        if (std::regex_search(page_obj_body, single_match, single_re)) {
            refs.push_back(std::atoi(single_match[1].str().c_str()));
        }
        return refs;
    };
    std::vector<PdfObj> objects;
    std::unordered_map<int, std::string> object_map;
    {
        const std::regex header_re(R"((\d+)\s+(\d+)\s+obj)");
        std::smatch m;
        std::string::const_iterator search_begin = pdf_content.cbegin();
        while (std::regex_search(search_begin, pdf_content.cend(), m, header_re)) {
            const int obj_id = std::atoi(m[1].str().c_str());
            const size_t header_offset = static_cast<size_t>(m.position(0) + (search_begin - pdf_content.cbegin()));
            const size_t body_begin = header_offset + static_cast<size_t>(m.length(0));
            const size_t body_end = pdf_content.find("endobj", body_begin);
            if (body_end == std::string::npos || body_end <= body_begin) {
                break;
            }
            const std::string obj_body = pdf_content.substr(body_begin, body_end - body_begin);
            objects.push_back(PdfObj{obj_id, obj_body});
            object_map[obj_id] = obj_body;
            search_begin = pdf_content.cbegin() + static_cast<std::ptrdiff_t>(body_end + 6U);
        }
    }
    auto parse_objstm_int = [](const std::string& body, const std::string& key, int* out) -> bool {
        if (out == nullptr) {
            return false;
        }
        std::smatch m;
        const std::regex re("/" + key + R"(\s+(\d+))");
        if (!std::regex_search(body, m, re)) {
            return false;
        }
        *out = std::atoi(m[1].str().c_str());
        return *out > 0;
    };
    {
        std::vector<PdfObj> embedded;
        for (const auto& obj : objects) {
            if (!HasPdfNameEntry(obj.body, "Type", "ObjStm")) {
                continue;
            }
            int n = 0;
            int first = 0;
            if (!parse_objstm_int(obj.body, "N", &n) || !parse_objstm_int(obj.body, "First", &first)) {
                continue;
            }
            const std::string stream = extract_stream(obj.body);
            if (stream.empty() || first <= 0 || static_cast<size_t>(first) >= stream.size()) {
                continue;
            }
            const std::string header = stream.substr(0, static_cast<size_t>(first));
            std::stringstream hs(header);
            std::vector<int> nums;
            int v = 0;
            while (hs >> v) {
                nums.push_back(v);
            }
            if (nums.size() < static_cast<size_t>(n * 2)) {
                continue;
            }
            for (int i = 0; i < n; ++i) {
                const int obj_id = nums[static_cast<size_t>(i * 2)];
                const int off = nums[static_cast<size_t>(i * 2 + 1)];
                if (off < 0) {
                    continue;
                }
                const size_t body_start = static_cast<size_t>(first + off);
                const size_t body_end = (i + 1 < n)
                                            ? static_cast<size_t>(first + nums[static_cast<size_t>((i + 1) * 2 + 1)])
                                            : stream.size();
                if (body_start >= stream.size() || body_end <= body_start || body_end > stream.size()) {
                    continue;
                }
                std::string emb = TrimAscii(stream.substr(body_start, body_end - body_start));
                if (emb.empty()) {
                    continue;
                }
                object_map[obj_id] = emb;
                embedded.push_back(PdfObj{obj_id, std::move(emb)});
            }
        }
        objects.insert(objects.end(), embedded.begin(), embedded.end());
    }

    std::unordered_map<int, size_t> last_obj_index;
    for (size_t i = 0; i < objects.size(); ++i) {
        last_obj_index[objects[i].id] = i;
    }
    auto parse_kids_refs = [](const std::string& pages_obj_body) -> std::vector<int> {
        std::vector<int> out;
        std::smatch m;
        const std::regex kids_re(R"(/Kids\s*\[([\s\S]*?)\])");
        if (!std::regex_search(pages_obj_body, m, kids_re)) {
            return out;
        }
        const std::regex ref_re(R"((\d+)\s+\d+\s+R)");
        const std::string body = m[1].str();
        auto begin = std::sregex_iterator(body.begin(), body.end(), ref_re);
        auto end = std::sregex_iterator();
        for (auto it = begin; it != end; ++it) {
            out.push_back(std::atoi((*it)[1].str().c_str()));
        }
        return out;
    };
    std::vector<size_t> page_indices;
    std::unordered_set<int> page_ids_added;
    for (size_t i = 0; i < objects.size(); ++i) {
        auto li = last_obj_index.find(objects[i].id);
        if (li != last_obj_index.end() && li->second != i) {
            continue;
        }
        if (!HasPdfNameEntry(objects[i].body, "Type", "Pages")) {
            continue;
        }
        const auto kids = parse_kids_refs(objects[i].body);
        for (int pid : kids) {
            auto pi = last_obj_index.find(pid);
            if (pi == last_obj_index.end()) {
                continue;
            }
            const auto& pobj = objects[pi->second];
            if (!HasPdfNameEntry(pobj.body, "Type", "Page") || HasPdfNameEntry(pobj.body, "Type", "Pages")) {
                continue;
            }
            if (page_ids_added.insert(pid).second) {
                page_indices.push_back(pi->second);
            }
        }
    }
    for (size_t i = 0; i < objects.size(); ++i) {
        auto li = last_obj_index.find(objects[i].id);
        if (li != last_obj_index.end() && li->second != i) {
            continue;
        }
        if (!HasPdfNameEntry(objects[i].body, "Type", "Page") || HasPdfNameEntry(objects[i].body, "Type", "Pages")) {
            continue;
        }
        if (page_ids_added.insert(objects[i].id).second) {
            page_indices.push_back(i);
        }
    }

    std::vector<std::string> pages;
    size_t page_object_count = 0;
    for (size_t obj_idx : page_indices) {
        const auto& obj = objects[obj_idx];
        ++page_object_count;

        const PdfPageFontMaps page_font_maps = ParsePageFontMaps(obj.body, object_map, extract_stream);
        PdfFontCMap merged_cmap;
        for (const auto& kv : page_font_maps) {
            merged_cmap.insert(kv.second.begin(), kv.second.end());
        }

        auto extract_stream_tokens = [&](const std::string& stream_text) -> std::vector<std::string> {
            auto skip_ws = [&](size_t* idx) {
                while (*idx < stream_text.size() && std::isspace(static_cast<unsigned char>(stream_text[*idx]))) {
                    ++(*idx);
                }
            };
            auto read_name_from = [&](const std::string& src, size_t start, std::string* out, size_t* next) -> bool {
                if (start >= src.size() || src[start] != '/' || out == nullptr || next == nullptr) {
                    return false;
                }
                size_t i = start + 1U;
                while (i < src.size()) {
                    const unsigned char c = static_cast<unsigned char>(src[i]);
                    if (std::isspace(c) || c == '/' || c == '[' || c == ']' || c == '<' || c == '>' || c == '(' || c == ')') {
                        break;
                    }
                    ++i;
                }
                if (i <= start + 1U) {
                    return false;
                }
                *out = src.substr(start + 1U, i - (start + 1U));
                *next = i;
                return true;
            };
            auto read_number_from = [&](const std::string& src, size_t start, std::string* out, size_t* next) -> bool {
                if (start >= src.size() || out == nullptr || next == nullptr) {
                    return false;
                }
                size_t i = start;
                if (src[i] == '+' || src[i] == '-') {
                    ++i;
                }
                bool has_digit = false;
                while (i < src.size() && std::isdigit(static_cast<unsigned char>(src[i]))) {
                    has_digit = true;
                    ++i;
                }
                if (i < src.size() && src[i] == '.') {
                    ++i;
                    while (i < src.size() && std::isdigit(static_cast<unsigned char>(src[i]))) {
                        has_digit = true;
                        ++i;
                    }
                }
                if (!has_digit) {
                    return false;
                }
                *out = src.substr(start, i - start);
                *next = i;
                return true;
            };
            auto parse_literal_from = [&](const std::string& src, size_t start, std::string* out, size_t* next) -> bool {
                if (start >= src.size() || src[start] != '(' || out == nullptr || next == nullptr) {
                    return false;
                }
                int nested = 1;
                bool escaping = false;
                std::string raw;
                for (size_t i = start + 1U; i < src.size(); ++i) {
                    const char c = src[i];
                    if (escaping) {
                        raw.push_back('\\');
                        raw.push_back(c);
                        escaping = false;
                        continue;
                    }
                    if (c == '\\') {
                        escaping = true;
                        continue;
                    }
                    if (c == '(') {
                        ++nested;
                        raw.push_back(c);
                        continue;
                    }
                    if (c == ')') {
                        --nested;
                        if (nested == 0) {
                            *out = UnescapePdfString(raw);
                            *next = i + 1U;
                            return true;
                        }
                        raw.push_back(c);
                        continue;
                    }
                    raw.push_back(c);
                }
                return false;
            };
            auto parse_hex_from = [&](const std::string& src, size_t start, std::string* out, size_t* next) -> bool {
                if (start >= src.size() || src[start] != '<' || out == nullptr || next == nullptr) {
                    return false;
                }
                if (start + 1U < src.size() && src[start + 1U] == '<') {
                    return false;
                }
                const size_t end = src.find('>', start + 1U);
                if (end == std::string::npos) {
                    return false;
                }
                *out = src.substr(start, end - start + 1U);
                *next = end + 1U;
                return true;
            };
            auto read_op = [&](size_t* idx) -> std::string {
                skip_ws(idx);
                size_t j = *idx;
                while (j < stream_text.size() && std::isalpha(static_cast<unsigned char>(stream_text[j]))) {
                    ++j;
                }
                const std::string op = stream_text.substr(*idx, j - *idx);
                *idx = j;
                return op;
            };

            std::vector<std::string> lines;
            std::string current_line;
            auto flush_line = [&]() {
                const std::string normalized = NormalizeExtractedToken(current_line);
                if (!normalized.empty()) {
                    lines.push_back(normalized);
                }
                current_line.clear();
            };
            auto append_token = [&](const std::string& raw_token) {
                const std::string token = NormalizeExtractedToken(raw_token);
                if (token.empty()) {
                    return;
                }
                if (!merge_lines) {
                    lines.push_back(token);
                    return;
                }
                if (current_line.empty()) {
                    current_line = token;
                    return;
                }
                const char prev = current_line.back();
                const char next = token.front();
                const bool prev_alnum = std::isalnum(static_cast<unsigned char>(prev)) != 0;
                const bool next_alnum = std::isalnum(static_cast<unsigned char>(next)) != 0;
                if (prev_alnum && next_alnum) {
                    current_line.push_back(' ');
                }
                current_line += token;
            };
            std::string active_font_name;
            for (size_t i = 0; i < stream_text.size();) {
                if (std::isalpha(static_cast<unsigned char>(stream_text[i]))) {
                    size_t op_idx = i;
                    const std::string op = read_op(&op_idx);
                    if (op == "BT" || op == "ET" || op == "Td" || op == "TD" || op == "T*") {
                        if (op != "BT") {
                            flush_line();
                        }
                        i = op_idx;
                        continue;
                    }
                }
                if (stream_text[i] == '/') {
                    std::string font_name;
                    size_t after_name = i;
                    if (read_name_from(stream_text, i, &font_name, &after_name)) {
                        size_t cursor = after_name;
                        skip_ws(&cursor);
                        std::string font_size_token;
                        size_t after_num = cursor;
                        if (read_number_from(stream_text, cursor, &font_size_token, &after_num)) {
                            size_t op_idx = after_num;
                            const std::string op = read_op(&op_idx);
                            if (op == "Tf") {
                                active_font_name = font_name;
                                i = op_idx;
                                continue;
                            }
                        }
                    }
                }
                if (stream_text[i] == '[') {
                    size_t j = i + 1U;
                    int bracket = 1;
                    while (j < stream_text.size() && bracket > 0) {
                        if (stream_text[j] == '[') {
                            ++bracket;
                        } else if (stream_text[j] == ']') {
                            --bracket;
                        }
                        ++j;
                    }
                    if (bracket != 0) {
                        break;
                    }
                    size_t op_idx = j;
                    const std::string op = read_op(&op_idx);
                    if (op == "TJ") {
                        const std::string body = stream_text.substr(i + 1U, (j - 1U) - (i + 1U) + 1U);
                        const PdfFontCMap* active_map = nullptr;
                        auto active_it = page_font_maps.find(active_font_name);
                        if (active_it != page_font_maps.end()) {
                            active_map = &active_it->second;
                        }
                        std::string merged;
                        for (size_t k = 0; k < body.size();) {
                            std::string literal;
                            size_t next = k;
                            if (parse_literal_from(body, k, &literal, &next)) {
                                merged += literal;
                                k = next;
                                continue;
                            }
                            std::string hex;
                            if (parse_hex_from(body, k, &hex, &next)) {
                                merged += DecodePdfHexToken(hex, active_map, &merged_cmap);
                                k = next;
                                continue;
                            }
                            ++k;
                        }
                        append_token(merged);
                        i = op_idx;
                        continue;
                    }
                    i = j;
                    continue;
                }
                if (stream_text[i] == '<') {
                    std::string hex;
                    size_t next = i;
                    if (parse_hex_from(stream_text, i, &hex, &next)) {
                        const PdfFontCMap* active_map = nullptr;
                        auto active_it = page_font_maps.find(active_font_name);
                        if (active_it != page_font_maps.end()) {
                            active_map = &active_it->second;
                        }
                        size_t op_idx = next;
                        const std::string op = read_op(&op_idx);
                        if (op == "Tj") {
                            const std::string decoded = DecodePdfHexToken(hex, active_map, &merged_cmap);
                            append_token(decoded);
                            i = op_idx;
                            continue;
                        }
                    }
                }
                if (stream_text[i] == '(') {
                    std::string literal;
                    size_t next = i;
                    if (parse_literal_from(stream_text, i, &literal, &next)) {
                        size_t op_idx = next;
                        const std::string op = read_op(&op_idx);
                        if (op == "Tj") {
                            append_token(literal);
                            i = op_idx;
                            continue;
                        }
                    }
                }
                ++i;
            }
            flush_line();

            if (lines.empty()) {
                const auto fallback_tokens = ExtractStringsFromPdfStream(stream_text);
                for (const auto& tk : fallback_tokens) {
                    const std::string normalized = NormalizeExtractedToken(tk);
                    if (!normalized.empty()) {
                        lines.push_back(normalized);
                    }
                }
            }
            if (merge_lines) {
                return MergeContinuationLines(lines);
            }
            return lines;
        };

        std::vector<std::string> all_tokens;
        const auto refs = parse_content_refs(obj.body);
        if (!refs.empty()) {
            for (int ref : refs) {
                auto map_it = object_map.find(ref);
                if (map_it == object_map.end()) {
                    continue;
                }
                const std::string stream_text = extract_stream(map_it->second);
                if (stream_text.empty()) {
                    continue;
                }
                const auto tokens = extract_stream_tokens(stream_text);
                all_tokens.insert(all_tokens.end(), tokens.begin(), tokens.end());
            }
        } else {
            const std::string stream_text = extract_stream(obj.body);
            if (!stream_text.empty()) {
                const auto tokens = extract_stream_tokens(stream_text);
                all_tokens.insert(all_tokens.end(), tokens.begin(), tokens.end());
            }
        }

        std::ostringstream merged_page_text;
        bool first = true;
        for (const auto& raw_token : all_tokens) {
            const std::string token = NormalizeExtractedToken(raw_token);
            if (token.empty()) {
                continue;
            }
            if (!first) {
                merged_page_text << "\n";
            }
            first = false;
            merged_page_text << token;
        }
        pages.push_back(merged_page_text.str());
    }

    if (!pages.empty()) {
        return pages;
    }
    if (page_object_count > 0) {
        return std::vector<std::string>(page_object_count, "");
    }
    return {};
}

static bool IsPdfNumberToken(const std::string& token) {
    if (token.empty()) {
        return false;
    }
    size_t i = 0;
    if (token[i] == '+' || token[i] == '-') {
        ++i;
    }
    bool has_digit = false;
    bool has_dot = false;
    for (; i < token.size(); ++i) {
        const char c = token[i];
        if (std::isdigit(static_cast<unsigned char>(c))) {
            has_digit = true;
            continue;
        }
        if (c == '.' && !has_dot) {
            has_dot = true;
            continue;
        }
        return false;
    }
    return has_digit;
}

static bool ParsePageMediaBoxPt(const std::string& page_body, double* out_w, double* out_h) {
    if (out_w == nullptr || out_h == nullptr) {
        return false;
    }
    const std::regex media_box_re(R"(/MediaBox\s*\[\s*([^\]]+)\s*\])");
    std::smatch m;
    if (!std::regex_search(page_body, m, media_box_re)) {
        return false;
    }
    std::stringstream ss(m[1].str());
    double x0 = 0.0;
    double y0 = 0.0;
    double x1 = 0.0;
    double y1 = 0.0;
    if (!(ss >> x0 >> y0 >> x1 >> y1)) {
        return false;
    }
    *out_w = std::max(1.0, std::abs(x1 - x0));
    *out_h = std::max(1.0, std::abs(y1 - y0));
    return true;
}

static bool ExtractBalancedDictBody(const std::string& text, size_t dict_open_pos, std::string* out_body, size_t* out_end_pos) {
    if (out_body == nullptr || dict_open_pos + 1U >= text.size() || text[dict_open_pos] != '<' || text[dict_open_pos + 1U] != '<') {
        return false;
    }
    int depth = 0;
    size_t i = dict_open_pos;
    for (; i + 1U < text.size(); ++i) {
        if (text[i] == '<' && text[i + 1U] == '<') {
            ++depth;
            ++i;
            continue;
        }
        if (text[i] == '>' && text[i + 1U] == '>') {
            --depth;
            if (depth == 0) {
                const size_t body_begin = dict_open_pos + 2U;
                *out_body = text.substr(body_begin, i - body_begin);
                if (out_end_pos != nullptr) {
                    *out_end_pos = i + 2U;
                }
                return true;
            }
            ++i;
        }
    }
    return false;
}

static std::string FindPageResourceBody(
    const std::string& page_body, const std::unordered_map<int, std::string>& object_map, int max_hops = 8) {
    auto find_resource_in = [&](const std::string& body) -> std::string {
        const size_t key_pos = body.find("/Resources");
        if (key_pos == std::string::npos) {
            return "";
        }
        size_t p = key_pos + std::string("/Resources").size();
        while (p < body.size() && std::isspace(static_cast<unsigned char>(body[p]))) {
            ++p;
        }
        if (p + 1U < body.size() && body[p] == '<' && body[p + 1U] == '<') {
            std::string dict_body;
            if (ExtractBalancedDictBody(body, p, &dict_body, nullptr)) {
                return dict_body;
            }
            return "";
        }
        std::smatch m;
        const std::regex ref_re(R"((\d+)\s+\d+\s+R)");
        const std::string rest = body.substr(p);
        if (std::regex_search(rest, m, ref_re)) {
            const int obj_id = std::atoi(m[1].str().c_str());
            auto it = object_map.find(obj_id);
            if (it == object_map.end()) {
                return "";
            }
            const size_t dict_pos = it->second.find("<<");
            if (dict_pos != std::string::npos) {
                std::string dict_body;
                if (ExtractBalancedDictBody(it->second, dict_pos, &dict_body, nullptr)) {
                    return dict_body;
                }
            }
            return it->second;
        }
        return "";
    };

    std::string res = find_resource_in(page_body);
    if (!res.empty()) {
        return res;
    }

    std::string cur = page_body;
    for (int i = 0; i < max_hops; ++i) {
        std::smatch pm;
        const std::regex parent_re(R"(/Parent\s+(\d+)\s+\d+\s+R)");
        if (!std::regex_search(cur, pm, parent_re)) {
            break;
        }
        const int parent_id = std::atoi(pm[1].str().c_str());
        auto it = object_map.find(parent_id);
        if (it == object_map.end()) {
            break;
        }
        cur = it->second;
        res = find_resource_in(cur);
        if (!res.empty()) {
            return res;
        }
    }
    return "";
}

static std::unordered_map<std::string, int> ParseXObjectRefsFromResource(const std::string& resource_body) {
    std::unordered_map<std::string, int> refs;
    if (resource_body.empty()) {
        return refs;
    }
    const size_t key_pos = resource_body.find("/XObject");
    if (key_pos == std::string::npos) {
        return refs;
    }
    size_t p = key_pos + std::string("/XObject").size();
    while (p < resource_body.size() && std::isspace(static_cast<unsigned char>(resource_body[p]))) {
        ++p;
    }
    if (!(p + 1U < resource_body.size() && resource_body[p] == '<' && resource_body[p + 1U] == '<')) {
        return refs;
    }
    std::string block;
    if (!ExtractBalancedDictBody(resource_body, p, &block, nullptr)) {
        return refs;
    }
    const std::regex ref_re(R"(/([A-Za-z0-9_]+)\s+(\d+)\s+\d+\s+R)");
    auto begin = std::sregex_iterator(block.begin(), block.end(), ref_re);
    auto end = std::sregex_iterator();
    for (auto it = begin; it != end; ++it) {
        refs[(*it)[1].str()] = std::atoi((*it)[2].str().c_str());
    }
    return refs;
}

static std::string ExtractDecodedPdfObjectStream(const std::string& obj_body) {
    const size_t stream_pos = obj_body.find("stream");
    if (stream_pos == std::string::npos) {
        return "";
    }
    size_t data_begin = stream_pos + 6U;
    if (data_begin < obj_body.size() && obj_body[data_begin] == '\r') {
        ++data_begin;
    }
    if (data_begin < obj_body.size() && obj_body[data_begin] == '\n') {
        ++data_begin;
    }
    const size_t endstream_pos = obj_body.find("endstream", data_begin);
    if (endstream_pos == std::string::npos || endstream_pos <= data_begin) {
        return "";
    }
    std::string raw = obj_body.substr(data_begin, endstream_pos - data_begin);
    if (obj_body.find("/FlateDecode") != std::string::npos) {
        raw = TryInflateZlibStream(raw);
    }
    return raw;
}

static std::array<double, 6> ParsePdfObjectMatrix(const std::string& obj_body) {
    std::array<double, 6> m = {1.0, 0.0, 0.0, 1.0, 0.0, 0.0};
    std::smatch mm;
    const std::regex matrix_re(R"(/Matrix\s*\[\s*([^\]]+)\])");
    if (!std::regex_search(obj_body, mm, matrix_re)) {
        return m;
    }
    std::stringstream ss(mm[1].str());
    double a = 1.0;
    double b = 0.0;
    double c = 0.0;
    double d = 1.0;
    double e = 0.0;
    double f = 0.0;
    if (ss >> a >> b >> c >> d >> e >> f) {
        m = {a, b, c, d, e, f};
    }
    return m;
}

static std::unordered_map<std::string, int> ResolveXObjectRefsForObjectBody(
    const std::string& obj_body, const std::unordered_map<int, std::string>& object_map) {
    auto refs = ParseXObjectRefsFromResource(FindPageResourceBody(obj_body, object_map));
    if (!refs.empty()) {
        return refs;
    }
    const size_t xo_key = obj_body.find("/XObject");
    if (xo_key != std::string::npos) {
        size_t p = xo_key + std::string("/XObject").size();
        while (p < obj_body.size() && std::isspace(static_cast<unsigned char>(obj_body[p]))) {
            ++p;
        }
        std::string xo_block;
        if (p + 1U < obj_body.size() && obj_body[p] == '<' && obj_body[p + 1U] == '<' &&
            ExtractBalancedDictBody(obj_body, p, &xo_block, nullptr)) {
            const std::regex ref_re(R"(/([A-Za-z0-9_]+)\s+(\d+)\s+\d+\s+R)");
            auto begin = std::sregex_iterator(xo_block.begin(), xo_block.end(), ref_re);
            auto end = std::sregex_iterator();
            for (auto it = begin; it != end; ++it) {
                refs[(*it)[1].str()] = std::atoi((*it)[2].str().c_str());
            }
        }
    }
    return refs;
}

static void ParsePdfPathAndImageUses(
    const std::string& stream_text, double page_h_pt, const std::unordered_map<std::string, int>& xobj_refs,
    const std::unordered_map<int, std::string>* object_map, std::vector<PdfToOfdImageUse>* out_images,
    std::vector<PdfToOfdPathUse>* out_paths, const PdfParseGraphicState* initial_state = nullptr, int depth = 0) {
    if (out_images == nullptr || out_paths == nullptr) {
        return;
    }
    std::stringstream ss(stream_text);
    std::vector<std::string> tokens;
    std::string t;
    while (ss >> t) {
        tokens.push_back(t);
    }
    if (tokens.empty()) {
        return;
    }
    if (depth > 5) {
        return;
    }

    auto mat_mul = [](const std::array<double, 6>& lhs, const std::array<double, 6>& rhs) -> std::array<double, 6> {
        // PDF affine matrix multiply (lhs followed by rhs).
        return {
            lhs[0] * rhs[0] + lhs[2] * rhs[1],  // a
            lhs[1] * rhs[0] + lhs[3] * rhs[1],  // b
            lhs[0] * rhs[2] + lhs[2] * rhs[3],  // c
            lhs[1] * rhs[2] + lhs[3] * rhs[3],  // d
            lhs[0] * rhs[4] + lhs[2] * rhs[5] + lhs[4],  // e
            lhs[1] * rhs[4] + lhs[3] * rhs[5] + lhs[5]   // f
        };
    };
    auto mat_apply = [](const std::array<double, 6>& m, double x, double y, double* out_x, double* out_y) {
        if (out_x == nullptr || out_y == nullptr) {
            return;
        }
        *out_x = m[0] * x + m[2] * y + m[4];
        *out_y = m[1] * x + m[3] * y + m[5];
    };

    std::vector<double> nums;
    PdfParseGraphicState gs;
    if (initial_state != nullptr) {
        gs = *initial_state;
    }
    std::vector<PdfParseGraphicState> gs_stack;
    std::vector<std::string> path_tokens;
    auto flush_path = [&](bool stroke, bool fill, bool close_path) {
        if (path_tokens.empty()) {
            return;
        }
        if (close_path) {
            path_tokens.push_back("B");
        }
        PdfToOfdPathUse p;
        p.stroke = stroke;
        p.fill = fill;
        const double det = gs.ctm[0] * gs.ctm[3] - gs.ctm[1] * gs.ctm[2];
        const double scale = std::sqrt(std::max(1e-6, std::abs(det)));
        p.line_width_mm = std::max(0.1, (gs.line_width_pt * scale) / kMmToPt);
        p.has_stroke_color = gs.has_stroke_color;
        p.has_fill_color = gs.has_fill_color;
        p.stroke_r = gs.stroke_r;
        p.stroke_g = gs.stroke_g;
        p.stroke_b = gs.stroke_b;
        p.fill_r = gs.fill_r;
        p.fill_g = gs.fill_g;
        p.fill_b = gs.fill_b;
        std::ostringstream ps;
        for (size_t i = 0; i < path_tokens.size(); ++i) {
            if (i > 0) {
                ps << ' ';
            }
            ps << path_tokens[i];
        }
        p.abbreviated_data = ps.str();
        if (!p.abbreviated_data.empty()) {
            out_paths->push_back(std::move(p));
        }
        path_tokens.clear();
    };

    auto push_mm = [&](double x_pt, double y_pt) {
        const double x_mm = x_pt / kMmToPt;
        const double y_mm = (page_h_pt - y_pt) / kMmToPt;
        path_tokens.push_back(std::to_string(x_mm));
        path_tokens.push_back(std::to_string(y_mm));
    };

    for (size_t i = 0; i < tokens.size(); ++i) {
        const std::string& op = tokens[i];
        if (IsPdfNumberToken(op)) {
            double v = 0.0;
            if (ParseDouble(op, &v)) {
                nums.push_back(v);
            }
            continue;
        }
        if (op == "q") {
            gs_stack.push_back(gs);
            nums.clear();
            continue;
        }
        if (op == "Q") {
            if (!gs_stack.empty()) {
                gs = gs_stack.back();
                gs_stack.pop_back();
            }
            nums.clear();
            continue;
        }
        if (op == "cm" && nums.size() >= 6U) {
            const std::array<double, 6> m = {
                nums[nums.size() - 6U], nums[nums.size() - 5U], nums[nums.size() - 4U],
                nums[nums.size() - 3U], nums[nums.size() - 2U], nums[nums.size() - 1U]};
            gs.ctm = mat_mul(gs.ctm, m);
            nums.clear();
            continue;
        }
        if (op == "w" && !nums.empty()) {
            gs.line_width_pt = std::max(0.1, nums.back());
            nums.clear();
            continue;
        }
        if (op == "RG" && nums.size() >= 3U) {
            gs.stroke_r = std::max(0, std::min(255, static_cast<int>(nums[nums.size() - 3U] * 255.0 + 0.5)));
            gs.stroke_g = std::max(0, std::min(255, static_cast<int>(nums[nums.size() - 2U] * 255.0 + 0.5)));
            gs.stroke_b = std::max(0, std::min(255, static_cast<int>(nums[nums.size() - 1U] * 255.0 + 0.5)));
            gs.has_stroke_color = true;
            nums.clear();
            continue;
        }
        if (op == "G" && !nums.empty()) {
            const int g = std::max(0, std::min(255, static_cast<int>(nums.back() * 255.0 + 0.5)));
            gs.stroke_r = g;
            gs.stroke_g = g;
            gs.stroke_b = g;
            gs.has_stroke_color = true;
            nums.clear();
            continue;
        }
        if (op == "rg" && nums.size() >= 3U) {
            gs.fill_r = std::max(0, std::min(255, static_cast<int>(nums[nums.size() - 3U] * 255.0 + 0.5)));
            gs.fill_g = std::max(0, std::min(255, static_cast<int>(nums[nums.size() - 2U] * 255.0 + 0.5)));
            gs.fill_b = std::max(0, std::min(255, static_cast<int>(nums[nums.size() - 1U] * 255.0 + 0.5)));
            gs.has_fill_color = true;
            nums.clear();
            continue;
        }
        if (op == "g" && !nums.empty()) {
            const int g = std::max(0, std::min(255, static_cast<int>(nums.back() * 255.0 + 0.5)));
            gs.fill_r = g;
            gs.fill_g = g;
            gs.fill_b = g;
            gs.has_fill_color = true;
            nums.clear();
            continue;
        }
        if (op == "Do" && i > 0U) {
            std::string name = tokens[i - 1U];
            if (!name.empty() && name[0] == '/') {
                name.erase(name.begin());
            }
            auto it = xobj_refs.find(name);
            if (it != xobj_refs.end()) {
                const int obj_id = it->second;
                bool handled = false;
                if (object_map != nullptr) {
                    auto oit = object_map->find(obj_id);
                    if (oit != object_map->end()) {
                        const std::string& xobj_body = oit->second;
                        if (HasPdfNameEntry(xobj_body, "Subtype", "Form")) {
                            const auto child_refs = ResolveXObjectRefsForObjectBody(xobj_body, *object_map);
                            const std::string child_stream = ExtractDecodedPdfObjectStream(xobj_body);
                            if (!child_stream.empty()) {
                                PdfParseGraphicState child_state = gs;
                                child_state.ctm = mat_mul(gs.ctm, ParsePdfObjectMatrix(xobj_body));
                                ParsePdfPathAndImageUses(
                                    child_stream, page_h_pt, child_refs, object_map, out_images, out_paths, &child_state, depth + 1);
                                handled = true;
                            }
                        } else if (HasPdfNameEntry(xobj_body, "Subtype", "Image")) {
                            double x0 = 0.0;
                            double y0 = 0.0;
                            double x1 = 0.0;
                            double y1 = 0.0;
                            double x2 = 0.0;
                            double y2 = 0.0;
                            double x3 = 0.0;
                            double y3 = 0.0;
                            mat_apply(gs.ctm, 0.0, 0.0, &x0, &y0);
                            mat_apply(gs.ctm, 1.0, 0.0, &x1, &y1);
                            mat_apply(gs.ctm, 0.0, 1.0, &x2, &y2);
                            mat_apply(gs.ctm, 1.0, 1.0, &x3, &y3);
                            const double min_x = std::min(std::min(x0, x1), std::min(x2, x3));
                            const double max_x = std::max(std::max(x0, x1), std::max(x2, x3));
                            const double min_y = std::min(std::min(y0, y1), std::min(y2, y3));
                            const double max_y = std::max(std::max(y0, y1), std::max(y2, y3));
                            const double w_pt = std::max(1.0, max_x - min_x);
                            const double h_pt = std::max(1.0, max_y - min_y);
                            const double x_pt = min_x;
                            const double y_top_pt = page_h_pt - max_y;
                            PdfToOfdImageUse u;
                            u.object_id = obj_id;
                            u.x_mm = x_pt / kMmToPt;
                            u.y_mm = std::max(0.0, y_top_pt / kMmToPt);
                            u.w_mm = w_pt / kMmToPt;
                            u.h_mm = h_pt / kMmToPt;
                            out_images->push_back(std::move(u));
                            handled = true;
                        }
                    }
                }
                if (!handled) {
                    double x0 = 0.0;
                    double y0 = 0.0;
                    double x1 = 0.0;
                    double y1 = 0.0;
                    double x2 = 0.0;
                    double y2 = 0.0;
                    double x3 = 0.0;
                    double y3 = 0.0;
                    mat_apply(gs.ctm, 0.0, 0.0, &x0, &y0);
                    mat_apply(gs.ctm, 1.0, 0.0, &x1, &y1);
                    mat_apply(gs.ctm, 0.0, 1.0, &x2, &y2);
                    mat_apply(gs.ctm, 1.0, 1.0, &x3, &y3);
                    const double min_x = std::min(std::min(x0, x1), std::min(x2, x3));
                    const double max_x = std::max(std::max(x0, x1), std::max(x2, x3));
                    const double min_y = std::min(std::min(y0, y1), std::min(y2, y3));
                    const double max_y = std::max(std::max(y0, y1), std::max(y2, y3));
                    const double w_pt = std::max(1.0, max_x - min_x);
                    const double h_pt = std::max(1.0, max_y - min_y);
                    const double x_pt = min_x;
                    const double y_top_pt = page_h_pt - max_y;
                    PdfToOfdImageUse u;
                    u.object_id = obj_id;
                    u.x_mm = x_pt / kMmToPt;
                    u.y_mm = std::max(0.0, y_top_pt / kMmToPt);
                    u.w_mm = w_pt / kMmToPt;
                    u.h_mm = h_pt / kMmToPt;
                    out_images->push_back(std::move(u));
                }
            }
            nums.clear();
            continue;
        }
        if (op == "m" && nums.size() >= 2U) {
            const double x = nums[nums.size() - 2U];
            const double y = nums[nums.size() - 1U];
            double tx = x;
            double ty = y;
            mat_apply(gs.ctm, x, y, &tx, &ty);
            path_tokens.push_back("M");
            push_mm(tx, ty);
            nums.clear();
            continue;
        }
        if (op == "l" && nums.size() >= 2U) {
            const double x = nums[nums.size() - 2U];
            const double y = nums[nums.size() - 1U];
            double tx = x;
            double ty = y;
            mat_apply(gs.ctm, x, y, &tx, &ty);
            path_tokens.push_back("L");
            push_mm(tx, ty);
            nums.clear();
            continue;
        }
        if (op == "c" && nums.size() >= 6U) {
            path_tokens.push_back("Q");
            double tx1 = 0.0;
            double ty1 = 0.0;
            double tx2 = 0.0;
            double ty2 = 0.0;
            double tx3 = 0.0;
            double ty3 = 0.0;
            mat_apply(gs.ctm, nums[nums.size() - 6U], nums[nums.size() - 5U], &tx1, &ty1);
            mat_apply(gs.ctm, nums[nums.size() - 4U], nums[nums.size() - 3U], &tx2, &ty2);
            mat_apply(gs.ctm, nums[nums.size() - 2U], nums[nums.size() - 1U], &tx3, &ty3);
            push_mm(tx1, ty1);
            push_mm(tx2, ty2);
            push_mm(tx3, ty3);
            nums.clear();
            continue;
        }
        if (op == "re" && nums.size() >= 4U) {
            const double x = nums[nums.size() - 4U];
            const double y = nums[nums.size() - 3U];
            const double w = nums[nums.size() - 2U];
            const double h = nums[nums.size() - 1U];
            double x0 = 0.0;
            double y0 = 0.0;
            double x1 = 0.0;
            double y1 = 0.0;
            double x2 = 0.0;
            double y2 = 0.0;
            double x3 = 0.0;
            double y3 = 0.0;
            mat_apply(gs.ctm, x, y, &x0, &y0);
            mat_apply(gs.ctm, x + w, y, &x1, &y1);
            mat_apply(gs.ctm, x + w, y + h, &x2, &y2);
            mat_apply(gs.ctm, x, y + h, &x3, &y3);
            path_tokens.push_back("M");
            push_mm(x0, y0);
            path_tokens.push_back("L");
            push_mm(x1, y1);
            path_tokens.push_back("L");
            push_mm(x2, y2);
            path_tokens.push_back("L");
            push_mm(x3, y3);
            path_tokens.push_back("B");
            nums.clear();
            continue;
        }
        if (op == "h") {
            path_tokens.push_back("B");
            nums.clear();
            continue;
        }
        if (op == "S") {
            flush_path(true, false, false);
            nums.clear();
            continue;
        }
        if (op == "s") {
            flush_path(true, false, true);
            nums.clear();
            continue;
        }
        if (op == "f" || op == "f*") {
            flush_path(false, true, false);
            nums.clear();
            continue;
        }
        if (op == "B" || op == "B*") {
            flush_path(true, true, false);
            nums.clear();
            continue;
        }
        if (op == "b" || op == "b*") {
            flush_path(true, true, true);
            nums.clear();
            continue;
        }
        if (op == "n") {
            path_tokens.clear();
            nums.clear();
            continue;
        }
        nums.clear();
    }
}

static void ParsePdfTextUses(
    const std::string& stream_text, double page_h_pt, const PdfPageFontMaps& page_font_maps,
    const std::unordered_map<std::string, int>& xobj_refs, const std::unordered_map<int, std::string>* object_map,
    std::vector<PdfToOfdTextUse>* out_texts, const std::array<double, 6>& initial_ctm = {1.0, 0.0, 0.0, 1.0, 0.0, 0.0},
    int depth = 0) {
    if (out_texts == nullptr) {
        return;
    }
    if (depth > 5) {
        return;
    }
    auto mat_mul = [](const std::array<double, 6>& lhs, const std::array<double, 6>& rhs) -> std::array<double, 6> {
        return {
            lhs[0] * rhs[0] + lhs[2] * rhs[1], lhs[1] * rhs[0] + lhs[3] * rhs[1],
            lhs[0] * rhs[2] + lhs[2] * rhs[3], lhs[1] * rhs[2] + lhs[3] * rhs[3],
            lhs[0] * rhs[4] + lhs[2] * rhs[5] + lhs[4], lhs[1] * rhs[4] + lhs[3] * rhs[5] + lhs[5]};
    };
    auto mat_apply = [](const std::array<double, 6>& m, double x, double y, double* out_x, double* out_y) {
        if (out_x == nullptr || out_y == nullptr) {
            return;
        }
        *out_x = m[0] * x + m[2] * y + m[4];
        *out_y = m[1] * x + m[3] * y + m[5];
    };
    auto skip_ws = [&](size_t* idx) {
        while (*idx < stream_text.size() && std::isspace(static_cast<unsigned char>(stream_text[*idx]))) {
            ++(*idx);
        }
    };
    auto read_name_from = [&](const std::string& src, size_t start, std::string* out, size_t* next) -> bool {
        if (start >= src.size() || src[start] != '/' || out == nullptr || next == nullptr) {
            return false;
        }
        size_t i = start + 1U;
        while (i < src.size()) {
            const unsigned char c = static_cast<unsigned char>(src[i]);
            if (std::isspace(c) || c == '/' || c == '[' || c == ']' || c == '<' || c == '>' || c == '(' || c == ')') {
                break;
            }
            ++i;
        }
        if (i <= start + 1U) {
            return false;
        }
        *out = src.substr(start + 1U, i - (start + 1U));
        *next = i;
        return true;
    };
    auto read_number_from = [&](const std::string& src, size_t start, std::string* out, size_t* next) -> bool {
        if (start >= src.size() || out == nullptr || next == nullptr) {
            return false;
        }
        size_t i = start;
        if (src[i] == '+' || src[i] == '-') {
            ++i;
        }
        bool has_digit = false;
        while (i < src.size() && std::isdigit(static_cast<unsigned char>(src[i]))) {
            has_digit = true;
            ++i;
        }
        if (i < src.size() && src[i] == '.') {
            ++i;
            while (i < src.size() && std::isdigit(static_cast<unsigned char>(src[i]))) {
                has_digit = true;
                ++i;
            }
        }
        if (!has_digit) {
            return false;
        }
        *out = src.substr(start, i - start);
        *next = i;
        return true;
    };
    auto parse_literal_from = [&](const std::string& src, size_t start, std::string* out, size_t* next) -> bool {
        if (start >= src.size() || src[start] != '(' || out == nullptr || next == nullptr) {
            return false;
        }
        int nested = 1;
        bool escaping = false;
        std::string raw;
        for (size_t i = start + 1U; i < src.size(); ++i) {
            const char c = src[i];
            if (escaping) {
                raw.push_back('\\');
                raw.push_back(c);
                escaping = false;
                continue;
            }
            if (c == '\\') {
                escaping = true;
                continue;
            }
            if (c == '(') {
                ++nested;
                raw.push_back(c);
                continue;
            }
            if (c == ')') {
                --nested;
                if (nested == 0) {
                    *out = UnescapePdfString(raw);
                    *next = i + 1U;
                    return true;
                }
                raw.push_back(c);
                continue;
            }
            raw.push_back(c);
        }
        return false;
    };
    auto parse_hex_from = [&](const std::string& src, size_t start, std::string* out, size_t* next) -> bool {
        if (start >= src.size() || src[start] != '<' || out == nullptr || next == nullptr) {
            return false;
        }
        if (start + 1U < src.size() && src[start + 1U] == '<') {
            return false;
        }
        const size_t end = src.find('>', start + 1U);
        if (end == std::string::npos) {
            return false;
        }
        *out = src.substr(start, end - start + 1U);
        *next = end + 1U;
        return true;
    };
    auto read_op = [&](size_t* idx) -> std::string {
        skip_ws(idx);
        size_t j = *idx;
        while (j < stream_text.size() && std::isalpha(static_cast<unsigned char>(stream_text[j]))) {
            ++j;
        }
        const std::string op = stream_text.substr(*idx, j - *idx);
        *idx = j;
        return op;
    };

    std::array<double, 6> text_matrix = {1, 0, 0, 1, 0, 0};
    std::array<double, 6> current_ctm = initial_ctm;
    std::vector<std::array<double, 6>> ctm_stack;
    double text_pos_x = 0.0;
    double text_pos_y = 0.0;
    double font_size_pt = 12.0;
    double leading_pt = 0.0;
    std::string active_font_name;
    int fill_r = 0;
    int fill_g = 0;
    int fill_b = 0;
    bool has_fill_color = false;

    PdfFontCMap merged_cmap;
    for (const auto& kv : page_font_maps) {
        merged_cmap.insert(kv.second.begin(), kv.second.end());
    }

    auto emit_text = [&](const std::string& raw_text, const std::vector<double>* delta_override_mm = nullptr) {
        const std::string text = NormalizeExtractedToken(raw_text);
        if (text.empty()) {
            return;
        }
        PdfToOfdTextUse t;
        t.text = text;
        t.size_mm = std::max(2.0, font_size_pt / kMmToPt);
        double gx = text_pos_x;
        double gy = text_pos_y;
        mat_apply(current_ctm, text_pos_x, text_pos_y, &gx, &gy);
        t.x_mm = std::max(0.0, gx / kMmToPt);
        t.y_mm = std::max(0.0, (page_h_pt - gy - font_size_pt) / kMmToPt);
        t.has_fill_color = has_fill_color;
        t.fill_r = fill_r;
        t.fill_g = fill_g;
        t.fill_b = fill_b;
        const auto glyphs = SplitUtf8Codepoints(text);
        if (glyphs.size() >= 2U) {
            if (delta_override_mm != nullptr && delta_override_mm->size() == glyphs.size() - 1U) {
                t.delta_x_mm = *delta_override_mm;
            } else {
                const double adv_mm = std::max(0.2, (font_size_pt * 0.55) / kMmToPt);
                t.delta_x_mm.assign(glyphs.size() - 1U, adv_mm);
            }
        }
        out_texts->push_back(std::move(t));

        text_pos_x += std::max(1.0, font_size_pt * 0.55 * static_cast<double>(glyphs.empty() ? text.size() : glyphs.size()));
    };

    for (size_t i = 0; i < stream_text.size();) {
        if (stream_text[i] == '/') {
            std::string font_name;
            size_t after_name = i;
            if (read_name_from(stream_text, i, &font_name, &after_name)) {
                size_t cursor = after_name;
                skip_ws(&cursor);
                std::string font_size_token;
                size_t after_num = cursor;
                if (read_number_from(stream_text, cursor, &font_size_token, &after_num)) {
                    size_t op_idx = after_num;
                    const std::string op = read_op(&op_idx);
                    if (op == "Tf") {
                        active_font_name = font_name;
                        ParseDouble(font_size_token, &font_size_pt);
                        i = op_idx;
                        continue;
                    }
                }
            }
        }
        if (stream_text[i] == '[') {
            size_t j = i + 1U;
            int bracket = 1;
            while (j < stream_text.size() && bracket > 0) {
                if (stream_text[j] == '[') {
                    ++bracket;
                } else if (stream_text[j] == ']') {
                    --bracket;
                }
                ++j;
            }
            if (bracket != 0) {
                break;
            }
            size_t op_idx = j;
            const std::string op = read_op(&op_idx);
            if (op == "TJ") {
                const std::string body = stream_text.substr(i + 1U, (j - 1U) - (i + 1U) + 1U);
                const PdfFontCMap* active_map = nullptr;
                auto active_it = page_font_maps.find(active_font_name);
                if (active_it != page_font_maps.end()) {
                    active_map = &active_it->second;
                }
                struct Piece {
                    std::string text;
                    double pre_adjust_pt = 0.0;
                };
                std::vector<Piece> pieces;
                double pending_adjust_pt = 0.0;
                for (size_t k = 0; k < body.size();) {
                    std::string literal;
                    size_t next = k;
                    if (parse_literal_from(body, k, &literal, &next)) {
                        const std::string normalized = NormalizeExtractedToken(literal);
                        if (!normalized.empty()) {
                            pieces.push_back(Piece{normalized, pending_adjust_pt});
                            pending_adjust_pt = 0.0;
                        }
                        k = next;
                        continue;
                    }
                    std::string hex;
                    if (parse_hex_from(body, k, &hex, &next)) {
                        const std::string decoded = NormalizeExtractedToken(DecodePdfHexToken(hex, active_map, &merged_cmap));
                        if (!decoded.empty()) {
                            pieces.push_back(Piece{decoded, pending_adjust_pt});
                            pending_adjust_pt = 0.0;
                        }
                        k = next;
                        continue;
                    }
                    std::string num_tok;
                    if (read_number_from(body, k, &num_tok, &next)) {
                        double num = 0.0;
                        if (ParseDouble(num_tok, &num)) {
                            // TJ number adjusts next glyph position in text space units.
                            pending_adjust_pt += (-num / 1000.0) * font_size_pt;
                        }
                        k = next;
                        continue;
                    }
                    ++k;
                }
                if (!pieces.empty()) {
                    std::string merged;
                    std::vector<double> delta_mm;
                    const double base_adv_mm = std::max(0.2, (font_size_pt * 0.55) / kMmToPt);
                    for (size_t pi = 0; pi < pieces.size(); ++pi) {
                        const auto& piece = pieces[pi];
                        const auto glyphs = SplitUtf8Codepoints(piece.text);
                        const size_t n = glyphs.empty() ? piece.text.size() : glyphs.size();
                        if (n == 0U) {
                            continue;
                        }
                        if (!merged.empty()) {
                            const double bridge_mm = std::max(0.05, base_adv_mm + piece.pre_adjust_pt / kMmToPt);
                            delta_mm.push_back(bridge_mm);
                        }
                        merged += piece.text;
                        for (size_t gi = 1; gi < n; ++gi) {
                            delta_mm.push_back(base_adv_mm);
                        }
                    }
                    emit_text(merged, &delta_mm);
                }
                i = op_idx;
                continue;
            }
            i = j;
            continue;
        }
        if (stream_text[i] == '<') {
            std::string hex;
            size_t next = i;
            if (parse_hex_from(stream_text, i, &hex, &next)) {
                const PdfFontCMap* active_map = nullptr;
                auto active_it = page_font_maps.find(active_font_name);
                if (active_it != page_font_maps.end()) {
                    active_map = &active_it->second;
                }
                size_t op_idx = next;
                const std::string op = read_op(&op_idx);
                if (op == "Tj") {
                    emit_text(DecodePdfHexToken(hex, active_map, &merged_cmap));
                    i = op_idx;
                    continue;
                }
            }
        }
        if (stream_text[i] == '(') {
            std::string literal;
            size_t next = i;
            if (parse_literal_from(stream_text, i, &literal, &next)) {
                size_t op_idx = next;
                const std::string op = read_op(&op_idx);
                if (op == "Tj") {
                    emit_text(literal);
                    i = op_idx;
                    continue;
                }
            }
        }
        if (std::isalpha(static_cast<unsigned char>(stream_text[i]))) {
            size_t op_idx = i;
            const std::string op = read_op(&op_idx);
            if (op == "Tm") {
                size_t cursor = i;
                while (cursor > 0U && std::isspace(static_cast<unsigned char>(stream_text[cursor - 1U]))) {
                    --cursor;
                }
                size_t start = cursor;
                while (start > 0U && !std::isalpha(static_cast<unsigned char>(stream_text[start - 1U])) &&
                       stream_text[start - 1U] != '\n' && stream_text[start - 1U] != '\r') {
                    --start;
                }
                std::stringstream ns(stream_text.substr(start, cursor - start));
                double a = 1.0;
                double b = 0.0;
                double c = 0.0;
                double d = 1.0;
                double e = 0.0;
                double f = 0.0;
                if (ns >> a >> b >> c >> d >> e >> f) {
                    text_matrix = {a, b, c, d, e, f};
                    text_pos_x = e;
                    text_pos_y = f;
                }
                i = op_idx;
                continue;
            }
            if (op == "q") {
                ctm_stack.push_back(current_ctm);
                i = op_idx;
                continue;
            }
            if (op == "Q") {
                if (!ctm_stack.empty()) {
                    current_ctm = ctm_stack.back();
                    ctm_stack.pop_back();
                }
                i = op_idx;
                continue;
            }
            if (op == "cm") {
                size_t cursor = i;
                while (cursor > 0U && std::isspace(static_cast<unsigned char>(stream_text[cursor - 1U]))) {
                    --cursor;
                }
                size_t start = cursor;
                while (start > 0U && !std::isalpha(static_cast<unsigned char>(stream_text[start - 1U])) &&
                       stream_text[start - 1U] != '\n' && stream_text[start - 1U] != '\r') {
                    --start;
                }
                std::stringstream ns(stream_text.substr(start, cursor - start));
                double a = 1.0;
                double b = 0.0;
                double c = 0.0;
                double d = 1.0;
                double e = 0.0;
                double f = 0.0;
                if (ns >> a >> b >> c >> d >> e >> f) {
                    current_ctm = mat_mul(current_ctm, {a, b, c, d, e, f});
                }
                i = op_idx;
                continue;
            }
            if (op == "Do" && i > 0U && object_map != nullptr) {
                std::string name = stream_text.substr(i, op_idx - i);
                (void)name;
                size_t name_end = i;
                while (name_end > 0U && std::isspace(static_cast<unsigned char>(stream_text[name_end - 1U]))) {
                    --name_end;
                }
                size_t name_begin = name_end;
                while (name_begin > 0U && !std::isspace(static_cast<unsigned char>(stream_text[name_begin - 1U]))) {
                    --name_begin;
                }
                std::string xname = stream_text.substr(name_begin, name_end - name_begin);
                if (!xname.empty() && xname[0] == '/') {
                    xname.erase(xname.begin());
                }
                auto it_x = xobj_refs.find(xname);
                if (it_x != xobj_refs.end()) {
                    auto it_obj = object_map->find(it_x->second);
                    if (it_obj != object_map->end() && HasPdfNameEntry(it_obj->second, "Subtype", "Form")) {
                        const auto child_refs = ResolveXObjectRefsForObjectBody(it_obj->second, *object_map);
                        const std::string child_stream = ExtractDecodedPdfObjectStream(it_obj->second);
                        if (!child_stream.empty()) {
                            const auto child_ctm = mat_mul(current_ctm, ParsePdfObjectMatrix(it_obj->second));
                            ParsePdfTextUses(
                                child_stream, page_h_pt, page_font_maps, child_refs, object_map, out_texts, child_ctm, depth + 1);
                        }
                    }
                }
                i = op_idx;
                continue;
            }
            if (op == "Td" || op == "TD") {
                size_t cursor = i;
                while (cursor > 0U && std::isspace(static_cast<unsigned char>(stream_text[cursor - 1U]))) {
                    --cursor;
                }
                size_t start = cursor;
                while (start > 0U && !std::isalpha(static_cast<unsigned char>(stream_text[start - 1U])) &&
                       stream_text[start - 1U] != '\n' && stream_text[start - 1U] != '\r') {
                    --start;
                }
                std::stringstream ns(stream_text.substr(start, cursor - start));
                double tx = 0.0;
                double ty = 0.0;
                if (ns >> tx >> ty) {
                    text_pos_x += tx;
                    text_pos_y += ty;
                    if (op == "TD") {
                        leading_pt = -ty;
                    }
                }
                i = op_idx;
                continue;
            }
            if (op == "T*") {
                text_pos_y -= leading_pt;
                i = op_idx;
                continue;
            }
            if (op == "TL") {
                size_t cursor = i;
                while (cursor > 0U && std::isspace(static_cast<unsigned char>(stream_text[cursor - 1U]))) {
                    --cursor;
                }
                size_t start = cursor;
                while (start > 0U && !std::isalpha(static_cast<unsigned char>(stream_text[start - 1U])) &&
                       stream_text[start - 1U] != '\n' && stream_text[start - 1U] != '\r') {
                    --start;
                }
                std::stringstream ns(stream_text.substr(start, cursor - start));
                ns >> leading_pt;
                i = op_idx;
                continue;
            }
            if (op == "rg" || op == "g") {
                size_t cursor = i;
                while (cursor > 0U && std::isspace(static_cast<unsigned char>(stream_text[cursor - 1U]))) {
                    --cursor;
                }
                size_t start = cursor;
                while (start > 0U && !std::isalpha(static_cast<unsigned char>(stream_text[start - 1U])) &&
                       stream_text[start - 1U] != '\n' && stream_text[start - 1U] != '\r') {
                    --start;
                }
                std::stringstream ns(stream_text.substr(start, cursor - start));
                if (op == "rg") {
                    double r = 0.0;
                    double g = 0.0;
                    double b = 0.0;
                    if (ns >> r >> g >> b) {
                        fill_r = std::max(0, std::min(255, static_cast<int>(r * 255.0 + 0.5)));
                        fill_g = std::max(0, std::min(255, static_cast<int>(g * 255.0 + 0.5)));
                        fill_b = std::max(0, std::min(255, static_cast<int>(b * 255.0 + 0.5)));
                        has_fill_color = true;
                    }
                } else {
                    double g = 0.0;
                    if (ns >> g) {
                        const int v = std::max(0, std::min(255, static_cast<int>(g * 255.0 + 0.5)));
                        fill_r = v;
                        fill_g = v;
                        fill_b = v;
                        has_fill_color = true;
                    }
                }
                i = op_idx;
                continue;
            }
            if (op == "BT") {
                text_matrix = {1, 0, 0, 1, 0, 0};
                text_pos_x = 0.0;
                text_pos_y = 0.0;
                i = op_idx;
                continue;
            }
            if (op == "ET") {
                i = op_idx;
                continue;
            }
        }
        ++i;
    }
}

static double ApproxTextWidthMm(const std::string& text, double size_mm) {
    auto glyphs = SplitUtf8Codepoints(text);
    const size_t n = glyphs.empty() ? text.size() : glyphs.size();
    return std::max(1.0, size_mm * std::max<size_t>(1U, n) * 0.55);
}

static void CompactPdfTextUses(std::vector<PdfToOfdTextUse>* texts) {
    if (texts == nullptr || texts->empty()) {
        return;
    }
    std::vector<PdfToOfdTextUse> out;
    out.reserve(texts->size());
    PdfToOfdTextUse cur = (*texts)[0];
    auto same_style = [](const PdfToOfdTextUse& a, const PdfToOfdTextUse& b) {
        if (std::abs(a.size_mm - b.size_mm) > 0.4) {
            return false;
        }
        if (a.has_fill_color != b.has_fill_color) {
            return false;
        }
        if (!a.has_fill_color) {
            return true;
        }
        return a.fill_r == b.fill_r && a.fill_g == b.fill_g && a.fill_b == b.fill_b;
    };
    for (size_t i = 1; i < texts->size(); ++i) {
        const auto& nxt = (*texts)[i];
        const bool same_line = std::abs(cur.y_mm - nxt.y_mm) <= 1.2;
        const double cur_end_x = cur.x_mm + ApproxTextWidthMm(cur.text, cur.size_mm);
        const double gap = nxt.x_mm - cur_end_x;
        const uint32_t prev_cp = LastUtf8CodePoint(cur.text);
        const uint32_t next_cp = FirstUtf8CodePoint(nxt.text);
        const bool prev_cjk = IsCjkCodePoint(prev_cp);
        const bool next_cjk = IsCjkCodePoint(next_cp);
        auto is_num_sym = [](uint32_t cp) {
            return (cp >= '0' && cp <= '9') || cp == '.' || cp == ',' || cp == '-' || cp == ':' || cp == '/';
        };
        const bool continuation_numeric = is_num_sym(prev_cp) && is_num_sym(next_cp);
        const bool continuation_cjk = prev_cjk && next_cjk;
        if (same_style(cur, nxt) && gap > -0.8) {
            const bool keep_tight = continuation_cjk || continuation_numeric;
            const size_t next_glyphs = SplitUtf8Codepoints(nxt.text).size();
            const bool should_merge_line =
                same_line && (gap < 10.0 || continuation_numeric || (continuation_cjk && (gap < 35.0 || next_glyphs <= 2U)));
            const bool should_merge_numeric_bridge =
                continuation_numeric && std::abs(cur.y_mm - nxt.y_mm) <= 4.0 && gap < 50.0;
            if (!(should_merge_line || should_merge_numeric_bridge)) {
                out.push_back(std::move(cur));
                cur = nxt;
                continue;
            }
            if (!keep_tight && gap > cur.size_mm * 0.6) {
                cur.text.push_back(' ');
            }
            cur.text += nxt.text;
            continue;
        }
        out.push_back(std::move(cur));
        cur = nxt;
    }
    out.push_back(std::move(cur));
    *texts = std::move(out);
}

static bool ShouldUseGeometryTextLayer(const std::vector<PdfToOfdTextUse>& texts) {
    if (texts.empty()) {
        return false;
    }
    size_t total_glyphs = 0;
    size_t short_tokens = 0;
    size_t tiny_numeric_tokens = 0;
    for (const auto& t : texts) {
        const auto glyphs = SplitUtf8Codepoints(t.text);
        const size_t n = glyphs.empty() ? t.text.size() : glyphs.size();
        if (n == 0U) {
            continue;
        }
        total_glyphs += n;
        if (n <= 2U) {
            ++short_tokens;
            bool numeric_like = true;
            for (char c : t.text) {
                if (!std::isdigit(static_cast<unsigned char>(c)) && c != '.' && c != ',' && c != ':' && c != '-' && c != '/') {
                    numeric_like = false;
                    break;
                }
            }
            if (numeric_like) {
                ++tiny_numeric_tokens;
            }
        }
    }
    if (total_glyphs < 4U) {
        return false;
    }
    const double short_ratio = static_cast<double>(short_tokens) / static_cast<double>(texts.size());
    const double tiny_num_ratio = static_cast<double>(tiny_numeric_tokens) / static_cast<double>(texts.size());
    if (texts.size() > 40U && short_ratio > 0.55 && tiny_num_ratio > 0.25) {
        return false;
    }
    return true;
}

static void ExtractPageGraphicsFromPdf(
    const std::string& pdf_content, std::vector<PdfToOfdPageGraphics>* out_pages, std::vector<PdfToOfdImageAsset>* out_assets) {
    if (out_pages == nullptr || out_assets == nullptr) {
        return;
    }
    out_pages->clear();
    out_assets->clear();

    struct PdfObj {
        int id = 0;
        std::string body;
    };
    std::vector<PdfObj> objects;
    std::unordered_map<int, std::string> object_map;
    const std::regex header_re(R"((\d+)\s+(\d+)\s+obj)");
    std::smatch m;
    std::string::const_iterator search_begin = pdf_content.cbegin();
    while (std::regex_search(search_begin, pdf_content.cend(), m, header_re)) {
        const int obj_id = std::atoi(m[1].str().c_str());
        const size_t header_offset = static_cast<size_t>(m.position(0) + (search_begin - pdf_content.cbegin()));
        const size_t body_begin = header_offset + static_cast<size_t>(m.length(0));
        const size_t body_end = pdf_content.find("endobj", body_begin);
        if (body_end == std::string::npos || body_end <= body_begin) {
            break;
        }
        const std::string obj_body = pdf_content.substr(body_begin, body_end - body_begin);
        objects.push_back(PdfObj{obj_id, obj_body});
        object_map[obj_id] = obj_body;
        search_begin = pdf_content.cbegin() + static_cast<std::ptrdiff_t>(body_end + 6U);
    }

    auto extract_stream = [](const std::string& obj_body) -> std::string {
        const size_t stream_pos = obj_body.find("stream");
        if (stream_pos == std::string::npos) {
            return "";
        }
        size_t data_begin = stream_pos + 6U;
        if (data_begin < obj_body.size() && obj_body[data_begin] == '\r') {
            ++data_begin;
        }
        if (data_begin < obj_body.size() && obj_body[data_begin] == '\n') {
            ++data_begin;
        }
        const size_t endstream_pos = obj_body.find("endstream", data_begin);
        if (endstream_pos == std::string::npos || endstream_pos <= data_begin) {
            return "";
        }
        std::string raw = obj_body.substr(data_begin, endstream_pos - data_begin);
        if (obj_body.find("/FlateDecode") != std::string::npos) {
            raw = TryInflateZlibStream(raw);
        }
        return raw;
    };

    auto parse_objstm_int = [](const std::string& body, const std::string& key, int* out) -> bool {
        if (out == nullptr) {
            return false;
        }
        std::smatch m;
        const std::regex re("/" + key + R"(\s+(\d+))");
        if (!std::regex_search(body, m, re)) {
            return false;
        }
        *out = std::atoi(m[1].str().c_str());
        return *out > 0;
    };
    {
        std::vector<PdfObj> embedded;
        for (const auto& obj : objects) {
            if (!HasPdfNameEntry(obj.body, "Type", "ObjStm")) {
                continue;
            }
            int n = 0;
            int first = 0;
            if (!parse_objstm_int(obj.body, "N", &n) || !parse_objstm_int(obj.body, "First", &first)) {
                continue;
            }
            const std::string stream = extract_stream(obj.body);
            if (stream.empty() || first <= 0 || static_cast<size_t>(first) >= stream.size()) {
                continue;
            }
            const std::string header = stream.substr(0, static_cast<size_t>(first));
            std::stringstream hs(header);
            std::vector<int> nums;
            int v = 0;
            while (hs >> v) {
                nums.push_back(v);
            }
            if (nums.size() < static_cast<size_t>(n * 2)) {
                continue;
            }
            for (int i = 0; i < n; ++i) {
                const int obj_id = nums[static_cast<size_t>(i * 2)];
                const int off = nums[static_cast<size_t>(i * 2 + 1)];
                if (off < 0) {
                    continue;
                }
                const size_t body_start = static_cast<size_t>(first + off);
                const size_t body_end = (i + 1 < n)
                                            ? static_cast<size_t>(first + nums[static_cast<size_t>((i + 1) * 2 + 1)])
                                            : stream.size();
                if (body_start >= stream.size() || body_end <= body_start || body_end > stream.size()) {
                    continue;
                }
                std::string emb = TrimAscii(stream.substr(body_start, body_end - body_start));
                if (emb.empty()) {
                    continue;
                }
                object_map[obj_id] = emb;
                embedded.push_back(PdfObj{obj_id, std::move(emb)});
            }
        }
        objects.insert(objects.end(), embedded.begin(), embedded.end());
    }

    auto parse_content_refs = [](const std::string& page_obj_body) -> std::vector<int> {
        std::vector<int> refs;
        const std::regex array_re(R"(/Contents\s*\[([\s\S]*?)\])");
        std::smatch array_match;
        if (std::regex_search(page_obj_body, array_match, array_re)) {
            const std::string refs_body = array_match[1].str();
            const std::regex ref_re(R"((\d+)\s+\d+\s+R)");
            auto begin = std::sregex_iterator(refs_body.begin(), refs_body.end(), ref_re);
            auto end = std::sregex_iterator();
            for (auto it = begin; it != end; ++it) {
                refs.push_back(std::atoi((*it)[1].str().c_str()));
            }
            return refs;
        }
        const std::regex single_re(R"(/Contents\s+(\d+)\s+\d+\s+R)");
        std::smatch single_match;
        if (std::regex_search(page_obj_body, single_match, single_re)) {
            refs.push_back(std::atoi(single_match[1].str().c_str()));
        }
        return refs;
    };

    auto parse_annot_refs = [](const std::string& page_obj_body) -> std::vector<int> {
        std::vector<int> refs;
        const std::regex array_re(R"(/Annots\s*\[([\s\S]*?)\])");
        std::smatch array_match;
        if (!std::regex_search(page_obj_body, array_match, array_re)) {
            return refs;
        }
        const std::regex ref_re(R"((\d+)\s+\d+\s+R)");
        const std::string body = array_match[1].str();
        auto begin = std::sregex_iterator(body.begin(), body.end(), ref_re);
        auto end = std::sregex_iterator();
        for (auto it = begin; it != end; ++it) {
            refs.push_back(std::atoi((*it)[1].str().c_str()));
        }
        return refs;
    };

    int next_res_id = 1000;
    std::unordered_map<int, int> res_id_by_pdf_obj;
    std::unordered_map<int, std::string> file_by_pdf_obj;
    for (const auto& obj : objects) {
        if (!HasPdfNameEntry(obj.body, "Subtype", "Image")) {
            continue;
        }
        std::string blob = extract_stream(obj.body);
        if (blob.empty()) {
            continue;
        }
        std::vector<unsigned char> data(blob.begin(), blob.end());
        std::string format;
        if (obj.body.find("/DCTDecode") != std::string::npos) {
            format = "JPEG";
        } else if (data.size() >= 8U && data[0] == 0x89U && data[1] == 'P' && data[2] == 'N' && data[3] == 'G') {
            format = "PNG";
        } else if (data.size() >= 2U && data[0] == 0xFFU && data[1] == 0xD8U) {
            format = "JPEG";
        } else {
            int iw = 0;
            int ih = 0;
            std::vector<unsigned char> rgb;
            if (!DecodePdfRawImageToRgb(obj.body, data, &iw, &ih, &rgb)) {
                continue;
            }
            std::vector<unsigned char> converted;
            if (EncodeJpegViaPythonPillow(iw, ih, rgb, &converted)) {
                data = std::move(converted);
                format = "JPEG";
            } else if (ConvertPdfRawImageToBmp(obj.body, data, &converted)) {
                data = std::move(converted);
                format = "BMP";
            } else {
                continue;
            }
        }

        std::string ext = Lower(format);
        if (ext == "jpeg") {
            ext = "jpg";
        }
        const int res_id = next_res_id++;
        const std::string file_name = "Image_" + std::to_string(res_id) + "." + ext;
        out_assets->push_back(PdfToOfdImageAsset{obj.id, res_id, file_name, format, data});
        res_id_by_pdf_obj[obj.id] = res_id;
        file_by_pdf_obj[obj.id] = file_name;

#ifdef LIBOFD_HAVE_ZLIB
        // For WPS compatibility, keep a dual-format fallback for raw decoded images.
        if (format == "BMP") {
            std::vector<unsigned char> png;
            if (ConvertPdfRawImageToPng(obj.body, std::vector<unsigned char>(blob.begin(), blob.end()), &png)) {
                const int alt_res_id = next_res_id++;
                const std::string alt_file_name = "Image_" + std::to_string(alt_res_id) + ".png";
                out_assets->push_back(PdfToOfdImageAsset{obj.id, alt_res_id, alt_file_name, "PNG", std::move(png)});
            }
        }
#endif
    }

    std::unordered_map<int, size_t> last_obj_index;
    for (size_t i = 0; i < objects.size(); ++i) {
        last_obj_index[objects[i].id] = i;
    }
    auto parse_kids_refs = [](const std::string& pages_obj_body) -> std::vector<int> {
        std::vector<int> out;
        std::smatch m;
        const std::regex kids_re(R"(/Kids\s*\[([\s\S]*?)\])");
        if (!std::regex_search(pages_obj_body, m, kids_re)) {
            return out;
        }
        const std::regex ref_re(R"((\d+)\s+\d+\s+R)");
        const std::string body = m[1].str();
        auto begin = std::sregex_iterator(body.begin(), body.end(), ref_re);
        auto end = std::sregex_iterator();
        for (auto it = begin; it != end; ++it) {
            out.push_back(std::atoi((*it)[1].str().c_str()));
        }
        return out;
    };
    std::vector<size_t> page_indices;
    std::unordered_set<int> page_ids_added;
    for (size_t i = 0; i < objects.size(); ++i) {
        auto li = last_obj_index.find(objects[i].id);
        if (li != last_obj_index.end() && li->second != i) {
            continue;
        }
        if (!HasPdfNameEntry(objects[i].body, "Type", "Pages")) {
            continue;
        }
        const auto kids = parse_kids_refs(objects[i].body);
        for (int pid : kids) {
            auto pi = last_obj_index.find(pid);
            if (pi == last_obj_index.end()) {
                continue;
            }
            const auto& pobj = objects[pi->second];
            if (!HasPdfNameEntry(pobj.body, "Type", "Page") || HasPdfNameEntry(pobj.body, "Type", "Pages")) {
                continue;
            }
            if (page_ids_added.insert(pid).second) {
                page_indices.push_back(pi->second);
            }
        }
    }
    for (size_t i = 0; i < objects.size(); ++i) {
        auto li = last_obj_index.find(objects[i].id);
        if (li != last_obj_index.end() && li->second != i) {
            continue;
        }
        if (!HasPdfNameEntry(objects[i].body, "Type", "Page") || HasPdfNameEntry(objects[i].body, "Type", "Pages")) {
            continue;
        }
        if (page_ids_added.insert(objects[i].id).second) {
            page_indices.push_back(i);
        }
    }

    for (size_t obj_idx : page_indices) {
        const auto& obj = objects[obj_idx];
        PdfToOfdPageGraphics page;
        ParsePageMediaBoxPt(obj.body, &page.width_pt, &page.height_pt);
        const PdfPageFontMaps page_font_maps = ParsePageFontMaps(obj.body, object_map, extract_stream);

        const std::string resource_body = FindPageResourceBody(obj.body, object_map);
        auto xobj_refs = ParseXObjectRefsFromResource(resource_body);
        if (xobj_refs.empty()) {
            const size_t xo_key = obj.body.find("/XObject");
            if (xo_key != std::string::npos) {
                size_t p = xo_key + std::string("/XObject").size();
                while (p < obj.body.size() && std::isspace(static_cast<unsigned char>(obj.body[p]))) {
                    ++p;
                }
                std::string xo_block;
                if (p + 1U < obj.body.size() && obj.body[p] == '<' && obj.body[p + 1U] == '<' &&
                    ExtractBalancedDictBody(obj.body, p, &xo_block, nullptr)) {
                    const std::regex ref_re(R"(/([A-Za-z0-9_]+)\s+(\d+)\s+\d+\s+R)");
                    auto begin = std::sregex_iterator(xo_block.begin(), xo_block.end(), ref_re);
                    auto end = std::sregex_iterator();
                    for (auto it = begin; it != end; ++it) {
                        xobj_refs[(*it)[1].str()] = std::atoi((*it)[2].str().c_str());
                    }
                }
            }
        }
        const auto refs = parse_content_refs(obj.body);
        if (refs.empty()) {
            const std::string stream_text = extract_stream(obj.body);
            if (!stream_text.empty()) {
                ParsePdfPathAndImageUses(stream_text, page.height_pt, xobj_refs, &object_map, &page.images, &page.paths);
                ParsePdfTextUses(stream_text, page.height_pt, page_font_maps, xobj_refs, &object_map, &page.texts);
            }
        } else {
            for (int ref : refs) {
                auto it = object_map.find(ref);
                if (it == object_map.end()) {
                    continue;
                }
                const std::string stream_text = extract_stream(it->second);
                if (stream_text.empty()) {
                    continue;
                }
                ParsePdfPathAndImageUses(stream_text, page.height_pt, xobj_refs, &object_map, &page.images, &page.paths);
                ParsePdfTextUses(stream_text, page.height_pt, page_font_maps, xobj_refs, &object_map, &page.texts);
            }
        }
        // Parse annotation appearance streams (e.g. signature/stamp widgets).
        const auto annot_refs = parse_annot_refs(obj.body);
        for (int annot_ref : annot_refs) {
            auto annot_it = object_map.find(annot_ref);
            if (annot_it == object_map.end()) {
                continue;
            }
            const std::string& annot_body = annot_it->second;
            std::smatch ap_match;
            const std::regex ap_n_re(R"(/AP\s*<<[\s\S]*?/N\s+(\d+)\s+\d+\s+R)");
            if (!std::regex_search(annot_body, ap_match, ap_n_re)) {
                continue;
            }
            const int ap_obj_id = std::atoi(ap_match[1].str().c_str());
            auto ap_it = object_map.find(ap_obj_id);
            if (ap_it == object_map.end()) {
                continue;
            }
            std::array<double, 4> rect = {0.0, 0.0, 0.0, 0.0};
            if (!ParsePdfRect4(annot_body, "Rect", &rect)) {
                continue;
            }
            std::array<double, 4> bbox = {0.0, 0.0, 1.0, 1.0};
            ParsePdfRect4(ap_it->second, "BBox", &bbox);
            const double bw = std::max(1e-6, bbox[2] - bbox[0]);
            const double bh = std::max(1e-6, bbox[3] - bbox[1]);
            const double rw = std::max(1e-6, rect[2] - rect[0]);
            const double rh = std::max(1e-6, rect[3] - rect[1]);
            const double sx = rw / bw;
            const double sy = rh / bh;
            PdfParseGraphicState ap_state;
            ap_state.ctm = {sx, 0.0, 0.0, sy, rect[0] - bbox[0] * sx, rect[1] - bbox[1] * sy};
            const auto ap_xobj_refs = ResolveXObjectRefsForObjectBody(ap_it->second, object_map);
            const std::string ap_stream = extract_stream(ap_it->second);
            if (ap_stream.empty()) {
                continue;
            }
            const size_t image_count_before = page.images.size();
            ParsePdfPathAndImageUses(
                ap_stream, page.height_pt, ap_xobj_refs, &object_map, &page.images, &page.paths, &ap_state);
            for (size_t ii = image_count_before; ii < page.images.size(); ++ii) {
                page.images[ii].is_stamp_annotation = true;
            }
            ParsePdfTextUses(ap_stream, page.height_pt, page_font_maps, ap_xobj_refs, &object_map, &page.texts, ap_state.ctm);
            if (page.images.size() == image_count_before) {
                // Fallback for readers sensitive to complex AP transform stacks:
                // place AP's first image resource directly to annotation rect.
                for (const auto& xr : ap_xobj_refs) {
                    auto xo_it = object_map.find(xr.second);
                    if (xo_it == object_map.end()) {
                        continue;
                    }
                    if (!HasPdfNameEntry(xo_it->second, "Subtype", "Image")) {
                        continue;
                    }
                    PdfToOfdImageUse u;
                    u.object_id = xr.second;
                    u.x_mm = rect[0] / kMmToPt;
                    u.y_mm = std::max(0.0, (page.height_pt - rect[3]) / kMmToPt);
                    u.w_mm = std::max(1.0 / kMmToPt, (rect[2] - rect[0]) / kMmToPt);
                    u.h_mm = std::max(1.0 / kMmToPt, (rect[3] - rect[1]) / kMmToPt);
                    u.is_stamp_annotation = true;
                    page.images.push_back(std::move(u));
                    break;
                }
            }
        }

        // Remove unresolved image refs (unsupported image encodings are skipped by design).
        page.images.erase(
            std::remove_if(
                page.images.begin(), page.images.end(),
                [&](const PdfToOfdImageUse& use) { return res_id_by_pdf_obj.find(use.object_id) == res_id_by_pdf_obj.end(); }),
            page.images.end());
        CompactPdfTextUses(&page.texts);

        out_pages->push_back(std::move(page));
    }
}

static void ReplaceOrInsertTagContent(std::string* xml, const std::string& tag_name, const std::string& value) {
    if (xml == nullptr) {
        return;
    }
    const std::regex re("<(?:ofd:)?" + tag_name + R"(>\s*([^<]*)\s*</(?:ofd:)?)" + tag_name + ">");
    std::smatch m;
    if (std::regex_search(*xml, m, re)) {
        const std::string full = m[0].str();
        const std::string repl = "<ofd:" + tag_name + ">" + value + "</ofd:" + tag_name + ">";
        xml->replace(m.position(0), full.size(), repl);
    }
}

static void EnrichOfdPackageWithPdfGraphics(
    const fs::path& output_ofd_root, const std::vector<PdfToOfdPageGraphics>& graphics,
    const std::vector<PdfToOfdImageAsset>& assets, const std::vector<PdfEmbeddedFontAsset>& embedded_fonts,
    bool force_visual_raster) {
    if (graphics.empty()) {
        return;
    }
    const fs::path doc_xml_path = output_ofd_root / "Doc_0" / "Document.xml";
    std::string doc_xml;
    if (ReadTextFile(doc_xml_path.string(), &doc_xml) != LIBOFD_OK) {
        return;
    }

    const double page_w_mm = force_visual_raster ? 210.0 : std::max(1.0, graphics.front().width_pt / kMmToPt);
    const double page_h_mm = force_visual_raster ? 297.0 : std::max(1.0, graphics.front().height_pt / kMmToPt);
    {
        std::ostringstream pb;
        pb << "0 0 " << page_w_mm << " " << page_h_mm;
        ReplaceOrInsertTagContent(&doc_xml, "PhysicalBox", pb.str());
        ReplaceOrInsertTagContent(&doc_xml, "ApplicationBox", pb.str());
    }

    int max_unit_id = 1;
    {
        std::smatch m;
        const std::regex max_re(R"(<(?:ofd:)?MaxUnitID>\s*(\d+)\s*</(?:ofd:)?MaxUnitID>)");
        if (std::regex_search(doc_xml, m, max_re)) {
            max_unit_id = std::max(1, std::atoi(m[1].str().c_str()));
        }
    }

    std::unordered_map<int, std::vector<int>> res_ids_by_obj;
    for (const auto& a : assets) {
        res_ids_by_obj[a.object_id].push_back(a.resource_id);
        max_unit_id = std::max(max_unit_id, a.resource_id);
    }

    if (!embedded_fonts.empty() && false) {
        const fs::path res_dir = output_ofd_root / "Doc_0" / "Res";
        std::error_code ec;
        fs::create_directories(res_dir, ec);
        for (const auto& f : embedded_fonts) {
            std::ofstream font_out(res_dir / f.file_name, std::ios::binary);
            if (font_out.is_open()) {
                font_out.write(
                    reinterpret_cast<const char*>(f.data.data()),
                    static_cast<std::streamsize>(f.data.size()));
            }
        }
        const fs::path public_res_path = output_ofd_root / "Doc_0" / "PublicRes.xml";
        std::string public_res_xml;
        if (ReadTextFile(public_res_path.string(), &public_res_xml) == LIBOFD_OK) {
            if (public_res_xml.find("BaseLoc=\"Res\"") == std::string::npos) {
                const size_t tag_pos = public_res_xml.find("<ofd:Res");
                const size_t tag_end = public_res_xml.find('>', tag_pos);
                if (tag_end != std::string::npos) {
                    public_res_xml.insert(tag_end, " BaseLoc=\"Res\"");
                }
            }
            std::ostringstream fonts_block;
            fonts_block << "<ofd:Fonts>\n";
            const auto& f0 = embedded_fonts.front();
            const std::string safe_file_name = EscapeXmlText(f0.file_name);
            fonts_block << "    <ofd:Font ID=\"1\" FontName=\"宋体\" FamilyName=\"宋体\">\n";
            fonts_block << "      <ofd:FontFile>" << safe_file_name << "</ofd:FontFile>\n";
            fonts_block << "    </ofd:Font>\n";
            fonts_block << "  </ofd:Fonts>";
            const std::regex fonts_re(R"(<(?:ofd:)?Fonts>[\s\S]*?</(?:ofd:)?Fonts>)");
            if (std::regex_search(public_res_xml, fonts_re)) {
                public_res_xml = std::regex_replace(public_res_xml, fonts_re, fonts_block.str());
            } else {
                const size_t pos = public_res_xml.rfind("</ofd:Res>");
                if (pos != std::string::npos) {
                    public_res_xml.insert(pos, "  " + fonts_block.str() + "\n");
                }
            }
            std::ofstream pr_out(public_res_path, std::ios::binary);
            if (pr_out.is_open()) {
                pr_out << public_res_xml;
            }
        }
    }
    {
        const fs::path public_res_path = output_ofd_root / "Doc_0" / "PublicRes.xml";
        std::string public_res_xml;
        if (ReadTextFile(public_res_path.string(), &public_res_xml) == LIBOFD_OK &&
            public_res_xml.find("BaseLoc=\"Res\"") == std::string::npos) {
            const size_t tag_pos = public_res_xml.find("<ofd:Res");
            const size_t tag_end = public_res_xml.find('>', tag_pos);
            if (tag_end != std::string::npos) {
                public_res_xml.insert(tag_end, " BaseLoc=\"Res\"");
                std::ofstream pr_out(public_res_path, std::ios::binary);
                if (pr_out.is_open()) {
                    pr_out << public_res_xml;
                }
            }
        }
    }

    // Write image resources and DocumentRes.xml if needed.
    if (!assets.empty()) {
        const fs::path res_dir = output_ofd_root / "Doc_0" / "Res";
        std::error_code ec;
        fs::create_directories(res_dir, ec);
        for (const auto& a : assets) {
            std::ofstream img_out(res_dir / a.file_name, std::ios::binary);
            if (!img_out.is_open()) {
                continue;
            }
            img_out.write(reinterpret_cast<const char*>(a.data.data()), static_cast<std::streamsize>(a.data.size()));
        }
        std::ostringstream res_xml;
        res_xml << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
        res_xml << "<ofd:Res xmlns:ofd=\"http://www.ofdspec.org/2016\" BaseLoc=\"Res\">\n";
        res_xml << "  <ofd:MultiMedias>\n";
        for (const auto& a : assets) {
            res_xml << "    <ofd:MultiMedia ID=\"" << a.resource_id << "\" Type=\"Image\" Format=\"" << a.format << "\">\n";
            res_xml << "      <ofd:MediaFile>" << EscapeXmlText(a.file_name) << "</ofd:MediaFile>\n";
            res_xml << "    </ofd:MultiMedia>\n";
        }
        res_xml << "  </ofd:MultiMedias>\n";
        res_xml << "  <ofd:DrawParams>\n";
        res_xml << "    <ofd:DrawParam ID=\"7\">\n";
        res_xml << "      <ofd:FillColor Value=\"0 0 0\"/>\n";
        res_xml << "      <ofd:StrokeColor Value=\"0 0 0\"/>\n";
        res_xml << "    </ofd:DrawParam>\n";
        res_xml << "  </ofd:DrawParams>\n";
        res_xml << "</ofd:Res>\n";
        std::ofstream out(output_ofd_root / "Doc_0" / "DocumentRes.xml", std::ios::binary);
        if (out.is_open()) {
            out << res_xml.str();
        }
        if (doc_xml.find("<ofd:DocumentRes>") == std::string::npos) {
            const std::string marker = "<ofd:PublicRes>";
            const size_t pos = doc_xml.find(marker);
            if (pos != std::string::npos) {
                doc_xml.insert(pos, "    <ofd:DocumentRes>DocumentRes.xml</ofd:DocumentRes>\n");
            }
        }
    }

    auto page_locs = ParsePageBaseLocs(doc_xml);
    const fs::path doc_dir = (output_ofd_root / "Doc_0");
    const size_t count = std::min(page_locs.size(), graphics.size());
    for (size_t i = 0; i < count; ++i) {
        const fs::path page_path = ResolveOfdPath(output_ofd_root, doc_dir, page_locs[i]);
        std::string page_xml;
        if (ReadTextFile(page_path.string(), &page_xml) != LIBOFD_OK) {
            continue;
        }
        bool has_page_raster = false;
        for (const auto& img : graphics[i].images) {
            if (img.is_page_raster) {
                has_page_raster = true;
                break;
            }
        }
        if (force_visual_raster && has_page_raster) {
            const std::regex text_obj_re(R"(\s*<(?:ofd:)?TextObject\b[\s\S]*?</(?:ofd:)?TextObject>\s*)");
            page_xml = std::regex_replace(page_xml, text_obj_re, "\n");
        }
        const bool use_geometry_texts = (!has_page_raster) && ShouldUseGeometryTextLayer(graphics[i].texts);
        if (use_geometry_texts) {
            // Replace coarse fallback text layer with geometry-preserving text layer.
            const std::regex text_obj_re(R"(\s*<(?:ofd:)?TextObject\b[\s\S]*?</(?:ofd:)?TextObject>\s*)");
            page_xml = std::regex_replace(page_xml, text_obj_re, "\n");
        }
        const size_t layer_end = page_xml.rfind("</ofd:Layer>");
        if (layer_end == std::string::npos) {
            continue;
        }
        std::ostringstream inject;
        if (use_geometry_texts) {
            auto emit_text_object = [&](const PdfToOfdTextUse& txt, int font_id) {
                const int obj_id = ++max_unit_id;
                const auto glyphs = SplitUtf8Codepoints(txt.text);
                const double w_mm = std::max(5.0, txt.size_mm * static_cast<double>(glyphs.size()) * 0.7);
                const double h_mm = std::max(2.0, txt.size_mm * 1.2);
                inject << "      <ofd:TextObject ID=\"" << obj_id << "\" Font=\"" << font_id << "\" Size=\"" << txt.size_mm
                       << "\" Boundary=\"" << txt.x_mm << " " << txt.y_mm << " " << w_mm << " " << h_mm << "\">\n";
                if (txt.has_fill_color) {
                    inject << "        <ofd:FillColor Value=\"" << txt.fill_r << " " << txt.fill_g << " " << txt.fill_b
                           << "\"/>\n";
                }
                inject << "        <ofd:TextCode X=\"0\" Y=\"" << txt.size_mm << "\"";
                bool write_delta_x = false;
                if (!txt.delta_x_mm.empty()) {
                    // WPS 对密集 DeltaX 文本兼容性较敏感，仅在非均匀字距时输出。
                    const double first = txt.delta_x_mm.front();
                    for (double dx : txt.delta_x_mm) {
                        if (std::abs(dx - first) > 0.15) {
                            write_delta_x = true;
                            break;
                        }
                    }
                }
                if (write_delta_x) {
                    inject << " DeltaX=\"";
                    for (size_t di = 0; di < txt.delta_x_mm.size(); ++di) {
                        if (di > 0U) {
                            inject << " ";
                        }
                        inject << txt.delta_x_mm[di];
                    }
                    inject << "\"";
                }
                inject << ">" << EscapeXmlText(txt.text) << "</ofd:TextCode>\n";
                inject << "      </ofd:TextObject>\n";
            };
            for (const auto& txt : graphics[i].texts) {
                if (txt.text.empty()) {
                    continue;
                }
                // Use a standard face ID to reduce WPS tofu risk.
                emit_text_object(txt, 3);
            }
        }
        for (const auto& img : graphics[i].images) {
            if (force_visual_raster && has_page_raster && !img.is_page_raster) {
                continue;
            }
            auto it_res = res_ids_by_obj.find(img.object_id);
            if (it_res == res_ids_by_obj.end() || it_res->second.empty()) {
                continue;
            }
            for (int rid : it_res->second) {
                const int block_id = ++max_unit_id;
                inject << "      <ofd:PageBlock ID=\"" << block_id << "\">\n";
                const int obj_id = ++max_unit_id;
                if (img.is_page_raster) {
                    inject << "      <ofd:ImageObject ID=\"" << obj_id << "\" ResourceID=\"" << rid << "\" DrawParam=\"7\" Boundary=\"0 0 "
                           << page_w_mm << " " << page_h_mm << "\" CTM=\"" << page_w_mm << " 0 0 " << page_h_mm << " 0 0\"/>\n";
                } else {
                    inject << "      <ofd:ImageObject ID=\"" << obj_id << "\" ResourceID=\"" << rid
                           << "\" DrawParam=\"7\" Boundary=\"0 0 1 1\" CTM=\"" << img.w_mm << " 0 0 " << img.h_mm << " " << img.x_mm
                           << " " << img.y_mm << "\"/>\n";
                }
                inject << "      </ofd:PageBlock>\n";
            }
            if (!force_visual_raster && img.is_stamp_annotation) {
                const int box_id = ++max_unit_id;
                inject << "      <ofd:PathObject ID=\"" << box_id << "\" Boundary=\"0 0 " << page_w_mm << " " << page_h_mm
                       << "\" LineWidth=\"0.35\" Stroke=\"true\" Fill=\"false\" AbbreviatedData=\"M " << img.x_mm << " " << img.y_mm
                       << " L " << (img.x_mm + img.w_mm) << " " << img.y_mm << " L " << (img.x_mm + img.w_mm) << " "
                       << (img.y_mm + img.h_mm) << " L " << img.x_mm << " " << (img.y_mm + img.h_mm) << " B\">\n";
                inject << "        <ofd:StrokeColor Value=\"196 0 0\"/>\n";
                inject << "      </ofd:PathObject>\n";
            }
        }
        for (const auto& path : graphics[i].paths) {
            if (force_visual_raster && has_page_raster) {
                continue;
            }
            if (path.abbreviated_data.empty()) {
                continue;
            }
            const int obj_id = ++max_unit_id;
            inject << "      <ofd:PathObject ID=\"" << obj_id << "\" Boundary=\"0 0 " << page_w_mm << " " << page_h_mm << "\"";
            inject << " LineWidth=\"" << path.line_width_mm << "\"";
            inject << " Stroke=\"" << (path.stroke ? "true" : "false") << "\"";
            inject << " Fill=\"" << (path.fill ? "true" : "false") << "\"";
            inject << " AbbreviatedData=\"" << EscapeXmlText(path.abbreviated_data) << "\">\n";
            if (path.has_stroke_color) {
                inject << "        <ofd:StrokeColor Value=\"" << path.stroke_r << " " << path.stroke_g << " " << path.stroke_b
                       << "\"/>\n";
            }
            if (path.has_fill_color) {
                inject << "        <ofd:FillColor Value=\"" << path.fill_r << " " << path.fill_g << " " << path.fill_b << "\"/>\n";
            }
            inject << "      </ofd:PathObject>\n";
        }
        page_xml.insert(layer_end, inject.str());
        std::ofstream out(page_path, std::ios::binary);
        if (out.is_open()) {
            out << page_xml;
        }
    }

    ReplaceOrInsertTagContent(&doc_xml, "MaxUnitID", std::to_string(max_unit_id));
    std::ofstream doc_out(doc_xml_path, std::ios::binary);
    if (doc_out.is_open()) {
        doc_out << doc_xml;
    }
}

static bool HasPdfSignatureWidget(const std::string& pdf_content) {
    if (pdf_content.find("/FT/Sig") != std::string::npos || pdf_content.find("/Subtype/Widget") != std::string::npos) {
        return true;
    }
    struct PdfObj {
        int id = 0;
        std::string body;
    };
    std::vector<PdfObj> objects;
    const std::regex header_re(R"((\d+)\s+(\d+)\s+obj)");
    std::smatch m;
    std::string::const_iterator search_begin = pdf_content.cbegin();
    while (std::regex_search(search_begin, pdf_content.cend(), m, header_re)) {
        const int obj_id = std::atoi(m[1].str().c_str());
        const size_t header_offset = static_cast<size_t>(m.position(0) + (search_begin - pdf_content.cbegin()));
        const size_t body_begin = header_offset + static_cast<size_t>(m.length(0));
        const size_t body_end = pdf_content.find("endobj", body_begin);
        if (body_end == std::string::npos || body_end <= body_begin) {
            break;
        }
        objects.push_back(PdfObj{obj_id, pdf_content.substr(body_begin, body_end - body_begin)});
        search_begin = pdf_content.cbegin() + static_cast<std::ptrdiff_t>(body_end + 6U);
    }
    for (const auto& obj : objects) {
        if (!HasPdfNameEntry(obj.body, "Type", "ObjStm")) {
            continue;
        }
        const std::string stream = ExtractDecodedPdfObjectStream(obj.body);
        if (stream.empty()) {
            continue;
        }
        if (stream.find("/FT/Sig") != std::string::npos || stream.find("/Subtype/Widget") != std::string::npos ||
            stream.find("/FT /Sig") != std::string::npos || stream.find("/Subtype /Widget") != std::string::npos) {
            return true;
        }
    }
    return false;
}

libofd_status_t ConvertOfdToPdf(
    const std::string& input_ofd_path, const std::string& output_pdf_path, const ConvertOptions* options) {
    if (input_ofd_path.empty() || output_pdf_path.empty()) {
        return LIBOFD_ERR_INVALID_ARGUMENT;
    }
    const fs::path root(input_ofd_path);
    if (!fs::exists(root) || !fs::is_directory(root)) {
        return LIBOFD_ERR_UNSUPPORTED;
    }
    std::string ofd_xml;
    libofd_status_t status = ReadTextFile((root / "OFD.xml").string(), &ofd_xml);
    if (status != LIBOFD_OK) {
        return status;
    }
    const std::regex doc_root_re(R"(<(?:ofd:)?DocRoot>\s*([^<]+)\s*</(?:ofd:)?DocRoot>)");
    std::smatch doc_root_match;
    if (!std::regex_search(ofd_xml, doc_root_match, doc_root_re)) {
        return LIBOFD_ERR_PARSE;
    }
    const fs::path doc_root_path = NormalizeOfdPath(doc_root_match[1].str());
    std::string document_xml;
    status = ReadTextFile((root / doc_root_path).string(), &document_xml);
    if (status != LIBOFD_OK) {
        return status;
    }
    const fs::path document_dir = (root / doc_root_path).parent_path();

    double page_w = 595.0;
    double page_h = 842.0;
    ParsePhysicalBoxToPt(document_xml, &page_w, &page_h);
    auto page_locs = ParsePageBaseLocs(document_xml);
    ResourceTable resources;
    LoadDocumentResources(root, document_dir, document_xml, &resources);

    // Fallback through higher-level document parser for text content.
    OfdDocument fallback_doc;
    fallback_doc.LoadFromExplodedPackage(input_ofd_path);

    std::vector<OfdPageLayout> pages;
    for (size_t i = 0; i < page_locs.size(); ++i) {
        OfdPageLayout layout;
        layout.width_pt = page_w;
        layout.height_pt = page_h;
        std::string page_xml;
        const fs::path page_path = ResolveOfdPath(root, document_dir, page_locs[i]);
        if (ReadTextFile(page_path.string(), &page_xml) == LIBOFD_OK) {
            ParseTextObjects(page_xml, &layout.text_objects);
            ParseImageObjects(page_xml, resources, &layout.image_objects);
            ParsePathObjects(page_xml, &layout.path_objects);
        }
        std::string fallback_text;
        if (fallback_doc.GetPageText(i, &fallback_text) == LIBOFD_OK) {
            layout.fallback_text = fallback_text;
        }
        pages.push_back(layout);
    }
    if (pages.empty()) {
        OfdPageLayout layout;
        layout.fallback_text = " ";
        pages.push_back(layout);
    }

    AppendSignatureStampImages(root, document_dir, ofd_xml, document_xml, &pages);

    status = WritePdfDocument(pages, resources, options, output_pdf_path);
    if (status != LIBOFD_OK) {
        return status;
    }
    PreserveSignsToPdfSidecar(document_dir, fs::path(output_pdf_path));
    return LIBOFD_OK;
}

libofd_status_t ConvertPdfToOfd(
    const std::string& input_pdf_path, const std::string& output_ofd_path, const PdfToOfdOptions* options) {
    if (input_pdf_path.empty() || output_ofd_path.empty()) {
        return LIBOFD_ERR_INVALID_ARGUMENT;
    }
    std::string pdf;
    libofd_status_t status = ReadTextFile(input_pdf_path, &pdf);
    if (status != LIBOFD_OK) {
        return status;
    }

    auto page_texts = ExtractPageTextsFromPdf(pdf, true);
    std::vector<PdfToOfdPageGraphics> page_graphics;
    std::vector<PdfToOfdImageAsset> image_assets;
    std::vector<PdfEmbeddedFontAsset> embedded_fonts;
    ExtractPageGraphicsFromPdf(pdf, &page_graphics, &image_assets);
    embedded_fonts.clear();
    PdfToOfdMode mode = PdfToOfdMode::kAuto;
    if (options != nullptr) {
        mode = options->mode;
    }
    const bool auto_raster = HasPdfSignatureWidget(pdf);
    const bool prefer_raster_fallback = (mode == PdfToOfdMode::kVisualRaster) || (mode == PdfToOfdMode::kAuto && auto_raster);
    const bool force_visual_raster = prefer_raster_fallback;
    std::vector<fs::path> raster_pages;
    if (prefer_raster_fallback && RenderPdfPagesToImages(fs::path(input_pdf_path), &raster_pages)) {
        AppendRasterFallbackAssets(raster_pages, &page_graphics, &image_assets);
    }

    const size_t page_count = std::max(page_texts.size(), page_graphics.size());
    if (page_count == 0U) {
        page_texts.emplace_back("");
    }

    OfdDocument doc;
    doc.CreateEmpty("pdf-imported", "libofd-pdf-engine");
    const size_t emit_pages = page_count == 0U ? page_texts.size() : page_count;
    for (size_t i = 0; i < emit_pages; ++i) {
        const std::string text = i < page_texts.size() ? page_texts[i] : "";
        doc.AddPageText(text);
    }
    status = doc.SaveToExplodedPackage(output_ofd_path);
    if (status != LIBOFD_OK) {
        return status;
    }
    EnrichOfdPackageWithPdfGraphics(fs::path(output_ofd_path), page_graphics, image_assets, embedded_fonts, force_visual_raster);
    RestoreSignsFromPdfSidecar(fs::path(input_pdf_path), fs::path(output_ofd_path));
    return LIBOFD_OK;
}

} // namespace libofd::pdf_engine

