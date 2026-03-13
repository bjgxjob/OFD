#include <cstdlib>
#include <iostream>

#include "libofd/libofd.h"

int main() {
    libofd_handle_t* handle = libofd_create();
    if (handle == nullptr) {
        std::cerr << "failed to create libofd handle\n";
        return EXIT_FAILURE;
    }

#ifndef LIBOFD_SOURCE_DIR
#define LIBOFD_SOURCE_DIR "."
#endif
    const std::string package_path = std::string(LIBOFD_SOURCE_DIR) + "/tests/data";
    libofd_status_t status = libofd_load_exploded_package(handle, package_path.c_str());
    if (status != LIBOFD_OK) {
        std::cerr << "load failed: " << libofd_status_message(status) << "\n";
        libofd_destroy(handle);
        return EXIT_FAILURE;
    }

    libofd_doc_info_t info{};
    status = libofd_get_doc_info(handle, &info);
    if (status != LIBOFD_OK) {
        std::cerr << "get info failed: " << libofd_status_message(status) << "\n";
        libofd_destroy(handle);
        return EXIT_FAILURE;
    }

    std::cout << "libofd version: " << libofd_version() << "\n";
    std::cout << "doc id: " << info.doc_id << "\n";
    std::cout << "creator: " << info.creator << "\n";
    std::cout << "max unit id: " << info.max_unit_id << "\n";

    const size_t page_count = libofd_get_page_count(handle);
    if (page_count != 2U) {
        std::cerr << "unexpected page count: " << page_count << "\n";
        libofd_destroy(handle);
        return EXIT_FAILURE;
    }

    libofd_page_info_t first_page{};
    status = libofd_get_page_info(handle, 0U, &first_page);
    if (status != LIBOFD_OK) {
        std::cerr << "get first page failed: " << libofd_status_message(status) << "\n";
        libofd_destroy(handle);
        return EXIT_FAILURE;
    }
    std::cout << "first page id: " << first_page.page_id << "\n";

    libofd_destroy(handle);
    return EXIT_SUCCESS;
}

