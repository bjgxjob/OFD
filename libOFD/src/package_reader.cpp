#include "libofd/package_reader.h"

#include <filesystem>
#include <fstream>
#include <sstream>

#include "xml_utils.h"

namespace fs = std::filesystem;

namespace libofd {

static fs::path NormalizeOfdPath(const std::string& raw_path) {
    std::string v = raw_path;
    while (!v.empty() && (v.front() == '/' || v.front() == '\\')) {
        v.erase(v.begin());
    }
    return fs::path(v).lexically_normal();
}

static libofd_status_t ReadWholeFile(const fs::path& path, std::string* out_content) {
    if (out_content == nullptr) {
        return LIBOFD_ERR_INVALID_ARGUMENT;
    }

    std::ifstream in(path, std::ios::in | std::ios::binary);
    if (!in.is_open()) {
        return LIBOFD_ERR_IO;
    }

    std::ostringstream ss;
    ss << in.rdbuf();
    *out_content = ss.str();
    return LIBOFD_OK;
}

libofd_status_t PackageReader::ReadExplodedPackage(
    const std::string& package_root, ParsedOfdPackage* out_package) const {
    if (out_package == nullptr || package_root.empty()) {
        return LIBOFD_ERR_INVALID_ARGUMENT;
    }

    const fs::path root_path(package_root);
    if (!fs::exists(root_path) || !fs::is_directory(root_path)) {
        return LIBOFD_ERR_NOT_FOUND;
    }

    const fs::path ofd_xml = root_path / "OFD.xml";
    libofd_status_t status = ReadWholeFile(ofd_xml, &out_package->ofd_xml_content);
    if (status != LIBOFD_OK) {
        return status;
    }

    std::string doc_root;
    status = xml_utils::ExtractTagText(out_package->ofd_xml_content, "DocRoot", &doc_root);
    if (status != LIBOFD_OK) {
        return status;
    }
    out_package->doc_root_relative_path = NormalizeOfdPath(doc_root).string();

    const fs::path doc_xml = root_path / NormalizeOfdPath(doc_root);
    status = ReadWholeFile(doc_xml, &out_package->document_xml_content);
    if (status != LIBOFD_OK) {
        return status;
    }

    return LIBOFD_OK;
}

} // namespace libofd

