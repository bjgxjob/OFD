#include "libofd/libofd.h"

#include <algorithm>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#include "libofd/document.h"
#include "libofd/signature.h"
#include "pdf_engine/pdf_engine.h"
#include "xml_utils.h"

struct libofd_handle {
    libofd::OfdDocument document;
    bool loaded;
    bool external_provider_enabled;
    libofd_external_sign_provider_t external_provider;
    bool external_convert_provider_enabled;
    libofd_external_convert_provider_t external_convert_provider;
    bool external_image_decode_provider_enabled;
    libofd_external_image_decode_provider_t external_image_decode_provider;
    libofd_pdf_to_ofd_mode_t pdf_to_ofd_mode;
};

static void CopyString(char* dest, size_t capacity, const std::string& src) {
    if (dest == nullptr || capacity == 0U) {
        return;
    }
    const size_t n = std::min(capacity - 1U, src.size());
    std::memcpy(dest, src.data(), n);
    dest[n] = '\0';
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

static bool ParseBoundary4(const std::string& value, double* x, double* y, double* w, double* h) {
    if (x == nullptr || y == nullptr || w == nullptr || h == nullptr) {
        return false;
    }
    return std::sscanf(value.c_str(), "%lf %lf %lf %lf", x, y, w, h) == 4;
}

static std::string BuildBoundary4(double x, double y, double w, double h) {
    std::ostringstream ss;
    ss << x << " " << y << " " << w << " " << h;
    return ss.str();
}

static libofd_status_t BuildTextObjectXml(const libofd_text_object_t& object, std::string* out_xml) {
    if (out_xml == nullptr) {
        return LIBOFD_ERR_INVALID_ARGUMENT;
    }
    std::ostringstream ss;
    ss << "<ofd:TextObject ID=\"" << EscapeXmlText(object.id) << "\" Font=\"" << EscapeXmlText(object.font)
       << "\" Size=\"" << object.size << "\" Boundary=\""
       << BuildBoundary4(object.boundary_x, object.boundary_y, object.boundary_w, object.boundary_h) << "\">";
    ss << "<ofd:TextCode X=\"0\" Y=\"" << object.size << "\">" << EscapeXmlText(object.text) << "</ofd:TextCode>";
    ss << "</ofd:TextObject>";
    *out_xml = ss.str();
    return LIBOFD_OK;
}

static libofd_status_t BuildImageObjectXml(const libofd_image_object_t& object, std::string* out_xml) {
    if (out_xml == nullptr) {
        return LIBOFD_ERR_INVALID_ARGUMENT;
    }
    std::ostringstream ss;
    ss << "<ofd:ImageObject ID=\"" << EscapeXmlText(object.id) << "\" ResourceID=\"" << EscapeXmlText(object.resource_id)
       << "\" Boundary=\"" << BuildBoundary4(object.boundary_x, object.boundary_y, object.boundary_w, object.boundary_h)
       << "\"";
    if (object.ctm[0] != '\0') {
        ss << " CTM=\"" << EscapeXmlText(object.ctm) << "\"";
    }
    ss << "/>";
    *out_xml = ss.str();
    return LIBOFD_OK;
}

static libofd_status_t BuildPathObjectXml(const libofd_path_object_t& object, std::string* out_xml) {
    if (out_xml == nullptr) {
        return LIBOFD_ERR_INVALID_ARGUMENT;
    }
    std::ostringstream ss;
    ss << "<ofd:PathObject ID=\"" << EscapeXmlText(object.id) << "\" Boundary=\""
       << BuildBoundary4(object.boundary_x, object.boundary_y, object.boundary_w, object.boundary_h) << "\">";
    ss << "<ofd:AbbreviatedData>" << EscapeXmlText(object.abbreviated_data) << "</ofd:AbbreviatedData>";
    ss << "</ofd:PathObject>";
    *out_xml = ss.str();
    return LIBOFD_OK;
}

static libofd_status_t ParseTextObjectXml(const std::string& xml, libofd_text_object_t* out_object) {
    if (out_object == nullptr) {
        return LIBOFD_ERR_INVALID_ARGUMENT;
    }
    std::memset(out_object, 0, sizeof(*out_object));
    std::string value;
    if (libofd::xml_utils::ExtractTagAttribute(xml, "TextObject", "ID", &value) != LIBOFD_OK) {
        return LIBOFD_ERR_PARSE;
    }
    CopyString(out_object->id, sizeof(out_object->id), value);
    if (libofd::xml_utils::ExtractTagAttribute(xml, "TextObject", "Font", &value) == LIBOFD_OK) {
        CopyString(out_object->font, sizeof(out_object->font), value);
    }
    if (libofd::xml_utils::ExtractTagAttribute(xml, "TextObject", "Size", &value) == LIBOFD_OK) {
        out_object->size = std::atof(value.c_str());
    }
    if (libofd::xml_utils::ExtractTagAttribute(xml, "TextObject", "Boundary", &value) == LIBOFD_OK) {
        ParseBoundary4(
            value, &out_object->boundary_x, &out_object->boundary_y, &out_object->boundary_w, &out_object->boundary_h);
    }
    if (libofd::xml_utils::ExtractTagText(xml, "TextCode", &value) == LIBOFD_OK) {
        CopyString(out_object->text, sizeof(out_object->text), value);
    }
    return LIBOFD_OK;
}

static libofd_status_t ParseImageObjectXml(const std::string& xml, libofd_image_object_t* out_object) {
    if (out_object == nullptr) {
        return LIBOFD_ERR_INVALID_ARGUMENT;
    }
    std::memset(out_object, 0, sizeof(*out_object));
    std::string value;
    if (libofd::xml_utils::ExtractTagAttribute(xml, "ImageObject", "ID", &value) != LIBOFD_OK) {
        return LIBOFD_ERR_PARSE;
    }
    CopyString(out_object->id, sizeof(out_object->id), value);
    if (libofd::xml_utils::ExtractTagAttribute(xml, "ImageObject", "ResourceID", &value) == LIBOFD_OK) {
        CopyString(out_object->resource_id, sizeof(out_object->resource_id), value);
    }
    if (libofd::xml_utils::ExtractTagAttribute(xml, "ImageObject", "Boundary", &value) == LIBOFD_OK) {
        ParseBoundary4(
            value, &out_object->boundary_x, &out_object->boundary_y, &out_object->boundary_w, &out_object->boundary_h);
    }
    if (libofd::xml_utils::ExtractTagAttribute(xml, "ImageObject", "CTM", &value) == LIBOFD_OK) {
        CopyString(out_object->ctm, sizeof(out_object->ctm), value);
    }
    return LIBOFD_OK;
}

static libofd_status_t ParsePathObjectXml(const std::string& xml, libofd_path_object_t* out_object) {
    if (out_object == nullptr) {
        return LIBOFD_ERR_INVALID_ARGUMENT;
    }
    std::memset(out_object, 0, sizeof(*out_object));
    std::string value;
    if (libofd::xml_utils::ExtractTagAttribute(xml, "PathObject", "ID", &value) != LIBOFD_OK) {
        return LIBOFD_ERR_PARSE;
    }
    CopyString(out_object->id, sizeof(out_object->id), value);
    if (libofd::xml_utils::ExtractTagAttribute(xml, "PathObject", "Boundary", &value) == LIBOFD_OK) {
        ParseBoundary4(
            value, &out_object->boundary_x, &out_object->boundary_y, &out_object->boundary_w, &out_object->boundary_h);
    }
    if (libofd::xml_utils::ExtractTagAttribute(xml, "PathObject", "AbbreviatedData", &value) == LIBOFD_OK ||
        libofd::xml_utils::ExtractTagText(xml, "AbbreviatedData", &value) == LIBOFD_OK) {
        CopyString(out_object->abbreviated_data, sizeof(out_object->abbreviated_data), value);
    }
    return LIBOFD_OK;
}

static bool TryExtractObjectId(const std::string& object_xml, std::string* out_id) {
    if (out_id == nullptr) {
        return false;
    }
    std::string id;
    if (libofd::xml_utils::ExtractTagAttribute(object_xml, "TextObject", "ID", &id) == LIBOFD_OK ||
        libofd::xml_utils::ExtractTagAttribute(object_xml, "ImageObject", "ID", &id) == LIBOFD_OK ||
        libofd::xml_utils::ExtractTagAttribute(object_xml, "PathObject", "ID", &id) == LIBOFD_OK) {
        *out_id = id;
        return true;
    }
    return false;
}

static libofd_status_t FindBlockObjectIndexById(
    const libofd_handle_t* handle, size_t page_index, size_t block_index, const char* object_id, size_t* out_index) {
    if (handle == nullptr || object_id == nullptr || object_id[0] == '\0' || out_index == nullptr) {
        return LIBOFD_ERR_INVALID_ARGUMENT;
    }
    size_t count = 0;
    libofd_status_t status = handle->document.GetBlockObjectCount(page_index, block_index, &count);
    if (status != LIBOFD_OK) {
        return status;
    }
    for (size_t i = 0; i < count; ++i) {
        std::string object_xml;
        status = handle->document.GetBlockObjectXml(page_index, block_index, i, &object_xml);
        if (status != LIBOFD_OK) {
            return status;
        }
        std::string id;
        if (TryExtractObjectId(object_xml, &id) && id == object_id) {
            *out_index = i;
            return LIBOFD_OK;
        }
    }
    return LIBOFD_ERR_NOT_FOUND;
}

struct BookmarkEntry {
    std::string title;
    std::string page_id;
};

static std::vector<BookmarkEntry> ParseBookmarkEntries(const std::string& outline_xml) {
    std::vector<BookmarkEntry> entries;
    const std::regex elem_re(R"(<(?:ofd:)?OutlineElem\b[^>]*>)");
    auto begin = std::sregex_iterator(outline_xml.begin(), outline_xml.end(), elem_re);
    auto end = std::sregex_iterator();
    for (auto it = begin; it != end; ++it) {
        const std::string tag = it->str();
        BookmarkEntry entry;
        libofd::xml_utils::ExtractTagAttribute(tag, "OutlineElem", "Title", &entry.title);
        if (libofd::xml_utils::ExtractTagAttribute(tag, "OutlineElem", "PageID", &entry.page_id) != LIBOFD_OK) {
            libofd::xml_utils::ExtractTagAttribute(tag, "OutlineElem", "Page", &entry.page_id);
        }
        entries.push_back(entry);
    }
    return entries;
}

extern "C" {

const char* libofd_status_message(libofd_status_t status) {
    switch (status) {
        case LIBOFD_OK:
            return "ok";
        case LIBOFD_ERR_INVALID_ARGUMENT:
            return "invalid argument";
        case LIBOFD_ERR_IO:
            return "io error";
        case LIBOFD_ERR_NOT_FOUND:
            return "not found";
        case LIBOFD_ERR_PARSE:
            return "parse error";
        case LIBOFD_ERR_UNSUPPORTED:
            return "unsupported";
        default:
            return "unknown";
    }
}

libofd_handle_t* libofd_create(void) {
    libofd_handle_t* handle = new (std::nothrow) libofd_handle_t{};
    if (handle != nullptr) {
        handle->loaded = false;
        handle->external_provider_enabled = false;
        std::memset(&handle->external_provider, 0, sizeof(handle->external_provider));
        handle->external_convert_provider_enabled = false;
        std::memset(&handle->external_convert_provider, 0, sizeof(handle->external_convert_provider));
        handle->external_image_decode_provider_enabled = false;
        std::memset(&handle->external_image_decode_provider, 0, sizeof(handle->external_image_decode_provider));
        handle->pdf_to_ofd_mode = LIBOFD_PDF_TO_OFD_MODE_AUTO;
    }
    return handle;
}

void libofd_destroy(libofd_handle_t* handle) {
    delete handle;
}

libofd_status_t libofd_create_empty(libofd_handle_t* handle, const char* doc_id, const char* creator) {
    if (handle == nullptr) {
        return LIBOFD_ERR_INVALID_ARGUMENT;
    }
    handle->document.CreateEmpty(doc_id == nullptr ? "" : doc_id, creator == nullptr ? "" : creator);
    handle->loaded = true;
    return LIBOFD_OK;
}

libofd_status_t libofd_load_exploded_package(libofd_handle_t* handle, const char* package_root) {
    if (handle == nullptr || package_root == nullptr || package_root[0] == '\0') {
        return LIBOFD_ERR_INVALID_ARGUMENT;
    }

    libofd_status_t status = handle->document.LoadFromExplodedPackage(package_root);
    if (status != LIBOFD_OK) {
        handle->loaded = false;
        return status;
    }
    handle->loaded = true;
    return LIBOFD_OK;
}

libofd_status_t libofd_load_path(libofd_handle_t* handle, const char* path) {
    if (handle == nullptr || path == nullptr || path[0] == '\0') {
        return LIBOFD_ERR_INVALID_ARGUMENT;
    }
    libofd_status_t status = handle->document.LoadFromPath(path);
    if (status != LIBOFD_OK) {
        handle->loaded = false;
        return status;
    }
    handle->loaded = true;
    return LIBOFD_OK;
}

libofd_status_t libofd_save_exploded_package(const libofd_handle_t* handle, const char* output_root) {
    if (handle == nullptr || output_root == nullptr || output_root[0] == '\0') {
        return LIBOFD_ERR_INVALID_ARGUMENT;
    }
    if (!handle->loaded) {
        return LIBOFD_ERR_NOT_FOUND;
    }
    return handle->document.SaveToExplodedPackage(output_root);
}

libofd_status_t libofd_get_doc_info(const libofd_handle_t* handle, libofd_doc_info_t* out_info) {
    if (handle == nullptr || out_info == nullptr) {
        return LIBOFD_ERR_INVALID_ARGUMENT;
    }
    if (!handle->loaded) {
        return LIBOFD_ERR_NOT_FOUND;
    }

    std::memset(out_info, 0, sizeof(*out_info));
    const libofd::OfdDocumentInfo& info = handle->document.Info();
    CopyString(out_info->package_root, sizeof(out_info->package_root), info.package_root);
    CopyString(out_info->ofd_xml_path, sizeof(out_info->ofd_xml_path), info.ofd_xml_path);
    CopyString(out_info->document_xml_path, sizeof(out_info->document_xml_path), info.document_xml_path);
    CopyString(out_info->creator, sizeof(out_info->creator), info.creator);
    CopyString(out_info->creation_date, sizeof(out_info->creation_date), info.creation_date);
    CopyString(out_info->doc_id, sizeof(out_info->doc_id), info.doc_id);
    CopyString(out_info->max_unit_id, sizeof(out_info->max_unit_id), info.max_unit_id);
    return LIBOFD_OK;
}

libofd_status_t libofd_set_creator(libofd_handle_t* handle, const char* creator) {
    if (handle == nullptr || creator == nullptr) {
        return LIBOFD_ERR_INVALID_ARGUMENT;
    }
    if (!handle->loaded) {
        return LIBOFD_ERR_NOT_FOUND;
    }
    return handle->document.SetCreator(creator);
}

size_t libofd_get_page_count(const libofd_handle_t* handle) {
    if (handle == nullptr || !handle->loaded) {
        return 0;
    }
    return handle->document.Pages().size();
}

libofd_status_t libofd_get_page_info(const libofd_handle_t* handle, size_t page_index, libofd_page_info_t* out_info) {
    if (handle == nullptr || out_info == nullptr) {
        return LIBOFD_ERR_INVALID_ARGUMENT;
    }
    if (!handle->loaded) {
        return LIBOFD_ERR_NOT_FOUND;
    }
    const auto& pages = handle->document.Pages();
    if (page_index >= pages.size()) {
        return LIBOFD_ERR_NOT_FOUND;
    }

    std::memset(out_info, 0, sizeof(*out_info));
    CopyString(out_info->page_id, sizeof(out_info->page_id), pages[page_index].page_id);
    CopyString(out_info->base_loc, sizeof(out_info->base_loc), pages[page_index].base_loc);
    return LIBOFD_OK;
}

libofd_status_t libofd_add_page_text(libofd_handle_t* handle, const char* text) {
    if (handle == nullptr || text == nullptr) {
        return LIBOFD_ERR_INVALID_ARGUMENT;
    }
    if (!handle->loaded) {
        return LIBOFD_ERR_NOT_FOUND;
    }
    return handle->document.AddPageText(text);
}

libofd_status_t libofd_set_page_text(libofd_handle_t* handle, size_t page_index, const char* text) {
    if (handle == nullptr || text == nullptr) {
        return LIBOFD_ERR_INVALID_ARGUMENT;
    }
    if (!handle->loaded) {
        return LIBOFD_ERR_NOT_FOUND;
    }
    return handle->document.SetPageText(page_index, text);
}

libofd_status_t libofd_get_page_text(
    const libofd_handle_t* handle, size_t page_index, char* out_text, size_t out_text_capacity) {
    if (handle == nullptr || out_text == nullptr || out_text_capacity == 0U) {
        return LIBOFD_ERR_INVALID_ARGUMENT;
    }
    if (!handle->loaded) {
        return LIBOFD_ERR_NOT_FOUND;
    }
    std::string page_text;
    libofd_status_t status = handle->document.GetPageText(page_index, &page_text);
    if (status != LIBOFD_OK) {
        return status;
    }
    CopyString(out_text, out_text_capacity, page_text);
    return LIBOFD_OK;
}

libofd_status_t libofd_set_page_content_xml(libofd_handle_t* handle, size_t page_index, const char* content_xml) {
    if (handle == nullptr || content_xml == nullptr || content_xml[0] == '\0') {
        return LIBOFD_ERR_INVALID_ARGUMENT;
    }
    if (!handle->loaded) {
        return LIBOFD_ERR_NOT_FOUND;
    }
    return handle->document.SetPageContentXml(page_index, content_xml);
}

libofd_status_t libofd_get_page_content_xml(
    const libofd_handle_t* handle, size_t page_index, char* out_content_xml, size_t out_content_xml_capacity) {
    if (handle == nullptr || out_content_xml == nullptr || out_content_xml_capacity == 0U) {
        return LIBOFD_ERR_INVALID_ARGUMENT;
    }
    if (!handle->loaded) {
        return LIBOFD_ERR_NOT_FOUND;
    }
    std::string content_xml;
    libofd_status_t status = handle->document.GetPageContentXml(page_index, &content_xml);
    if (status != LIBOFD_OK) {
        return status;
    }
    CopyString(out_content_xml, out_content_xml_capacity, content_xml);
    return LIBOFD_OK;
}

libofd_status_t libofd_get_page_content_block_count(
    const libofd_handle_t* handle, size_t page_index, size_t* out_block_count) {
    if (handle == nullptr || out_block_count == nullptr) {
        return LIBOFD_ERR_INVALID_ARGUMENT;
    }
    if (!handle->loaded) {
        return LIBOFD_ERR_NOT_FOUND;
    }
    return handle->document.GetPageContentBlockCount(page_index, out_block_count);
}

libofd_status_t libofd_get_page_content_block_xml(
    const libofd_handle_t* handle, size_t page_index, size_t block_index, char* out_block_xml, size_t out_block_xml_capacity) {
    if (handle == nullptr || out_block_xml == nullptr || out_block_xml_capacity == 0U) {
        return LIBOFD_ERR_INVALID_ARGUMENT;
    }
    if (!handle->loaded) {
        return LIBOFD_ERR_NOT_FOUND;
    }
    std::string block_xml;
    libofd_status_t status = handle->document.GetPageContentBlockXml(page_index, block_index, &block_xml);
    if (status != LIBOFD_OK) {
        return status;
    }
    CopyString(out_block_xml, out_block_xml_capacity, block_xml);
    return LIBOFD_OK;
}

libofd_status_t libofd_set_page_content_block_xml(
    libofd_handle_t* handle, size_t page_index, size_t block_index, const char* block_xml) {
    if (handle == nullptr || block_xml == nullptr || block_xml[0] == '\0') {
        return LIBOFD_ERR_INVALID_ARGUMENT;
    }
    if (!handle->loaded) {
        return LIBOFD_ERR_NOT_FOUND;
    }
    return handle->document.SetPageContentBlockXml(page_index, block_index, block_xml);
}

libofd_status_t libofd_get_block_object_count(
    const libofd_handle_t* handle, size_t page_index, size_t block_index, size_t* out_object_count) {
    if (handle == nullptr || out_object_count == nullptr) {
        return LIBOFD_ERR_INVALID_ARGUMENT;
    }
    if (!handle->loaded) {
        return LIBOFD_ERR_NOT_FOUND;
    }
    return handle->document.GetBlockObjectCount(page_index, block_index, out_object_count);
}

libofd_status_t libofd_get_block_object_xml(
    const libofd_handle_t* handle, size_t page_index, size_t block_index, size_t object_index,
    char* out_object_xml, size_t out_object_xml_capacity) {
    if (handle == nullptr || out_object_xml == nullptr || out_object_xml_capacity == 0U) {
        return LIBOFD_ERR_INVALID_ARGUMENT;
    }
    if (!handle->loaded) {
        return LIBOFD_ERR_NOT_FOUND;
    }
    std::string object_xml;
    libofd_status_t status = handle->document.GetBlockObjectXml(page_index, block_index, object_index, &object_xml);
    if (status != LIBOFD_OK) {
        return status;
    }
    CopyString(out_object_xml, out_object_xml_capacity, object_xml);
    return LIBOFD_OK;
}

libofd_status_t libofd_get_block_object_index_by_id(
    const libofd_handle_t* handle, size_t page_index, size_t block_index, const char* object_id, size_t* out_object_index) {
    if (handle == nullptr || out_object_index == nullptr || object_id == nullptr || object_id[0] == '\0') {
        return LIBOFD_ERR_INVALID_ARGUMENT;
    }
    if (!handle->loaded) {
        return LIBOFD_ERR_NOT_FOUND;
    }
    return FindBlockObjectIndexById(handle, page_index, block_index, object_id, out_object_index);
}

libofd_status_t libofd_set_block_object_xml(
    libofd_handle_t* handle, size_t page_index, size_t block_index, size_t object_index, const char* object_xml) {
    if (handle == nullptr || object_xml == nullptr || object_xml[0] == '\0') {
        return LIBOFD_ERR_INVALID_ARGUMENT;
    }
    if (!handle->loaded) {
        return LIBOFD_ERR_NOT_FOUND;
    }
    return handle->document.SetBlockObjectXml(page_index, block_index, object_index, object_xml);
}

libofd_status_t libofd_add_block_object_xml(
    libofd_handle_t* handle, size_t page_index, size_t block_index, const char* object_xml) {
    if (handle == nullptr || object_xml == nullptr || object_xml[0] == '\0') {
        return LIBOFD_ERR_INVALID_ARGUMENT;
    }
    if (!handle->loaded) {
        return LIBOFD_ERR_NOT_FOUND;
    }
    return handle->document.AddBlockObjectXml(page_index, block_index, object_xml);
}

libofd_status_t libofd_get_text_object(
    const libofd_handle_t* handle, size_t page_index, size_t block_index, size_t object_index, libofd_text_object_t* out_object) {
    if (handle == nullptr || out_object == nullptr) {
        return LIBOFD_ERR_INVALID_ARGUMENT;
    }
    if (!handle->loaded) {
        return LIBOFD_ERR_NOT_FOUND;
    }
    std::string object_xml;
    libofd_status_t status = handle->document.GetBlockObjectXml(page_index, block_index, object_index, &object_xml);
    if (status != LIBOFD_OK) {
        return status;
    }
    return ParseTextObjectXml(object_xml, out_object);
}

libofd_status_t libofd_get_text_object_by_id(
    const libofd_handle_t* handle, size_t page_index, size_t block_index, const char* object_id, libofd_text_object_t* out_object) {
    if (handle == nullptr || out_object == nullptr || object_id == nullptr || object_id[0] == '\0') {
        return LIBOFD_ERR_INVALID_ARGUMENT;
    }
    size_t index = 0;
    libofd_status_t status = libofd_get_block_object_index_by_id(handle, page_index, block_index, object_id, &index);
    if (status != LIBOFD_OK) {
        return status;
    }
    return libofd_get_text_object(handle, page_index, block_index, index, out_object);
}

libofd_status_t libofd_set_text_object(
    libofd_handle_t* handle, size_t page_index, size_t block_index, size_t object_index, const libofd_text_object_t* object) {
    if (handle == nullptr || object == nullptr) {
        return LIBOFD_ERR_INVALID_ARGUMENT;
    }
    if (!handle->loaded) {
        return LIBOFD_ERR_NOT_FOUND;
    }
    std::string object_xml;
    libofd_status_t status = BuildTextObjectXml(*object, &object_xml);
    if (status != LIBOFD_OK) {
        return status;
    }
    return handle->document.SetBlockObjectXml(page_index, block_index, object_index, object_xml);
}

libofd_status_t libofd_set_text_object_by_id(
    libofd_handle_t* handle, size_t page_index, size_t block_index, const char* object_id, const libofd_text_object_t* object) {
    if (handle == nullptr || object == nullptr || object_id == nullptr || object_id[0] == '\0') {
        return LIBOFD_ERR_INVALID_ARGUMENT;
    }
    size_t index = 0;
    libofd_status_t status = libofd_get_block_object_index_by_id(handle, page_index, block_index, object_id, &index);
    if (status != LIBOFD_OK) {
        return status;
    }
    return libofd_set_text_object(handle, page_index, block_index, index, object);
}

libofd_status_t libofd_add_text_object(
    libofd_handle_t* handle, size_t page_index, size_t block_index, const libofd_text_object_t* object) {
    if (handle == nullptr || object == nullptr) {
        return LIBOFD_ERR_INVALID_ARGUMENT;
    }
    if (!handle->loaded) {
        return LIBOFD_ERR_NOT_FOUND;
    }
    std::string object_xml;
    libofd_status_t status = BuildTextObjectXml(*object, &object_xml);
    if (status != LIBOFD_OK) {
        return status;
    }
    return handle->document.AddBlockObjectXml(page_index, block_index, object_xml);
}

libofd_status_t libofd_get_image_object(
    const libofd_handle_t* handle, size_t page_index, size_t block_index, size_t object_index, libofd_image_object_t* out_object) {
    if (handle == nullptr || out_object == nullptr) {
        return LIBOFD_ERR_INVALID_ARGUMENT;
    }
    if (!handle->loaded) {
        return LIBOFD_ERR_NOT_FOUND;
    }
    std::string object_xml;
    libofd_status_t status = handle->document.GetBlockObjectXml(page_index, block_index, object_index, &object_xml);
    if (status != LIBOFD_OK) {
        return status;
    }
    return ParseImageObjectXml(object_xml, out_object);
}

libofd_status_t libofd_get_image_object_by_id(
    const libofd_handle_t* handle, size_t page_index, size_t block_index, const char* object_id, libofd_image_object_t* out_object) {
    if (handle == nullptr || out_object == nullptr || object_id == nullptr || object_id[0] == '\0') {
        return LIBOFD_ERR_INVALID_ARGUMENT;
    }
    size_t index = 0;
    libofd_status_t status = libofd_get_block_object_index_by_id(handle, page_index, block_index, object_id, &index);
    if (status != LIBOFD_OK) {
        return status;
    }
    return libofd_get_image_object(handle, page_index, block_index, index, out_object);
}

libofd_status_t libofd_set_image_object(
    libofd_handle_t* handle, size_t page_index, size_t block_index, size_t object_index, const libofd_image_object_t* object) {
    if (handle == nullptr || object == nullptr) {
        return LIBOFD_ERR_INVALID_ARGUMENT;
    }
    if (!handle->loaded) {
        return LIBOFD_ERR_NOT_FOUND;
    }
    std::string object_xml;
    libofd_status_t status = BuildImageObjectXml(*object, &object_xml);
    if (status != LIBOFD_OK) {
        return status;
    }
    return handle->document.SetBlockObjectXml(page_index, block_index, object_index, object_xml);
}

libofd_status_t libofd_set_image_object_by_id(
    libofd_handle_t* handle, size_t page_index, size_t block_index, const char* object_id, const libofd_image_object_t* object) {
    if (handle == nullptr || object == nullptr || object_id == nullptr || object_id[0] == '\0') {
        return LIBOFD_ERR_INVALID_ARGUMENT;
    }
    size_t index = 0;
    libofd_status_t status = libofd_get_block_object_index_by_id(handle, page_index, block_index, object_id, &index);
    if (status != LIBOFD_OK) {
        return status;
    }
    return libofd_set_image_object(handle, page_index, block_index, index, object);
}

libofd_status_t libofd_add_image_object(
    libofd_handle_t* handle, size_t page_index, size_t block_index, const libofd_image_object_t* object) {
    if (handle == nullptr || object == nullptr) {
        return LIBOFD_ERR_INVALID_ARGUMENT;
    }
    if (!handle->loaded) {
        return LIBOFD_ERR_NOT_FOUND;
    }
    std::string object_xml;
    libofd_status_t status = BuildImageObjectXml(*object, &object_xml);
    if (status != LIBOFD_OK) {
        return status;
    }
    return handle->document.AddBlockObjectXml(page_index, block_index, object_xml);
}

libofd_status_t libofd_get_path_object(
    const libofd_handle_t* handle, size_t page_index, size_t block_index, size_t object_index, libofd_path_object_t* out_object) {
    if (handle == nullptr || out_object == nullptr) {
        return LIBOFD_ERR_INVALID_ARGUMENT;
    }
    if (!handle->loaded) {
        return LIBOFD_ERR_NOT_FOUND;
    }
    std::string object_xml;
    libofd_status_t status = handle->document.GetBlockObjectXml(page_index, block_index, object_index, &object_xml);
    if (status != LIBOFD_OK) {
        return status;
    }
    return ParsePathObjectXml(object_xml, out_object);
}

libofd_status_t libofd_get_path_object_by_id(
    const libofd_handle_t* handle, size_t page_index, size_t block_index, const char* object_id, libofd_path_object_t* out_object) {
    if (handle == nullptr || out_object == nullptr || object_id == nullptr || object_id[0] == '\0') {
        return LIBOFD_ERR_INVALID_ARGUMENT;
    }
    size_t index = 0;
    libofd_status_t status = libofd_get_block_object_index_by_id(handle, page_index, block_index, object_id, &index);
    if (status != LIBOFD_OK) {
        return status;
    }
    return libofd_get_path_object(handle, page_index, block_index, index, out_object);
}

libofd_status_t libofd_set_path_object(
    libofd_handle_t* handle, size_t page_index, size_t block_index, size_t object_index, const libofd_path_object_t* object) {
    if (handle == nullptr || object == nullptr) {
        return LIBOFD_ERR_INVALID_ARGUMENT;
    }
    if (!handle->loaded) {
        return LIBOFD_ERR_NOT_FOUND;
    }
    std::string object_xml;
    libofd_status_t status = BuildPathObjectXml(*object, &object_xml);
    if (status != LIBOFD_OK) {
        return status;
    }
    return handle->document.SetBlockObjectXml(page_index, block_index, object_index, object_xml);
}

libofd_status_t libofd_set_path_object_by_id(
    libofd_handle_t* handle, size_t page_index, size_t block_index, const char* object_id, const libofd_path_object_t* object) {
    if (handle == nullptr || object == nullptr || object_id == nullptr || object_id[0] == '\0') {
        return LIBOFD_ERR_INVALID_ARGUMENT;
    }
    size_t index = 0;
    libofd_status_t status = libofd_get_block_object_index_by_id(handle, page_index, block_index, object_id, &index);
    if (status != LIBOFD_OK) {
        return status;
    }
    return libofd_set_path_object(handle, page_index, block_index, index, object);
}

libofd_status_t libofd_add_path_object(
    libofd_handle_t* handle, size_t page_index, size_t block_index, const libofd_path_object_t* object) {
    if (handle == nullptr || object == nullptr) {
        return LIBOFD_ERR_INVALID_ARGUMENT;
    }
    if (!handle->loaded) {
        return LIBOFD_ERR_NOT_FOUND;
    }
    std::string object_xml;
    libofd_status_t status = BuildPathObjectXml(*object, &object_xml);
    if (status != LIBOFD_OK) {
        return status;
    }
    return handle->document.AddBlockObjectXml(page_index, block_index, object_xml);
}

libofd_status_t libofd_get_bookmark_count(const libofd_handle_t* handle, size_t* out_count) {
    if (handle == nullptr || out_count == nullptr) {
        return LIBOFD_ERR_INVALID_ARGUMENT;
    }
    if (!handle->loaded) {
        return LIBOFD_ERR_NOT_FOUND;
    }
    std::string outline_xml;
    libofd_status_t status = handle->document.GetOutlineXml(&outline_xml);
    if (status != LIBOFD_OK) {
        return status;
    }
    *out_count = ParseBookmarkEntries(outline_xml).size();
    return LIBOFD_OK;
}

libofd_status_t libofd_get_bookmark(
    const libofd_handle_t* handle, size_t bookmark_index, libofd_bookmark_t* out_bookmark) {
    if (handle == nullptr || out_bookmark == nullptr) {
        return LIBOFD_ERR_INVALID_ARGUMENT;
    }
    if (!handle->loaded) {
        return LIBOFD_ERR_NOT_FOUND;
    }
    std::string outline_xml;
    libofd_status_t status = handle->document.GetOutlineXml(&outline_xml);
    if (status != LIBOFD_OK) {
        return status;
    }
    const auto entries = ParseBookmarkEntries(outline_xml);
    if (bookmark_index >= entries.size()) {
        return LIBOFD_ERR_NOT_FOUND;
    }
    std::memset(out_bookmark, 0, sizeof(*out_bookmark));
    CopyString(out_bookmark->title, sizeof(out_bookmark->title), entries[bookmark_index].title);
    CopyString(out_bookmark->page_id, sizeof(out_bookmark->page_id), entries[bookmark_index].page_id);
    return LIBOFD_OK;
}

libofd_status_t libofd_add_bookmark(libofd_handle_t* handle, const libofd_bookmark_t* bookmark) {
    if (handle == nullptr || bookmark == nullptr || bookmark->title[0] == '\0') {
        return LIBOFD_ERR_INVALID_ARGUMENT;
    }
    if (!handle->loaded) {
        return LIBOFD_ERR_NOT_FOUND;
    }
    std::string page_id = bookmark->page_id;
    if (page_id.empty()) {
        if (!handle->document.Pages().empty()) {
            page_id = handle->document.Pages().front().page_id;
        } else {
            return LIBOFD_ERR_NOT_FOUND;
        }
    }

    std::string outline_xml;
    libofd_status_t status = handle->document.GetOutlineXml(&outline_xml);
    if (status != LIBOFD_OK) {
        return status;
    }

    std::ostringstream node;
    node << "  <ofd:OutlineElem Title=\"" << EscapeXmlText(bookmark->title) << "\" PageID=\"" << EscapeXmlText(page_id)
         << "\"/>\n";
    if (outline_xml.empty()) {
        outline_xml = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                      "<ofd:Outlines xmlns:ofd=\"http://www.ofdspec.org/2016\">\n" +
                      node.str() + "</ofd:Outlines>\n";
    } else {
        size_t self_close = outline_xml.find("<ofd:Outlines");
        if (self_close != std::string::npos) {
            size_t self_end = outline_xml.find("/>", self_close);
            size_t tag_end = outline_xml.find('>', self_close);
            if (self_end != std::string::npos && (tag_end == std::string::npos || self_end < tag_end)) {
                const std::string open_tag = outline_xml.substr(self_close, self_end - self_close) + ">";
                const std::string prefix = outline_xml.substr(0, self_close);
                const std::string suffix = outline_xml.substr(self_end + 2);
                outline_xml = prefix + open_tag + "\n" + node.str() + "</ofd:Outlines>\n" + suffix;
                return handle->document.SetOutlineXml(outline_xml);
            }
        }
        size_t pos = outline_xml.rfind("</ofd:Outlines>");
        size_t pos_plain = outline_xml.rfind("</Outlines>");
        if (pos == std::string::npos || (pos_plain != std::string::npos && pos_plain > pos)) {
            pos = pos_plain;
        }
        if (pos == std::string::npos) {
            return LIBOFD_ERR_PARSE;
        }
        outline_xml.insert(pos, node.str());
    }
    return handle->document.SetOutlineXml(outline_xml);
}

libofd_status_t libofd_set_common_data_xml(libofd_handle_t* handle, const char* common_data_xml) {
    if (handle == nullptr || common_data_xml == nullptr || common_data_xml[0] == '\0') {
        return LIBOFD_ERR_INVALID_ARGUMENT;
    }
    if (!handle->loaded) {
        return LIBOFD_ERR_NOT_FOUND;
    }
    return handle->document.SetCommonDataXml(common_data_xml);
}

libofd_status_t libofd_get_common_data_xml(
    const libofd_handle_t* handle, char* out_common_data_xml, size_t out_common_data_xml_capacity) {
    if (handle == nullptr || out_common_data_xml == nullptr || out_common_data_xml_capacity == 0U) {
        return LIBOFD_ERR_INVALID_ARGUMENT;
    }
    if (!handle->loaded) {
        return LIBOFD_ERR_NOT_FOUND;
    }
    std::string common_data_xml;
    libofd_status_t status = handle->document.GetCommonDataXml(&common_data_xml);
    if (status != LIBOFD_OK) {
        return status;
    }
    CopyString(out_common_data_xml, out_common_data_xml_capacity, common_data_xml);
    return LIBOFD_OK;
}

libofd_status_t libofd_set_outline_xml(libofd_handle_t* handle, const char* outline_xml) {
    if (handle == nullptr || outline_xml == nullptr || outline_xml[0] == '\0') {
        return LIBOFD_ERR_INVALID_ARGUMENT;
    }
    if (!handle->loaded) {
        return LIBOFD_ERR_NOT_FOUND;
    }
    return handle->document.SetOutlineXml(outline_xml);
}

libofd_status_t libofd_get_outline_xml(
    const libofd_handle_t* handle, char* out_outline_xml, size_t out_outline_xml_capacity) {
    if (handle == nullptr || out_outline_xml == nullptr || out_outline_xml_capacity == 0U) {
        return LIBOFD_ERR_INVALID_ARGUMENT;
    }
    if (!handle->loaded) {
        return LIBOFD_ERR_NOT_FOUND;
    }
    std::string outline_xml;
    libofd_status_t status = handle->document.GetOutlineXml(&outline_xml);
    if (status != LIBOFD_OK) {
        return status;
    }
    CopyString(out_outline_xml, out_outline_xml_capacity, outline_xml);
    return LIBOFD_OK;
}

libofd_status_t libofd_set_permissions_xml(libofd_handle_t* handle, const char* permissions_xml) {
    if (handle == nullptr || permissions_xml == nullptr || permissions_xml[0] == '\0') {
        return LIBOFD_ERR_INVALID_ARGUMENT;
    }
    if (!handle->loaded) {
        return LIBOFD_ERR_NOT_FOUND;
    }
    return handle->document.SetPermissionsXml(permissions_xml);
}

libofd_status_t libofd_get_permissions_xml(
    const libofd_handle_t* handle, char* out_permissions_xml, size_t out_permissions_xml_capacity) {
    if (handle == nullptr || out_permissions_xml == nullptr || out_permissions_xml_capacity == 0U) {
        return LIBOFD_ERR_INVALID_ARGUMENT;
    }
    if (!handle->loaded) {
        return LIBOFD_ERR_NOT_FOUND;
    }
    std::string permissions_xml;
    libofd_status_t status = handle->document.GetPermissionsXml(&permissions_xml);
    if (status != LIBOFD_OK) {
        return status;
    }
    CopyString(out_permissions_xml, out_permissions_xml_capacity, permissions_xml);
    return LIBOFD_OK;
}

libofd_status_t libofd_set_form_xml(libofd_handle_t* handle, const char* form_xml) {
    if (handle == nullptr || form_xml == nullptr || form_xml[0] == '\0') {
        return LIBOFD_ERR_INVALID_ARGUMENT;
    }
    if (!handle->loaded) {
        return LIBOFD_ERR_NOT_FOUND;
    }
    return handle->document.SetFormXml(form_xml);
}

libofd_status_t libofd_get_form_xml(const libofd_handle_t* handle, char* out_form_xml, size_t out_form_xml_capacity) {
    if (handle == nullptr || out_form_xml == nullptr || out_form_xml_capacity == 0U) {
        return LIBOFD_ERR_INVALID_ARGUMENT;
    }
    if (!handle->loaded) {
        return LIBOFD_ERR_NOT_FOUND;
    }
    std::string form_xml;
    libofd_status_t status = handle->document.GetFormXml(&form_xml);
    if (status != LIBOFD_OK) {
        return status;
    }
    CopyString(out_form_xml, out_form_xml_capacity, form_xml);
    return LIBOFD_OK;
}

libofd_status_t libofd_set_page_annotations_xml(libofd_handle_t* handle, size_t page_index, const char* annotations_xml) {
    if (handle == nullptr || annotations_xml == nullptr || annotations_xml[0] == '\0') {
        return LIBOFD_ERR_INVALID_ARGUMENT;
    }
    if (!handle->loaded) {
        return LIBOFD_ERR_NOT_FOUND;
    }
    return handle->document.SetPageAnnotationsXml(page_index, annotations_xml);
}

libofd_status_t libofd_get_page_annotations_xml(
    const libofd_handle_t* handle, size_t page_index, char* out_annotations_xml, size_t out_annotations_xml_capacity) {
    if (handle == nullptr || out_annotations_xml == nullptr || out_annotations_xml_capacity == 0U) {
        return LIBOFD_ERR_INVALID_ARGUMENT;
    }
    if (!handle->loaded) {
        return LIBOFD_ERR_NOT_FOUND;
    }
    std::string annotations_xml;
    libofd_status_t status = handle->document.GetPageAnnotationsXml(page_index, &annotations_xml);
    if (status != LIBOFD_OK) {
        return status;
    }
    CopyString(out_annotations_xml, out_annotations_xml_capacity, annotations_xml);
    return LIBOFD_OK;
}

libofd_status_t libofd_set_page_actions_xml(libofd_handle_t* handle, size_t page_index, const char* actions_xml) {
    if (handle == nullptr || actions_xml == nullptr || actions_xml[0] == '\0') {
        return LIBOFD_ERR_INVALID_ARGUMENT;
    }
    if (!handle->loaded) {
        return LIBOFD_ERR_NOT_FOUND;
    }
    return handle->document.SetPageActionsXml(page_index, actions_xml);
}

libofd_status_t libofd_get_page_actions_xml(
    const libofd_handle_t* handle, size_t page_index, char* out_actions_xml, size_t out_actions_xml_capacity) {
    if (handle == nullptr || out_actions_xml == nullptr || out_actions_xml_capacity == 0U) {
        return LIBOFD_ERR_INVALID_ARGUMENT;
    }
    if (!handle->loaded) {
        return LIBOFD_ERR_NOT_FOUND;
    }
    std::string actions_xml;
    libofd_status_t status = handle->document.GetPageActionsXml(page_index, &actions_xml);
    if (status != LIBOFD_OK) {
        return status;
    }
    CopyString(out_actions_xml, out_actions_xml_capacity, actions_xml);
    return LIBOFD_OK;
}

libofd_status_t libofd_export_to_text(const libofd_handle_t* handle, const char* output_text_file) {
    if (handle == nullptr || output_text_file == nullptr || output_text_file[0] == '\0') {
        return LIBOFD_ERR_INVALID_ARGUMENT;
    }
    if (!handle->loaded) {
        return LIBOFD_ERR_NOT_FOUND;
    }
    return handle->document.ExportToText(output_text_file);
}

libofd_status_t libofd_import_from_text(
    libofd_handle_t* handle, const char* input_text_file, const char* doc_id, const char* creator) {
    if (handle == nullptr || input_text_file == nullptr || input_text_file[0] == '\0') {
        return LIBOFD_ERR_INVALID_ARGUMENT;
    }
    libofd_status_t status = handle->document.ImportFromText(
        input_text_file, doc_id == nullptr ? "" : doc_id, creator == nullptr ? "" : creator);
    if (status != LIBOFD_OK) {
        handle->loaded = false;
        return status;
    }
    handle->loaded = true;
    return LIBOFD_OK;
}

libofd_status_t libofd_set_sign_backend(libofd_handle_t* handle, libofd_sign_backend_t backend) {
    if (handle == nullptr) {
        return LIBOFD_ERR_INVALID_ARGUMENT;
    }
    switch (backend) {
        case LIBOFD_SIGN_BACKEND_OPENSSL:
            return handle->document.SetSignBackend(libofd::SignBackend::kOpenSSL);
        case LIBOFD_SIGN_BACKEND_TASSL:
            return handle->document.SetSignBackend(libofd::SignBackend::kTaSSL);
        default:
            return LIBOFD_ERR_INVALID_ARGUMENT;
    }
}

libofd_status_t libofd_set_pdf_to_ofd_mode(libofd_handle_t* handle, libofd_pdf_to_ofd_mode_t mode) {
    if (handle == nullptr) {
        return LIBOFD_ERR_INVALID_ARGUMENT;
    }
    switch (mode) {
        case LIBOFD_PDF_TO_OFD_MODE_AUTO:
        case LIBOFD_PDF_TO_OFD_MODE_STRUCTURED:
        case LIBOFD_PDF_TO_OFD_MODE_VISUAL_RASTER:
            handle->pdf_to_ofd_mode = mode;
            return LIBOFD_OK;
        default:
            return LIBOFD_ERR_INVALID_ARGUMENT;
    }
}

libofd_status_t libofd_get_pdf_to_ofd_mode(const libofd_handle_t* handle, libofd_pdf_to_ofd_mode_t* out_mode) {
    if (handle == nullptr || out_mode == nullptr) {
        return LIBOFD_ERR_INVALID_ARGUMENT;
    }
    *out_mode = handle->pdf_to_ofd_mode;
    return LIBOFD_OK;
}

libofd_status_t libofd_set_tassl_root(libofd_handle_t* handle, const char* tassl_root) {
    if (handle == nullptr || tassl_root == nullptr || tassl_root[0] == '\0') {
        return LIBOFD_ERR_INVALID_ARGUMENT;
    }
    return handle->document.SetTaSSLRoot(tassl_root);
}

libofd_status_t libofd_set_external_sign_provider(
    libofd_handle_t* handle, const libofd_external_sign_provider_t* provider) {
    if (handle == nullptr || provider == nullptr || provider->sign_fn == nullptr || provider->verify_fn == nullptr) {
        return LIBOFD_ERR_INVALID_ARGUMENT;
    }
    handle->external_provider = *provider;
    handle->external_provider_enabled = true;
    return LIBOFD_OK;
}

libofd_status_t libofd_clear_external_sign_provider(libofd_handle_t* handle) {
    if (handle == nullptr) {
        return LIBOFD_ERR_INVALID_ARGUMENT;
    }
    handle->external_provider_enabled = false;
    std::memset(&handle->external_provider, 0, sizeof(handle->external_provider));
    return LIBOFD_OK;
}

libofd_status_t libofd_set_external_convert_provider(
    libofd_handle_t* handle, const libofd_external_convert_provider_t* provider) {
    if (handle == nullptr || provider == nullptr || provider->ofd_to_pdf_fn == nullptr || provider->pdf_to_ofd_fn == nullptr) {
        return LIBOFD_ERR_INVALID_ARGUMENT;
    }
    handle->external_convert_provider = *provider;
    handle->external_convert_provider_enabled = true;
    return LIBOFD_OK;
}

libofd_status_t libofd_clear_external_convert_provider(libofd_handle_t* handle) {
    if (handle == nullptr) {
        return LIBOFD_ERR_INVALID_ARGUMENT;
    }
    handle->external_convert_provider_enabled = false;
    std::memset(&handle->external_convert_provider, 0, sizeof(handle->external_convert_provider));
    return LIBOFD_OK;
}

libofd_status_t libofd_set_external_image_decode_provider(
    libofd_handle_t* handle, const libofd_external_image_decode_provider_t* provider) {
    if (handle == nullptr || provider == nullptr || provider->decode_fn == nullptr) {
        return LIBOFD_ERR_INVALID_ARGUMENT;
    }
    handle->external_image_decode_provider = *provider;
    handle->external_image_decode_provider_enabled = true;
    return LIBOFD_OK;
}

libofd_status_t libofd_clear_external_image_decode_provider(libofd_handle_t* handle) {
    if (handle == nullptr) {
        return LIBOFD_ERR_INVALID_ARGUMENT;
    }
    handle->external_image_decode_provider_enabled = false;
    std::memset(&handle->external_image_decode_provider, 0, sizeof(handle->external_image_decode_provider));
    return LIBOFD_OK;
}

libofd_status_t libofd_sign_with_private_key(libofd_handle_t* handle, const char* private_key_pem_path) {
    if (handle == nullptr || private_key_pem_path == nullptr || private_key_pem_path[0] == '\0') {
        return LIBOFD_ERR_INVALID_ARGUMENT;
    }
    if (!handle->loaded) {
        return LIBOFD_ERR_NOT_FOUND;
    }
    if (handle->external_provider_enabled) {
        std::string payload;
        libofd_status_t status = handle->document.BuildSignPayload(&payload);
        if (status != LIBOFD_OK) {
            return status;
        }
        size_t signature_len = 0;
        status = handle->external_provider.sign_fn(
            reinterpret_cast<const unsigned char*>(payload.data()), payload.size(), private_key_pem_path, nullptr,
            &signature_len, handle->external_provider.user_data);
        if (status != LIBOFD_OK || signature_len == 0U) {
            return status == LIBOFD_OK ? LIBOFD_ERR_IO : status;
        }
        std::vector<unsigned char> signature(signature_len);
        status = handle->external_provider.sign_fn(
            reinterpret_cast<const unsigned char*>(payload.data()), payload.size(), private_key_pem_path,
            signature.data(), &signature_len, handle->external_provider.user_data);
        if (status != LIBOFD_OK) {
            return status;
        }
        signature.resize(signature_len);
        return handle->document.SetSignatureBlob(signature);
    }
    return handle->document.SignWithPrivateKeyPem(private_key_pem_path);
}

libofd_status_t libofd_verify_signatures(
    const libofd_handle_t* handle, const char* public_key_pem_path, libofd_signature_verify_result_t* out_result) {
    if (handle == nullptr || public_key_pem_path == nullptr || public_key_pem_path[0] == '\0' || out_result == nullptr) {
        return LIBOFD_ERR_INVALID_ARGUMENT;
    }
    if (!handle->loaded) {
        return LIBOFD_ERR_NOT_FOUND;
    }
    if (handle->external_provider_enabled) {
        std::string payload;
        libofd_status_t status = handle->document.BuildSignPayload(&payload);
        if (status != LIBOFD_OK) {
            return status;
        }
        const auto& signature = handle->document.SignatureBlob();
        if (signature.empty()) {
            return LIBOFD_ERR_NOT_FOUND;
        }
        int verified = 0;
        status = handle->external_provider.verify_fn(
            reinterpret_cast<const unsigned char*>(payload.data()), payload.size(), public_key_pem_path, signature.data(),
            signature.size(), &verified, handle->external_provider.user_data);
        if (status != LIBOFD_OK) {
            return status;
        }
        out_result->declared_signature_count = 1;
        out_result->verified_signature_count = verified ? 1U : 0U;
        out_result->all_valid = verified ? 1 : 0;
        return LIBOFD_OK;
    }

    libofd::SignatureVerifyResult result;
    libofd::SignatureVerifier verifier;
    libofd_status_t status = verifier.Verify(handle->document, public_key_pem_path, &result);

    out_result->declared_signature_count = result.declared_signature_count;
    out_result->verified_signature_count = result.verified_signature_count;
    out_result->all_valid = result.all_valid ? 1 : 0;
    return status;
}

libofd_status_t libofd_convert_ofd_to_pdf(
    libofd_handle_t* handle, const char* input_ofd_path, const char* output_pdf_path) {
    if (handle == nullptr || input_ofd_path == nullptr || output_pdf_path == nullptr || input_ofd_path[0] == '\0' ||
        output_pdf_path[0] == '\0') {
        return LIBOFD_ERR_INVALID_ARGUMENT;
    }
    if (handle->external_convert_provider_enabled) {
        return handle->external_convert_provider.ofd_to_pdf_fn(
            input_ofd_path, output_pdf_path, handle->external_convert_provider.user_data);
    }
    libofd::pdf_engine::ConvertOptions options;
    if (handle->external_image_decode_provider_enabled) {
        options.external_image_decode_fn = handle->external_image_decode_provider.decode_fn;
        options.external_image_decode_user_data = handle->external_image_decode_provider.user_data;
    }
    return libofd::pdf_engine::ConvertOfdToPdf(input_ofd_path, output_pdf_path, &options);
}

libofd_status_t libofd_convert_pdf_to_ofd(
    libofd_handle_t* handle, const char* input_pdf_path, const char* output_ofd_path) {
    if (handle == nullptr || input_pdf_path == nullptr || output_ofd_path == nullptr || input_pdf_path[0] == '\0' ||
        output_ofd_path[0] == '\0') {
        return LIBOFD_ERR_INVALID_ARGUMENT;
    }
    if (handle->external_convert_provider_enabled) {
        return handle->external_convert_provider.pdf_to_ofd_fn(
            input_pdf_path, output_ofd_path, handle->external_convert_provider.user_data);
    }
    libofd::pdf_engine::PdfToOfdOptions options;
    switch (handle->pdf_to_ofd_mode) {
        case LIBOFD_PDF_TO_OFD_MODE_STRUCTURED:
            options.mode = libofd::pdf_engine::PdfToOfdMode::kStructured;
            break;
        case LIBOFD_PDF_TO_OFD_MODE_VISUAL_RASTER:
            options.mode = libofd::pdf_engine::PdfToOfdMode::kVisualRaster;
            break;
        case LIBOFD_PDF_TO_OFD_MODE_AUTO:
        default:
            options.mode = libofd::pdf_engine::PdfToOfdMode::kAuto;
            break;
    }
    return libofd::pdf_engine::ConvertPdfToOfd(input_pdf_path, output_ofd_path, &options);
}

const char* libofd_version(void) {
#ifdef LIBOFD_VERSION
    return LIBOFD_VERSION;
#else
    return "dev";
#endif
}

} // extern "C"

