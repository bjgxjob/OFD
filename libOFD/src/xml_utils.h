#ifndef LIBOFD_XML_UTILS_H
#define LIBOFD_XML_UTILS_H

#include <string>
#include "libofd/status.h"

namespace libofd::xml_utils {

libofd_status_t ExtractTagText(const std::string& xml, const std::string& tag, std::string* out_text);
libofd_status_t ExtractTagAttribute(
    const std::string& xml, const std::string& tag, const std::string& attribute, std::string* out_value);
std::string Trim(const std::string& value);

} // namespace libofd::xml_utils

#endif

