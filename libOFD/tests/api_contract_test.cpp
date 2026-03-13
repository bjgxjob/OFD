#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>

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
    const std::string src_dir = LIBOFD_SOURCE_DIR;
    const std::string exploded_path = src_dir + "/tests/data";
    const std::string fake_ofd_path = src_dir + "/tests/data/sample.ofd";

    std::ofstream out(fake_ofd_path, std::ios::binary);
    out << "not a real ofd zip";
    out.close();

    libofd_status_t status = libofd_load_path(handle, exploded_path.c_str());
    if (status != LIBOFD_OK) {
        std::cerr << "load path (directory) failed: " << libofd_status_message(status) << "\n";
        libofd_destroy(handle);
        return EXIT_FAILURE;
    }

    libofd_signature_verify_result_t sig_result{};
    status = libofd_verify_signatures(handle, "", &sig_result);
    if (status != LIBOFD_ERR_INVALID_ARGUMENT) {
        std::cerr << "verify_signatures should validate key path argument\n";
        libofd_destroy(handle);
        return EXIT_FAILURE;
    }

    libofd_pdf_to_ofd_mode_t mode = LIBOFD_PDF_TO_OFD_MODE_STRUCTURED;
    status = libofd_get_pdf_to_ofd_mode(handle, &mode);
    if (status != LIBOFD_OK || mode != LIBOFD_PDF_TO_OFD_MODE_AUTO) {
        std::cerr << "default pdf_to_ofd mode should be AUTO\n";
        libofd_destroy(handle);
        return EXIT_FAILURE;
    }
    status = libofd_set_pdf_to_ofd_mode(handle, LIBOFD_PDF_TO_OFD_MODE_VISUAL_RASTER);
    if (status != LIBOFD_OK) {
        std::cerr << "set pdf_to_ofd mode failed\n";
        libofd_destroy(handle);
        return EXIT_FAILURE;
    }
    status = libofd_get_pdf_to_ofd_mode(handle, &mode);
    if (status != LIBOFD_OK || mode != LIBOFD_PDF_TO_OFD_MODE_VISUAL_RASTER) {
        std::cerr << "get pdf_to_ofd mode should return VISUAL_RASTER\n";
        libofd_destroy(handle);
        return EXIT_FAILURE;
    }
    status = libofd_set_pdf_to_ofd_mode(handle, static_cast<libofd_pdf_to_ofd_mode_t>(-1));
    if (status != LIBOFD_ERR_INVALID_ARGUMENT) {
        std::cerr << "invalid pdf_to_ofd mode should be rejected\n";
        libofd_destroy(handle);
        return EXIT_FAILURE;
    }

    status = libofd_create_empty(handle, "api-doc", "api-test");
    if (status != LIBOFD_OK) {
        std::cerr << "create empty failed\n";
        libofd_destroy(handle);
        return EXIT_FAILURE;
    }
    status = libofd_add_page_text(handle, "hello");
    if (status != LIBOFD_OK) {
        std::cerr << "add page failed\n";
        libofd_destroy(handle);
        return EXIT_FAILURE;
    }
    const char* layer_xml = "<ofd:Layer Type=\"Body\" ID=\"9\"><ofd:TextObject ID=\"10\" Font=\"1\" Size=\"3.5\" Boundary=\"10 10 20 5\"><ofd:TextCode X=\"0\" Y=\"3.5\">abc</ofd:TextCode></ofd:TextObject></ofd:Layer>";
    status = libofd_set_page_content_block_xml(handle, 0, 0, layer_xml);
    if (status != LIBOFD_OK) {
        std::cerr << "set page content block failed\n";
        libofd_destroy(handle);
        return EXIT_FAILURE;
    }
    size_t block_count = 0;
    status = libofd_get_page_content_block_count(handle, 0, &block_count);
    if (status != LIBOFD_OK || block_count == 0U) {
        std::cerr << "get page content block count failed\n";
        libofd_destroy(handle);
        return EXIT_FAILURE;
    }
    char block_buf[1024] = {0};
    status = libofd_get_page_content_block_xml(handle, 0, 0, block_buf, sizeof(block_buf));
    if (status != LIBOFD_OK || std::string(block_buf).find("<ofd:Layer") == std::string::npos) {
        std::cerr << "get page content block xml failed\n";
        libofd_destroy(handle);
        return EXIT_FAILURE;
    }
    size_t object_count = 0;
    status = libofd_get_block_object_count(handle, 0, 0, &object_count);
    if (status != LIBOFD_OK || object_count == 0U) {
        std::cerr << "get block object count failed\n";
        libofd_destroy(handle);
        return EXIT_FAILURE;
    }
    char obj_buf[1024] = {0};
    status = libofd_get_block_object_xml(handle, 0, 0, 0, obj_buf, sizeof(obj_buf));
    if (status != LIBOFD_OK || std::string(obj_buf).find("<ofd:TextObject") == std::string::npos) {
        std::cerr << "get block object xml failed\n";
        libofd_destroy(handle);
        return EXIT_FAILURE;
    }
    status = libofd_set_block_object_xml(
        handle, 0, 0, 0,
        "<ofd:TextObject ID=\"11\" Font=\"1\" Size=\"3.5\" Boundary=\"10 20 20 5\"><ofd:TextCode X=\"0\" Y=\"3.5\">xyz</ofd:TextCode></ofd:TextObject>");
    if (status != LIBOFD_OK) {
        std::cerr << "set block object xml failed\n";
        libofd_destroy(handle);
        return EXIT_FAILURE;
    }
    status = libofd_add_block_object_xml(
        handle, 0, 0,
        "<ofd:PathObject ID=\"12\" Boundary=\"10 10 20 20\" AbbreviatedData=\"M 10 10 L 30 30\"/>");
    if (status != LIBOFD_OK) {
        std::cerr << "add block object xml failed\n";
        libofd_destroy(handle);
        return EXIT_FAILURE;
    }
    libofd_text_object_t text_obj{};
    status = libofd_get_text_object(handle, 0, 0, 0, &text_obj);
    if (status != LIBOFD_OK || std::string(text_obj.id).empty()) {
        std::cerr << "get text object failed\n";
        libofd_destroy(handle);
        return EXIT_FAILURE;
    }
    std::strncpy(text_obj.text, "typed-api-text", sizeof(text_obj.text) - 1U);
    status = libofd_set_text_object(handle, 0, 0, 0, &text_obj);
    if (status != LIBOFD_OK) {
        std::cerr << "set text object failed\n";
        libofd_destroy(handle);
        return EXIT_FAILURE;
    }
    libofd_path_object_t path_obj{};
    status = libofd_get_path_object(handle, 0, 0, 1, &path_obj);
    if (status != LIBOFD_OK || std::string(path_obj.id).empty()) {
        std::cerr << "get path object failed\n";
        libofd_destroy(handle);
        return EXIT_FAILURE;
    }
    std::strncpy(path_obj.abbreviated_data, "M 0 0 L 10 10", sizeof(path_obj.abbreviated_data) - 1U);
    status = libofd_set_path_object(handle, 0, 0, 1, &path_obj);
    if (status != LIBOFD_OK) {
        std::cerr << "set path object failed\n";
        libofd_destroy(handle);
        return EXIT_FAILURE;
    }
    libofd_image_object_t image_obj{};
    std::strncpy(image_obj.id, "20", sizeof(image_obj.id) - 1U);
    std::strncpy(image_obj.resource_id, "2", sizeof(image_obj.resource_id) - 1U);
    image_obj.boundary_x = 1.0;
    image_obj.boundary_y = 2.0;
    image_obj.boundary_w = 3.0;
    image_obj.boundary_h = 4.0;
    std::strncpy(image_obj.ctm, "3 0 0 4 1 2", sizeof(image_obj.ctm) - 1U);
    status = libofd_add_image_object(handle, 0, 0, &image_obj);
    if (status != LIBOFD_OK) {
        std::cerr << "add image object failed\n";
        libofd_destroy(handle);
        return EXIT_FAILURE;
    }
    libofd_image_object_t image_obj_out{};
    status = libofd_get_image_object(handle, 0, 0, 2, &image_obj_out);
    if (status != LIBOFD_OK || std::string(image_obj_out.id) != "20") {
        std::cerr << "get image object failed\n";
        libofd_destroy(handle);
        return EXIT_FAILURE;
    }
    size_t object_idx = 0;
    status = libofd_get_block_object_index_by_id(handle, 0, 0, "20", &object_idx);
    if (status != LIBOFD_OK || object_idx != 2U) {
        std::cerr << "get object index by id failed\n";
        libofd_destroy(handle);
        return EXIT_FAILURE;
    }
    status = libofd_get_text_object_by_id(handle, 0, 0, "11", &text_obj);
    if (status != LIBOFD_OK || std::string(text_obj.id) != "11") {
        std::cerr << "get text object by id failed\n";
        libofd_destroy(handle);
        return EXIT_FAILURE;
    }
    std::strncpy(text_obj.text, "typed-by-id", sizeof(text_obj.text) - 1U);
    status = libofd_set_text_object_by_id(handle, 0, 0, "11", &text_obj);
    if (status != LIBOFD_OK) {
        std::cerr << "set text object by id failed\n";
        libofd_destroy(handle);
        return EXIT_FAILURE;
    }
    status = libofd_get_path_object_by_id(handle, 0, 0, "12", &path_obj);
    if (status != LIBOFD_OK || std::string(path_obj.id) != "12") {
        std::cerr << "get path object by id failed\n";
        libofd_destroy(handle);
        return EXIT_FAILURE;
    }
    std::strncpy(path_obj.abbreviated_data, "M 1 1 L 2 2", sizeof(path_obj.abbreviated_data) - 1U);
    status = libofd_set_path_object_by_id(handle, 0, 0, "12", &path_obj);
    if (status != LIBOFD_OK) {
        std::cerr << "set path object by id failed\n";
        libofd_destroy(handle);
        return EXIT_FAILURE;
    }
    status = libofd_get_image_object_by_id(handle, 0, 0, "20", &image_obj_out);
    if (status != LIBOFD_OK || std::string(image_obj_out.resource_id) != "2") {
        std::cerr << "get image object by id failed\n";
        libofd_destroy(handle);
        return EXIT_FAILURE;
    }

    status = libofd_set_outline_xml(handle, "<ofd:Outlines xmlns:ofd=\"http://www.ofdspec.org/2016\"/>");
    if (status != LIBOFD_OK) {
        std::cerr << "set outline xml failed\n";
        libofd_destroy(handle);
        return EXIT_FAILURE;
    }
    status = libofd_set_permissions_xml(handle, "<ofd:Permissions xmlns:ofd=\"http://www.ofdspec.org/2016\"/>");
    if (status != LIBOFD_OK) {
        std::cerr << "set permissions xml failed\n";
        libofd_destroy(handle);
        return EXIT_FAILURE;
    }
    status = libofd_set_form_xml(handle, "<ofd:Form xmlns:ofd=\"http://www.ofdspec.org/2016\"/>");
    if (status != LIBOFD_OK) {
        std::cerr << "set form xml failed\n";
        libofd_destroy(handle);
        return EXIT_FAILURE;
    }
    status = libofd_set_page_annotations_xml(handle, 0, "<ofd:Annotations xmlns:ofd=\"http://www.ofdspec.org/2016\"/>");
    if (status != LIBOFD_OK) {
        std::cerr << "set page annotations xml failed\n";
        libofd_destroy(handle);
        return EXIT_FAILURE;
    }
    status = libofd_set_page_actions_xml(handle, 0, "<ofd:Actions xmlns:ofd=\"http://www.ofdspec.org/2016\"/>");
    if (status != LIBOFD_OK) {
        std::cerr << "set page actions xml failed\n";
        libofd_destroy(handle);
        return EXIT_FAILURE;
    }
    char misc_buf[1024] = {0};
    status = libofd_get_outline_xml(handle, misc_buf, sizeof(misc_buf));
    if (status != LIBOFD_OK || std::string(misc_buf).find("Outlines") == std::string::npos) {
        std::cerr << "get outline xml failed\n";
        libofd_destroy(handle);
        return EXIT_FAILURE;
    }
    libofd_bookmark_t bookmark{};
    std::strncpy(bookmark.title, "bookmark-1", sizeof(bookmark.title) - 1U);
    std::strncpy(bookmark.page_id, "1", sizeof(bookmark.page_id) - 1U);
    status = libofd_add_bookmark(handle, &bookmark);
    if (status != LIBOFD_OK) {
        std::cerr << "add bookmark failed\n";
        libofd_destroy(handle);
        return EXIT_FAILURE;
    }
    size_t bookmark_count = 0;
    status = libofd_get_bookmark_count(handle, &bookmark_count);
    if (status != LIBOFD_OK || bookmark_count == 0U) {
        std::cerr << "get bookmark count failed\n";
        libofd_destroy(handle);
        return EXIT_FAILURE;
    }
    libofd_bookmark_t bookmark_out{};
    status = libofd_get_bookmark(handle, 0, &bookmark_out);
    if (status != LIBOFD_OK || std::string(bookmark_out.title).empty()) {
        std::cerr << "get bookmark failed\n";
        libofd_destroy(handle);
        return EXIT_FAILURE;
    }

    status = libofd_load_path(handle, fake_ofd_path.c_str());
    if (status != LIBOFD_ERR_UNSUPPORTED) {
        std::cerr << "loading .ofd file should be unsupported in current stage\n";
        libofd_destroy(handle);
        return EXIT_FAILURE;
    }

    std::remove(fake_ofd_path.c_str());
    libofd_destroy(handle);
    return EXIT_SUCCESS;
}

