#include "libofd/document.h"

#include <chrono>
#include <algorithm>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <regex>
#include <sstream>
#include <string>

#include "crypto_provider.h"
#include "libofd/package_reader.h"
#include "xml_utils.h"

namespace fs = std::filesystem;

namespace libofd {

static libofd_status_t ReadWholeFile(const fs::path& path, std::string* out_content) {
    if (out_content == nullptr) {
        return LIBOFD_ERR_INVALID_ARGUMENT;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        return LIBOFD_ERR_IO;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    *out_content = ss.str();
    return LIBOFD_OK;
}

static libofd_status_t ReadWholeBinaryFile(const fs::path& path, std::vector<unsigned char>* out_content) {
    if (out_content == nullptr) {
        return LIBOFD_ERR_INVALID_ARGUMENT;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        return LIBOFD_ERR_IO;
    }
    out_content->assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return LIBOFD_OK;
}

static libofd_status_t WriteTextFile(const fs::path& path, const std::string& content) {
    std::error_code ec;
    if (!path.parent_path().empty()) {
        fs::create_directories(path.parent_path(), ec);
    }
    std::ofstream out(path, std::ios::binary);
    if (!out.is_open()) {
        return LIBOFD_ERR_IO;
    }
    out << content;
    return LIBOFD_OK;
}

static libofd_status_t WriteBinaryFile(const fs::path& path, const std::vector<unsigned char>& content) {
    std::error_code ec;
    if (!path.parent_path().empty()) {
        fs::create_directories(path.parent_path(), ec);
    }
    std::ofstream out(path, std::ios::binary);
    if (!out.is_open()) {
        return LIBOFD_ERR_IO;
    }
    out.write(reinterpret_cast<const char*>(content.data()), static_cast<std::streamsize>(content.size()));
    return LIBOFD_OK;
}

static std::string EscapeXml(const std::string& input) {
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

static std::string BuildCanonicalPayload(const OfdDocumentInfo& info, const std::vector<OfdPageInfo>& pages) {
    std::ostringstream ss;
    ss << "doc_id=" << info.doc_id << "\n";
    ss << "creator=" << info.creator << "\n";
    ss << "creation_date=" << info.creation_date << "\n";
    ss << "max_unit_id=" << info.max_unit_id << "\n";
    ss << "page_count=" << pages.size() << "\n";
    for (size_t i = 0; i < pages.size(); ++i) {
        ss << "page[" << i << "].id=" << pages[i].page_id << "\n";
        ss << "page[" << i << "].base_loc=" << pages[i].base_loc << "\n";
        ss << "page[" << i << "].text=" << pages[i].text << "\n";
    }
    return ss.str();
}

static std::string NowDateIso() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    std::ostringstream ss;
    ss << std::put_time(&tm, "%Y-%m-%d");
    return ss.str();
}

static std::vector<std::string> SplitLinesPreserveEmpty(const std::string& text) {
    std::vector<std::string> out;
    std::stringstream ss(text);
    std::string line;
    while (std::getline(ss, line)) {
        out.push_back(line);
    }
    if (out.empty()) {
        out.push_back(text);
    }
    return out;
}

static fs::path NormalizeOfdPath(const std::string& raw_path) {
    std::string v = raw_path;
    while (!v.empty() && (v.front() == '/' || v.front() == '\\')) {
        v.erase(v.begin());
    }
    return fs::path(v).lexically_normal();
}

static fs::path ResolveOfdPath(
    const fs::path& package_root, const fs::path& document_dir, const std::string& maybe_relative_path) {
    const fs::path normalized = NormalizeOfdPath(maybe_relative_path);
    const fs::path doc_relative = document_dir / normalized;
    if (fs::exists(doc_relative)) {
        return doc_relative;
    }
    return package_root / normalized;
}

static void EnsurePageDefaults(std::vector<OfdPageInfo>* pages) {
    if (pages == nullptr) {
        return;
    }
    for (size_t i = 0; i < pages->size(); ++i) {
        OfdPageInfo& page = (*pages)[i];
        if (page.page_id.empty()) {
            page.page_id = std::to_string(i + 1U);
        }
        if (page.base_loc.empty()) {
            page.base_loc = "Pages/Page_" + std::to_string(i) + "/Content.xml";
        }
    }
}

static void ParsePagesFromDocumentXml(const std::string& document_xml, std::vector<OfdPageInfo>* out_pages) {
    if (out_pages == nullptr) {
        return;
    }
    out_pages->clear();

    // Handles both <Page .../> and <ofd:Page .../> forms.
    const std::regex page_tag_re(R"(<(?:ofd:)?Page\b[^>]*>)");
    auto begin = std::sregex_iterator(document_xml.begin(), document_xml.end(), page_tag_re);
    auto end = std::sregex_iterator();

    for (auto it = begin; it != end; ++it) {
        const std::string page_tag = it->str();
        OfdPageInfo page;
        if (xml_utils::ExtractTagAttribute(page_tag, "Page", "ID", &page.page_id) != LIBOFD_OK) {
            page.page_id.clear();
        }
        if (xml_utils::ExtractTagAttribute(page_tag, "Page", "BaseLoc", &page.base_loc) != LIBOFD_OK) {
            page.base_loc.clear();
        }
        out_pages->push_back(page);
    }
    EnsurePageDefaults(out_pages);
}

static int ExtractMaxUnitIdFromPageXml(const std::string& page_xml) {
    int max_id = 0;
    const std::regex id_re("\\bID\\s*=\\s*\"(\\d+)\"");
    auto begin = std::sregex_iterator(page_xml.begin(), page_xml.end(), id_re);
    auto end = std::sregex_iterator();
    for (auto it = begin; it != end; ++it) {
        try {
            max_id = std::max(max_id, std::stoi((*it)[1].str()));
        } catch (...) {
        }
    }
    return max_id;
}

static std::vector<std::pair<size_t, size_t>> FindLayerRanges(const std::string& content_xml) {
    std::vector<std::pair<size_t, size_t>> ranges;
    const std::regex layer_re(R"(<(?:ofd:)?Layer\b[^>]*>[\s\S]*?</(?:ofd:)?Layer>)");
    auto begin = std::sregex_iterator(content_xml.begin(), content_xml.end(), layer_re);
    auto end = std::sregex_iterator();
    for (auto it = begin; it != end; ++it) {
        ranges.emplace_back(static_cast<size_t>(it->position()), static_cast<size_t>(it->length()));
    }
    return ranges;
}

static std::string BuildSimplePageXmlWithLayer(const std::string& layer_xml) {
    std::ostringstream page_ss;
    page_ss << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    page_ss << "<ofd:Page xmlns:ofd=\"http://www.ofdspec.org/2016\">\n";
    page_ss << "  <ofd:Content>\n";
    page_ss << "    " << layer_xml << "\n";
    page_ss << "  </ofd:Content>\n";
    page_ss << "</ofd:Page>\n";
    return page_ss.str();
}

static bool LooksLikeLayerXml(const std::string& xml) {
    return xml.find("<ofd:Layer") != std::string::npos || xml.find("<Layer") != std::string::npos;
}

static bool LooksLikeAtomicObjectXml(const std::string& xml) {
    return xml.find("<ofd:TextObject") != std::string::npos || xml.find("<TextObject") != std::string::npos ||
           xml.find("<ofd:ImageObject") != std::string::npos || xml.find("<ImageObject") != std::string::npos ||
           xml.find("<ofd:PathObject") != std::string::npos || xml.find("<PathObject") != std::string::npos;
}

static std::vector<std::pair<size_t, size_t>> FindAtomicObjectRanges(const std::string& layer_xml) {
    std::vector<std::pair<size_t, size_t>> ranges;
    const std::regex object_re(
        R"(<(?:ofd:)?(?:TextObject|ImageObject|PathObject)\b[^>]*(?:\/>|>[\s\S]*?<\/(?:ofd:)?(?:TextObject|ImageObject|PathObject)>))");
    auto begin = std::sregex_iterator(layer_xml.begin(), layer_xml.end(), object_re);
    auto end = std::sregex_iterator();
    for (auto it = begin; it != end; ++it) {
        ranges.emplace_back(static_cast<size_t>(it->position()), static_cast<size_t>(it->length()));
    }
    return ranges;
}

void OfdDocument::CreateEmpty(const std::string& doc_id, const std::string& creator) {
    info_ = OfdDocumentInfo{};
    info_.doc_id = doc_id.empty() ? "libofd-doc-1" : doc_id;
    info_.creator = creator.empty() ? "libOFD" : creator;
    info_.creation_date = NowDateIso();
    info_.max_unit_id = "1";
    pages_.clear();
    signature_blob_.clear();
    common_data_xml_.clear();
    outline_xml_.clear();
    permissions_xml_.clear();
    form_xml_.clear();
    page_annotations_xml_.clear();
    page_actions_xml_.clear();
    loaded_ = true;
}

libofd_status_t OfdDocument::LoadFromPath(const std::string& path) {
    if (path.empty()) {
        return LIBOFD_ERR_INVALID_ARGUMENT;
    }
    const fs::path input(path);
    if (!fs::exists(input)) {
        return LIBOFD_ERR_NOT_FOUND;
    }
    if (fs::is_directory(input)) {
        return LoadFromExplodedPackage(path);
    }
    // TODO: Add zip-based .ofd package loader.
    return LIBOFD_ERR_UNSUPPORTED;
}

libofd_status_t OfdDocument::LoadFromExplodedPackage(const std::string& package_root) {
    if (package_root.empty()) {
        return LIBOFD_ERR_INVALID_ARGUMENT;
    }

    ParsedOfdPackage parsed;
    PackageReader reader;
    libofd_status_t status = reader.ReadExplodedPackage(package_root, &parsed);
    if (status != LIBOFD_OK) {
        return status;
    }

    info_.package_root = package_root;
    info_.ofd_xml_path = (fs::path(package_root) / "OFD.xml").string();
    const fs::path doc_xml_path = fs::path(package_root) / NormalizeOfdPath(parsed.doc_root_relative_path);
    info_.document_xml_path = doc_xml_path.string();
    pages_.clear();

    // Per GB/T 33190 OFD structure, these metadata are typically placed in Document.xml.
    std::string value;
    status = xml_utils::ExtractTagText(parsed.document_xml_content, "DocID", &value);
    if (status == LIBOFD_OK) {
        info_.doc_id = value;
    }

    status = xml_utils::ExtractTagText(parsed.document_xml_content, "Creator", &value);
    if (status == LIBOFD_OK) {
        info_.creator = value;
    }

    status = xml_utils::ExtractTagText(parsed.document_xml_content, "CreationDate", &value);
    if (status == LIBOFD_OK) {
        info_.creation_date = value;
    }

    status = xml_utils::ExtractTagText(parsed.document_xml_content, "MaxUnitID", &value);
    if (status == LIBOFD_OK) {
        info_.max_unit_id = value;
    }
    const std::regex common_data_re(R"(<(?:ofd:)?CommonData\b[^>]*>[\s\S]*?</(?:ofd:)?CommonData>)");
    std::smatch common_match;
    if (std::regex_search(parsed.document_xml_content, common_match, common_data_re)) {
        common_data_xml_ = common_match.str();
    }

    ParsePagesFromDocumentXml(parsed.document_xml_content, &pages_);
    const fs::path doc_dir = doc_xml_path.parent_path();
    for (auto& page : pages_) {
        if (page.base_loc.empty()) {
            continue;
        }
        std::string page_xml;
        status = ReadWholeFile(ResolveOfdPath(fs::path(package_root), doc_dir, page.base_loc), &page_xml);
        if (status != LIBOFD_OK) {
            continue;
        }
        page.content_xml = page_xml;
        const std::regex text_code_re(R"(<(?:ofd:)?TextCode\b[^>]*>\s*([\s\S]*?)\s*</(?:ofd:)?TextCode>)");
        auto begin = std::sregex_iterator(page_xml.begin(), page_xml.end(), text_code_re);
        auto end = std::sregex_iterator();
        std::ostringstream merged;
        for (auto it = begin; it != end; ++it) {
            if (it != begin) {
                merged << "\n";
            }
            merged << (*it)[1].str();
        }
        page.text = merged.str();
        if (page.text.empty()) {
            std::string text;
            status = xml_utils::ExtractTagText(page_xml, "TextCode", &text);
            if (status == LIBOFD_OK) {
                page.text = text;
            }
        }
    }
    const fs::path signature_path = fs::path(package_root) / "Signatures" / "signature.bin";
    signature_blob_.clear();
    if (fs::exists(signature_path)) {
        ReadWholeBinaryFile(signature_path, &signature_blob_);
    }
    auto read_optional = [&](const fs::path& p, std::string* out) {
        if (out == nullptr) {
            return;
        }
        out->clear();
        if (fs::exists(p)) {
            std::string tmp;
            if (ReadWholeFile(p, &tmp) == LIBOFD_OK) {
                *out = tmp;
            }
        }
    };
    const fs::path ext_dir = fs::path(package_root) / "Doc_0" / "Extensions";
    read_optional(ext_dir / "Outlines.xml", &outline_xml_);
    read_optional(ext_dir / "Permissions.xml", &permissions_xml_);
    read_optional(ext_dir / "Form.xml", &form_xml_);
    page_annotations_xml_.clear();
    page_actions_xml_.clear();
    for (size_t i = 0; i < pages_.size(); ++i) {
        std::string ann;
        std::string act;
        read_optional(ext_dir / ("Page_" + std::to_string(i) + "_Annotations.xml"), &ann);
        read_optional(ext_dir / ("Page_" + std::to_string(i) + "_Actions.xml"), &act);
        if (!ann.empty()) {
            page_annotations_xml_[i] = ann;
        }
        if (!act.empty()) {
            page_actions_xml_[i] = act;
        }
    }
    loaded_ = true;
    return LIBOFD_OK;
}

libofd_status_t OfdDocument::SaveToExplodedPackage(const std::string& output_root) const {
    if (!loaded_ || output_root.empty()) {
        return LIBOFD_ERR_INVALID_ARGUMENT;
    }

    std::error_code ec;
    fs::create_directories(output_root, ec);
    if (ec) {
        return LIBOFD_ERR_IO;
    }

    std::vector<OfdPageInfo> pages = pages_;
    EnsurePageDefaults(&pages);

    auto parse_positive_id = [](const std::string& s) -> int {
        try {
            const int v = std::stoi(s);
            return v > 0 ? v : 0;
        } catch (...) {
            return 0;
        }
    };

    // UnitID must be globally unique and MaxUnitID should cover every referenced ID.
    int max_unit_id = 1;  // default font ID in PublicRes.xml
    for (const auto& page : pages) {
        max_unit_id = std::max(max_unit_id, parse_positive_id(page.page_id));
    }
    const int content_start_id = std::max(2, max_unit_id + 1);
    int next_unit_id = content_start_id;
    for (const auto& page : pages) {
        if (!page.content_xml.empty()) {
            max_unit_id = std::max(max_unit_id, ExtractMaxUnitIdFromPageXml(page.content_xml));
        } else {
            const auto lines = SplitLinesPreserveEmpty(page.text);
            // Per page: 1 Layer + N TextObject
            const int page_span = 1 + static_cast<int>(lines.size());
            max_unit_id = std::max(max_unit_id, next_unit_id + page_span - 1);
            next_unit_id += page_span;
        }
    }

    const std::string ofd_xml = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                                "<ofd:OFD xmlns:ofd=\"http://www.ofdspec.org/2016\" Version=\"1.2\" DocType=\"OFD\">\n"
                                "  <ofd:DocBody>\n"
                                "    <ofd:DocInfo>\n"
                                "      <ofd:DocID>" +
                                EscapeXml(info_.doc_id) + "</ofd:DocID>\n"
                                                           "      <ofd:Creator>" +
                                EscapeXml(info_.creator) + "</ofd:Creator>\n"
                                                            "      <ofd:CreationDate>" +
                                EscapeXml(info_.creation_date) + "</ofd:CreationDate>\n"
                                                                 "    </ofd:DocInfo>\n"
                                                                 "    <ofd:DocRoot>Doc_0/Document.xml</ofd:DocRoot>\n"
                                                                 "  </ofd:DocBody>\n"
                                                                 "</ofd:OFD>\n";
    libofd_status_t status = WriteTextFile(fs::path(output_root) / "OFD.xml", ofd_xml);
    if (status != LIBOFD_OK) {
        return status;
    }

    std::ostringstream doc_ss;
    doc_ss << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    doc_ss << "<ofd:Document xmlns:ofd=\"http://www.ofdspec.org/2016\">\n";
    if (!common_data_xml_.empty()) {
        doc_ss << "  " << xml_utils::Trim(common_data_xml_) << "\n";
    } else {
        doc_ss << "  <ofd:CommonData>\n";
        doc_ss << "    <ofd:PageArea>\n";
        doc_ss << "      <ofd:PhysicalBox>0 0 210 297</ofd:PhysicalBox>\n";
        doc_ss << "      <ofd:ApplicationBox>0 0 210 297</ofd:ApplicationBox>\n";
        doc_ss << "    </ofd:PageArea>\n";
        doc_ss << "    <ofd:PublicRes>PublicRes.xml</ofd:PublicRes>\n";
        doc_ss << "    <ofd:MaxUnitID>" << max_unit_id << "</ofd:MaxUnitID>\n";
        doc_ss << "  </ofd:CommonData>\n";
    }
    doc_ss << "  <ofd:DocInfo>\n";
    doc_ss << "    <ofd:DocID>" << EscapeXml(info_.doc_id) << "</ofd:DocID>\n";
    doc_ss << "    <ofd:Creator>" << EscapeXml(info_.creator) << "</ofd:Creator>\n";
    doc_ss << "    <ofd:CreationDate>" << EscapeXml(info_.creation_date) << "</ofd:CreationDate>\n";
    doc_ss << "  </ofd:DocInfo>\n";
    doc_ss << "  <ofd:Pages>\n";
    for (const auto& page : pages) {
        fs::path page_rel = NormalizeOfdPath(page.base_loc);
        if (page_rel.string().rfind("Doc_0/", 0U) == 0U) {
            page_rel = page_rel.lexically_relative("Doc_0");
        }
        doc_ss << "    <ofd:Page ID=\"" << EscapeXml(page.page_id) << "\" BaseLoc=\"" << EscapeXml(page_rel.string())
               << "\" />\n";
    }
    doc_ss << "  </ofd:Pages>\n";
    if (!outline_xml_.empty()) {
        doc_ss << "  <ofd:Outlines>Extensions/Outlines.xml</ofd:Outlines>\n";
    }
    if (!permissions_xml_.empty()) {
        doc_ss << "  <ofd:Permissions>Extensions/Permissions.xml</ofd:Permissions>\n";
    }
    if (!form_xml_.empty()) {
        doc_ss << "  <ofd:Form>Extensions/Form.xml</ofd:Form>\n";
    }
    doc_ss << "</ofd:Document>\n";
    status = WriteTextFile(fs::path(output_root) / "Doc_0" / "Document.xml", doc_ss.str());
    if (status != LIBOFD_OK) {
        return status;
    }

    const std::string public_res_xml =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<ofd:Res xmlns:ofd=\"http://www.ofdspec.org/2016\">\n"
        "  <ofd:Fonts>\n"
        "    <ofd:Font ID=\"1\" FontName=\"宋体\" FamilyName=\"宋体\"/>\n"
        "  </ofd:Fonts>\n"
        "</ofd:Res>\n";
    status = WriteTextFile(fs::path(output_root) / "Doc_0" / "PublicRes.xml", public_res_xml);
    if (status != LIBOFD_OK) {
        return status;
    }

    int emit_unit_id = content_start_id;
    for (const auto& page : pages) {
        std::string page_xml;
        if (!page.content_xml.empty()) {
            page_xml = page.content_xml;
            emit_unit_id = std::max(emit_unit_id, ExtractMaxUnitIdFromPageXml(page_xml) + 1);
        } else {
            std::ostringstream page_ss;
            page_ss << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
            page_ss << "<ofd:Page xmlns:ofd=\"http://www.ofdspec.org/2016\">\n";
            page_ss << "  <ofd:Content>\n";
            const int layer_id = emit_unit_id++;
            page_ss << "    <ofd:Layer Type=\"Body\" ID=\"" << layer_id << "\">\n";
            const auto lines = SplitLinesPreserveEmpty(page.text);
            double y_mm = 10.0;
            for (const auto& line : lines) {
                const int text_id = emit_unit_id++;
                page_ss << "      <ofd:TextObject ID=\"" << text_id
                        << "\" Font=\"1\" Size=\"3.5\" Boundary=\"10 " << y_mm << " 180 4.5\">\n";
                page_ss << "        <ofd:FillColor Value=\"0 0 0\"/>\n";
                page_ss << "        <ofd:TextCode X=\"0\" Y=\"3.5\">" << EscapeXml(line) << "</ofd:TextCode>\n";
                page_ss << "      </ofd:TextObject>\n";
                y_mm += 5.0;
            }
            page_ss << "    </ofd:Layer>\n";
            page_ss << "  </ofd:Content>\n";
            page_ss << "</ofd:Page>\n";
            page_xml = page_ss.str();
        }
        const fs::path page_rel = NormalizeOfdPath(page.base_loc);
        fs::path page_out = fs::path(output_root) / "Doc_0" / page_rel;
        if (page_rel.string().rfind("Doc_0/", 0U) == 0U) {
            page_out = fs::path(output_root) / page_rel;
        }
        status = WriteTextFile(page_out, page_xml);
        if (status != LIBOFD_OK) {
            return status;
        }
    }

    if (!signature_blob_.empty()) {
        status = WriteBinaryFile(fs::path(output_root) / "Signatures" / "signature.bin", signature_blob_);
        if (status != LIBOFD_OK) {
            return status;
        }
    }
    const fs::path ext_dir = fs::path(output_root) / "Doc_0" / "Extensions";
    if (!outline_xml_.empty()) {
        status = WriteTextFile(ext_dir / "Outlines.xml", outline_xml_);
        if (status != LIBOFD_OK) {
            return status;
        }
    }
    if (!permissions_xml_.empty()) {
        status = WriteTextFile(ext_dir / "Permissions.xml", permissions_xml_);
        if (status != LIBOFD_OK) {
            return status;
        }
    }
    if (!form_xml_.empty()) {
        status = WriteTextFile(ext_dir / "Form.xml", form_xml_);
        if (status != LIBOFD_OK) {
            return status;
        }
    }
    for (const auto& kv : page_annotations_xml_) {
        status = WriteTextFile(ext_dir / ("Page_" + std::to_string(kv.first) + "_Annotations.xml"), kv.second);
        if (status != LIBOFD_OK) {
            return status;
        }
    }
    for (const auto& kv : page_actions_xml_) {
        status = WriteTextFile(ext_dir / ("Page_" + std::to_string(kv.first) + "_Actions.xml"), kv.second);
        if (status != LIBOFD_OK) {
            return status;
        }
    }

    return LIBOFD_OK;
}

libofd_status_t OfdDocument::ExportToText(const std::string& output_text_file) const {
    if (!loaded_ || output_text_file.empty()) {
        return LIBOFD_ERR_INVALID_ARGUMENT;
    }
    std::ostringstream ss;
    ss << "# doc_id: " << info_.doc_id << "\n";
    ss << "# creator: " << info_.creator << "\n";
    ss << "# creation_date: " << info_.creation_date << "\n";
    ss << "# pages: " << pages_.size() << "\n\n";
    for (size_t i = 0; i < pages_.size(); ++i) {
        ss << "=== PAGE " << (i + 1U) << " ===\n";
        ss << pages_[i].text << "\n";
        if (i + 1U < pages_.size()) {
            ss << "\n";
        }
    }
    return WriteTextFile(output_text_file, ss.str());
}

libofd_status_t OfdDocument::ImportFromText(
    const std::string& input_text_file, const std::string& doc_id, const std::string& creator) {
    if (input_text_file.empty()) {
        return LIBOFD_ERR_INVALID_ARGUMENT;
    }
    std::string content;
    libofd_status_t status = ReadWholeFile(input_text_file, &content);
    if (status != LIBOFD_OK) {
        return status;
    }

    CreateEmpty(doc_id, creator);
    pages_.clear();

    const std::regex splitter(R"(\n=== PAGE [0-9]+ ===\n)");
    std::sregex_token_iterator iter(content.begin(), content.end(), splitter, -1);
    std::sregex_token_iterator end;
    size_t idx = 0;
    for (; iter != end; ++iter) {
        std::string block = xml_utils::Trim(iter->str());
        if (block.empty()) {
            continue;
        }
        // Skip heading lines if text exported by this library.
        if (idx == 0 && block.rfind("# doc_id:", 0) == 0) {
            const size_t first_page_pos = block.find("=== PAGE");
            if (first_page_pos != std::string::npos) {
                block = xml_utils::Trim(block.substr(first_page_pos));
            } else {
                continue;
            }
        }
        OfdPageInfo page;
        page.page_id = std::to_string(pages_.size() + 1U);
        page.base_loc = "Pages/Page_" + std::to_string(pages_.size()) + "/Content.xml";
        page.text = block;
        page.content_xml.clear();
        pages_.push_back(page);
        ++idx;
    }
    if (pages_.empty()) {
        AddPageText(content);
    }
    return LIBOFD_OK;
}

libofd_status_t OfdDocument::SetCreator(const std::string& creator) {
    if (!loaded_) {
        return LIBOFD_ERR_NOT_FOUND;
    }
    info_.creator = creator;
    return LIBOFD_OK;
}

libofd_status_t OfdDocument::AddPageText(const std::string& page_text) {
    if (!loaded_) {
        return LIBOFD_ERR_NOT_FOUND;
    }
    OfdPageInfo page;
    page.page_id = std::to_string(pages_.size() + 1U);
    page.base_loc = "Pages/Page_" + std::to_string(pages_.size()) + "/Content.xml";
    page.text = page_text;
    page.content_xml.clear();
    pages_.push_back(page);
    info_.max_unit_id = std::to_string(pages_.size());
    return LIBOFD_OK;
}

libofd_status_t OfdDocument::SetPageText(size_t page_index, const std::string& page_text) {
    if (!loaded_) {
        return LIBOFD_ERR_NOT_FOUND;
    }
    if (page_index >= pages_.size()) {
        return LIBOFD_ERR_NOT_FOUND;
    }
    pages_[page_index].text = page_text;
    pages_[page_index].content_xml.clear();
    return LIBOFD_OK;
}

libofd_status_t OfdDocument::GetPageText(size_t page_index, std::string* out_page_text) const {
    if (out_page_text == nullptr) {
        return LIBOFD_ERR_INVALID_ARGUMENT;
    }
    if (!loaded_) {
        return LIBOFD_ERR_NOT_FOUND;
    }
    if (page_index >= pages_.size()) {
        return LIBOFD_ERR_NOT_FOUND;
    }
    *out_page_text = pages_[page_index].text;
    return LIBOFD_OK;
}

libofd_status_t OfdDocument::SetPageContentXml(size_t page_index, const std::string& content_xml) {
    if (!loaded_) {
        return LIBOFD_ERR_NOT_FOUND;
    }
    if (page_index >= pages_.size() || content_xml.empty()) {
        return LIBOFD_ERR_INVALID_ARGUMENT;
    }
    pages_[page_index].content_xml = content_xml;

    // Keep plain-text view in sync for legacy APIs.
    std::ostringstream merged;
    const std::regex text_code_re(R"(<(?:ofd:)?TextCode\b[^>]*>\s*([\s\S]*?)\s*</(?:ofd:)?TextCode>)");
    auto begin = std::sregex_iterator(content_xml.begin(), content_xml.end(), text_code_re);
    auto end = std::sregex_iterator();
    for (auto it = begin; it != end; ++it) {
        if (it != begin) {
            merged << "\n";
        }
        merged << (*it)[1].str();
    }
    pages_[page_index].text = merged.str();
    return LIBOFD_OK;
}

libofd_status_t OfdDocument::GetPageContentXml(size_t page_index, std::string* out_content_xml) const {
    if (out_content_xml == nullptr) {
        return LIBOFD_ERR_INVALID_ARGUMENT;
    }
    if (!loaded_) {
        return LIBOFD_ERR_NOT_FOUND;
    }
    if (page_index >= pages_.size()) {
        return LIBOFD_ERR_NOT_FOUND;
    }
    *out_content_xml = pages_[page_index].content_xml;
    return LIBOFD_OK;
}

libofd_status_t OfdDocument::GetPageContentBlockCount(size_t page_index, size_t* out_count) const {
    if (out_count == nullptr) {
        return LIBOFD_ERR_INVALID_ARGUMENT;
    }
    if (!loaded_) {
        return LIBOFD_ERR_NOT_FOUND;
    }
    if (page_index >= pages_.size()) {
        return LIBOFD_ERR_NOT_FOUND;
    }
    *out_count = FindLayerRanges(pages_[page_index].content_xml).size();
    return LIBOFD_OK;
}

libofd_status_t OfdDocument::GetPageContentBlockXml(
    size_t page_index, size_t block_index, std::string* out_block_xml) const {
    if (out_block_xml == nullptr) {
        return LIBOFD_ERR_INVALID_ARGUMENT;
    }
    if (!loaded_) {
        return LIBOFD_ERR_NOT_FOUND;
    }
    if (page_index >= pages_.size()) {
        return LIBOFD_ERR_NOT_FOUND;
    }
    const auto ranges = FindLayerRanges(pages_[page_index].content_xml);
    if (block_index >= ranges.size()) {
        return LIBOFD_ERR_NOT_FOUND;
    }
    const auto& r = ranges[block_index];
    *out_block_xml = pages_[page_index].content_xml.substr(r.first, r.second);
    return LIBOFD_OK;
}

libofd_status_t OfdDocument::SetPageContentBlockXml(
    size_t page_index, size_t block_index, const std::string& block_xml) {
    if (!loaded_) {
        return LIBOFD_ERR_NOT_FOUND;
    }
    if (page_index >= pages_.size() || block_xml.empty() || !LooksLikeLayerXml(block_xml)) {
        return LIBOFD_ERR_INVALID_ARGUMENT;
    }
    std::string& page_xml = pages_[page_index].content_xml;
    if (page_xml.empty()) {
        if (block_index != 0U) {
            return LIBOFD_ERR_NOT_FOUND;
        }
        page_xml = BuildSimplePageXmlWithLayer(block_xml);
    } else {
        const auto ranges = FindLayerRanges(page_xml);
        if (block_index >= ranges.size()) {
            return LIBOFD_ERR_NOT_FOUND;
        }
        const auto& r = ranges[block_index];
        page_xml.replace(r.first, r.second, block_xml);
    }
    return SetPageContentXml(page_index, page_xml);
}

libofd_status_t OfdDocument::GetBlockObjectCount(size_t page_index, size_t block_index, size_t* out_count) const {
    if (out_count == nullptr) {
        return LIBOFD_ERR_INVALID_ARGUMENT;
    }
    std::string block_xml;
    libofd_status_t status = GetPageContentBlockXml(page_index, block_index, &block_xml);
    if (status != LIBOFD_OK) {
        return status;
    }
    *out_count = FindAtomicObjectRanges(block_xml).size();
    return LIBOFD_OK;
}

libofd_status_t OfdDocument::GetBlockObjectXml(
    size_t page_index, size_t block_index, size_t object_index, std::string* out_object_xml) const {
    if (out_object_xml == nullptr) {
        return LIBOFD_ERR_INVALID_ARGUMENT;
    }
    std::string block_xml;
    libofd_status_t status = GetPageContentBlockXml(page_index, block_index, &block_xml);
    if (status != LIBOFD_OK) {
        return status;
    }
    const auto ranges = FindAtomicObjectRanges(block_xml);
    if (object_index >= ranges.size()) {
        return LIBOFD_ERR_NOT_FOUND;
    }
    const auto& r = ranges[object_index];
    *out_object_xml = block_xml.substr(r.first, r.second);
    return LIBOFD_OK;
}

libofd_status_t OfdDocument::SetBlockObjectXml(
    size_t page_index, size_t block_index, size_t object_index, const std::string& object_xml) {
    if (object_xml.empty() || !LooksLikeAtomicObjectXml(object_xml)) {
        return LIBOFD_ERR_INVALID_ARGUMENT;
    }
    std::string block_xml;
    libofd_status_t status = GetPageContentBlockXml(page_index, block_index, &block_xml);
    if (status != LIBOFD_OK) {
        return status;
    }
    const auto ranges = FindAtomicObjectRanges(block_xml);
    if (object_index >= ranges.size()) {
        return LIBOFD_ERR_NOT_FOUND;
    }
    const auto& r = ranges[object_index];
    block_xml.replace(r.first, r.second, object_xml);
    return SetPageContentBlockXml(page_index, block_index, block_xml);
}

libofd_status_t OfdDocument::AddBlockObjectXml(size_t page_index, size_t block_index, const std::string& object_xml) {
    if (object_xml.empty() || !LooksLikeAtomicObjectXml(object_xml)) {
        return LIBOFD_ERR_INVALID_ARGUMENT;
    }
    std::string block_xml;
    libofd_status_t status = GetPageContentBlockXml(page_index, block_index, &block_xml);
    if (status != LIBOFD_OK) {
        return status;
    }
    size_t close_pos = block_xml.rfind("</ofd:Layer>");
    const size_t close_pos_plain = block_xml.rfind("</Layer>");
    if (close_pos == std::string::npos || (close_pos_plain != std::string::npos && close_pos_plain > close_pos)) {
        close_pos = close_pos_plain;
    }
    if (close_pos == std::string::npos) {
        return LIBOFD_ERR_PARSE;
    }
    block_xml.insert(close_pos, "      " + object_xml + "\n");
    return SetPageContentBlockXml(page_index, block_index, block_xml);
}

libofd_status_t OfdDocument::SetCommonDataXml(const std::string& common_data_xml) {
    if (!loaded_) {
        return LIBOFD_ERR_NOT_FOUND;
    }
    if (common_data_xml.empty()) {
        return LIBOFD_ERR_INVALID_ARGUMENT;
    }
    common_data_xml_ = common_data_xml;
    return LIBOFD_OK;
}

libofd_status_t OfdDocument::GetCommonDataXml(std::string* out_common_data_xml) const {
    if (out_common_data_xml == nullptr) {
        return LIBOFD_ERR_INVALID_ARGUMENT;
    }
    if (!loaded_) {
        return LIBOFD_ERR_NOT_FOUND;
    }
    *out_common_data_xml = common_data_xml_;
    return LIBOFD_OK;
}

libofd_status_t OfdDocument::SetOutlineXml(const std::string& outline_xml) {
    if (!loaded_) {
        return LIBOFD_ERR_NOT_FOUND;
    }
    if (outline_xml.empty()) {
        return LIBOFD_ERR_INVALID_ARGUMENT;
    }
    outline_xml_ = outline_xml;
    return LIBOFD_OK;
}

libofd_status_t OfdDocument::GetOutlineXml(std::string* out_outline_xml) const {
    if (out_outline_xml == nullptr) {
        return LIBOFD_ERR_INVALID_ARGUMENT;
    }
    if (!loaded_) {
        return LIBOFD_ERR_NOT_FOUND;
    }
    *out_outline_xml = outline_xml_;
    return LIBOFD_OK;
}

libofd_status_t OfdDocument::SetPermissionsXml(const std::string& permissions_xml) {
    if (!loaded_) {
        return LIBOFD_ERR_NOT_FOUND;
    }
    if (permissions_xml.empty()) {
        return LIBOFD_ERR_INVALID_ARGUMENT;
    }
    permissions_xml_ = permissions_xml;
    return LIBOFD_OK;
}

libofd_status_t OfdDocument::GetPermissionsXml(std::string* out_permissions_xml) const {
    if (out_permissions_xml == nullptr) {
        return LIBOFD_ERR_INVALID_ARGUMENT;
    }
    if (!loaded_) {
        return LIBOFD_ERR_NOT_FOUND;
    }
    *out_permissions_xml = permissions_xml_;
    return LIBOFD_OK;
}

libofd_status_t OfdDocument::SetFormXml(const std::string& form_xml) {
    if (!loaded_) {
        return LIBOFD_ERR_NOT_FOUND;
    }
    if (form_xml.empty()) {
        return LIBOFD_ERR_INVALID_ARGUMENT;
    }
    form_xml_ = form_xml;
    return LIBOFD_OK;
}

libofd_status_t OfdDocument::GetFormXml(std::string* out_form_xml) const {
    if (out_form_xml == nullptr) {
        return LIBOFD_ERR_INVALID_ARGUMENT;
    }
    if (!loaded_) {
        return LIBOFD_ERR_NOT_FOUND;
    }
    *out_form_xml = form_xml_;
    return LIBOFD_OK;
}

libofd_status_t OfdDocument::SetPageAnnotationsXml(size_t page_index, const std::string& annotations_xml) {
    if (!loaded_) {
        return LIBOFD_ERR_NOT_FOUND;
    }
    if (annotations_xml.empty()) {
        return LIBOFD_ERR_INVALID_ARGUMENT;
    }
    if (page_index >= pages_.size()) {
        return LIBOFD_ERR_NOT_FOUND;
    }
    page_annotations_xml_[page_index] = annotations_xml;
    return LIBOFD_OK;
}

libofd_status_t OfdDocument::GetPageAnnotationsXml(size_t page_index, std::string* out_annotations_xml) const {
    if (out_annotations_xml == nullptr) {
        return LIBOFD_ERR_INVALID_ARGUMENT;
    }
    if (!loaded_) {
        return LIBOFD_ERR_NOT_FOUND;
    }
    if (page_index >= pages_.size()) {
        return LIBOFD_ERR_NOT_FOUND;
    }
    const auto it = page_annotations_xml_.find(page_index);
    if (it == page_annotations_xml_.end()) {
        out_annotations_xml->clear();
        return LIBOFD_OK;
    }
    *out_annotations_xml = it->second;
    return LIBOFD_OK;
}

libofd_status_t OfdDocument::SetPageActionsXml(size_t page_index, const std::string& actions_xml) {
    if (!loaded_) {
        return LIBOFD_ERR_NOT_FOUND;
    }
    if (actions_xml.empty()) {
        return LIBOFD_ERR_INVALID_ARGUMENT;
    }
    if (page_index >= pages_.size()) {
        return LIBOFD_ERR_NOT_FOUND;
    }
    page_actions_xml_[page_index] = actions_xml;
    return LIBOFD_OK;
}

libofd_status_t OfdDocument::GetPageActionsXml(size_t page_index, std::string* out_actions_xml) const {
    if (out_actions_xml == nullptr) {
        return LIBOFD_ERR_INVALID_ARGUMENT;
    }
    if (!loaded_) {
        return LIBOFD_ERR_NOT_FOUND;
    }
    if (page_index >= pages_.size()) {
        return LIBOFD_ERR_NOT_FOUND;
    }
    const auto it = page_actions_xml_.find(page_index);
    if (it == page_actions_xml_.end()) {
        out_actions_xml->clear();
        return LIBOFD_OK;
    }
    *out_actions_xml = it->second;
    return LIBOFD_OK;
}

libofd_status_t OfdDocument::SetSignBackend(SignBackend backend) {
    sign_backend_ = backend;
    return LIBOFD_OK;
}

libofd_status_t OfdDocument::SetTaSSLRoot(const std::string& tassl_root) {
    if (tassl_root.empty()) {
        return LIBOFD_ERR_INVALID_ARGUMENT;
    }
    tassl_root_ = tassl_root;
    return LIBOFD_OK;
}

libofd_status_t OfdDocument::SignWithPrivateKeyPem(const std::string& private_key_pem_path) {
    if (!loaded_ || private_key_pem_path.empty()) {
        return LIBOFD_ERR_INVALID_ARGUMENT;
    }
    std::unique_ptr<CryptoProvider> provider;
    libofd_status_t status = CreateCryptoProvider(sign_backend_, tassl_root_, &provider);
    if (status != LIBOFD_OK) {
        return status;
    }
    std::string payload;
    status = BuildSignPayload(&payload);
    if (status != LIBOFD_OK) {
        return status;
    }
    return provider->Sign(payload, private_key_pem_path, &signature_blob_);
}

libofd_status_t OfdDocument::VerifyWithPublicKeyPem(const std::string& public_key_pem_path, bool* out_verified) const {
    if (out_verified == nullptr || public_key_pem_path.empty()) {
        return LIBOFD_ERR_INVALID_ARGUMENT;
    }
    *out_verified = false;
    if (!loaded_) {
        return LIBOFD_ERR_NOT_FOUND;
    }
    if (signature_blob_.empty()) {
        return LIBOFD_ERR_NOT_FOUND;
    }
    std::unique_ptr<CryptoProvider> provider;
    libofd_status_t status = CreateCryptoProvider(sign_backend_, tassl_root_, &provider);
    if (status != LIBOFD_OK) {
        return status;
    }
    std::string payload;
    status = BuildSignPayload(&payload);
    if (status != LIBOFD_OK) {
        return status;
    }
    return provider->Verify(payload, public_key_pem_path, signature_blob_, out_verified);
}

libofd_status_t OfdDocument::BuildSignPayload(std::string* out_payload) const {
    if (out_payload == nullptr) {
        return LIBOFD_ERR_INVALID_ARGUMENT;
    }
    if (!loaded_) {
        return LIBOFD_ERR_NOT_FOUND;
    }
    *out_payload = BuildCanonicalPayload(info_, pages_);
    return LIBOFD_OK;
}

libofd_status_t OfdDocument::SetSignatureBlob(const std::vector<unsigned char>& signature_blob) {
    if (!loaded_) {
        return LIBOFD_ERR_NOT_FOUND;
    }
    signature_blob_ = signature_blob;
    return LIBOFD_OK;
}

} // namespace libofd

