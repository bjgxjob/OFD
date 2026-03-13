#include "xml_utils.h"

#include <algorithm>
#include <cctype>

namespace libofd::xml_utils {

std::string Trim(const std::string& value) {
    auto begin = value.begin();
    while (begin != value.end() && std::isspace(static_cast<unsigned char>(*begin))) {
        ++begin;
    }

    auto end = value.end();
    while (end != begin && std::isspace(static_cast<unsigned char>(*(end - 1)))) {
        --end;
    }
    return std::string(begin, end);
}

static bool FindOpenTag(const std::string& xml, const std::string& tag, size_t* out_open_pos, size_t* out_open_end) {
    const std::string plain = "<" + tag;
    const std::string ns = "<ofd:" + tag;

    size_t open_pos = xml.find(plain);
    if (open_pos == std::string::npos) {
        open_pos = xml.find(ns);
    }
    if (open_pos == std::string::npos) {
        return false;
    }

    const size_t open_end = xml.find('>', open_pos);
    if (open_end == std::string::npos) {
        return false;
    }
    *out_open_pos = open_pos;
    *out_open_end = open_end;
    return true;
}

libofd_status_t ExtractTagText(const std::string& xml, const std::string& tag, std::string* out_text) {
    if (out_text == nullptr) {
        return LIBOFD_ERR_INVALID_ARGUMENT;
    }

    size_t open_pos = 0;
    size_t open_end = 0;
    if (!FindOpenTag(xml, tag, &open_pos, &open_end)) {
        return LIBOFD_ERR_NOT_FOUND;
    }

    const std::string close_plain = "</" + tag + ">";
    const std::string close_ns = "</ofd:" + tag + ">";
    size_t close_pos = xml.find(close_plain, open_end + 1);
    if (close_pos == std::string::npos) {
        close_pos = xml.find(close_ns, open_end + 1);
    }
    if (close_pos == std::string::npos || close_pos <= open_end) {
        return LIBOFD_ERR_PARSE;
    }

    *out_text = Trim(xml.substr(open_end + 1, close_pos - open_end - 1));
    return LIBOFD_OK;
}

libofd_status_t ExtractTagAttribute(
    const std::string& xml, const std::string& tag, const std::string& attribute, std::string* out_value) {
    if (out_value == nullptr) {
        return LIBOFD_ERR_INVALID_ARGUMENT;
    }

    size_t open_pos = 0;
    size_t open_end = 0;
    if (!FindOpenTag(xml, tag, &open_pos, &open_end)) {
        return LIBOFD_ERR_NOT_FOUND;
    }

    const std::string open_chunk = xml.substr(open_pos, open_end - open_pos + 1);
    const std::string key = attribute + "=\"";
    size_t key_pos = open_chunk.find(key);
    if (key_pos == std::string::npos) {
        return LIBOFD_ERR_NOT_FOUND;
    }
    key_pos += key.size();
    size_t quote_end = open_chunk.find('"', key_pos);
    if (quote_end == std::string::npos) {
        return LIBOFD_ERR_PARSE;
    }

    *out_value = open_chunk.substr(key_pos, quote_end - key_pos);
    return LIBOFD_OK;
}

} // namespace libofd::xml_utils

