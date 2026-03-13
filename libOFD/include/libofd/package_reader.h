#ifndef LIBOFD_PACKAGE_READER_H
#define LIBOFD_PACKAGE_READER_H

#include <string>
#include "libofd/status.h"

namespace libofd {

struct ParsedOfdPackage {
    std::string ofd_xml_content;
    std::string document_xml_content;
    std::string doc_root_relative_path;
};

class PackageReader {
public:
    libofd_status_t ReadExplodedPackage(const std::string& package_root, ParsedOfdPackage* out_package) const;
};

} // namespace libofd

#endif

