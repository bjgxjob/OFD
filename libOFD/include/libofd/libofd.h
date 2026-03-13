#ifndef LIBOFD_LIBOFD_H
#define LIBOFD_LIBOFD_H

#include <stddef.h>
#include "libofd/status.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct libofd_handle libofd_handle_t;

typedef struct libofd_doc_info {
    char package_root[512];
    char ofd_xml_path[512];
    char document_xml_path[512];
    char creator[256];
    char creation_date[128];
    char doc_id[128];
    char max_unit_id[64];
} libofd_doc_info_t;

typedef struct libofd_page_info {
    char page_id[64];
    char base_loc[512];
} libofd_page_info_t;

typedef struct libofd_signature_verify_result {
    size_t declared_signature_count;
    size_t verified_signature_count;
    int all_valid;
} libofd_signature_verify_result_t;

typedef struct libofd_text_object {
    char id[64];
    char font[64];
    double size;
    double boundary_x;
    double boundary_y;
    double boundary_w;
    double boundary_h;
    char text[1024];
} libofd_text_object_t;

typedef struct libofd_image_object {
    char id[64];
    char resource_id[64];
    double boundary_x;
    double boundary_y;
    double boundary_w;
    double boundary_h;
    char ctm[256];
} libofd_image_object_t;

typedef struct libofd_path_object {
    char id[64];
    double boundary_x;
    double boundary_y;
    double boundary_w;
    double boundary_h;
    char abbreviated_data[2048];
} libofd_path_object_t;

typedef struct libofd_bookmark {
    char title[256];
    char page_id[64];
} libofd_bookmark_t;

typedef enum libofd_sign_backend {
    LIBOFD_SIGN_BACKEND_OPENSSL = 0,
    LIBOFD_SIGN_BACKEND_TASSL = 1
} libofd_sign_backend_t;

typedef enum libofd_pdf_to_ofd_mode {
    LIBOFD_PDF_TO_OFD_MODE_AUTO = 0,
    LIBOFD_PDF_TO_OFD_MODE_STRUCTURED = 1,
    LIBOFD_PDF_TO_OFD_MODE_VISUAL_RASTER = 2
} libofd_pdf_to_ofd_mode_t;

typedef libofd_status_t (*libofd_external_sign_fn)(
    const unsigned char* payload, size_t payload_len, const char* private_key_path,
    unsigned char* out_signature, size_t* inout_signature_len, void* user_data);

typedef libofd_status_t (*libofd_external_verify_fn)(
    const unsigned char* payload, size_t payload_len, const char* public_key_path,
    const unsigned char* signature, size_t signature_len, int* out_verified, void* user_data);

typedef struct libofd_external_sign_provider {
    libofd_external_sign_fn sign_fn;
    libofd_external_verify_fn verify_fn;
    void* user_data;
} libofd_external_sign_provider_t;

typedef libofd_status_t (*libofd_external_ofd_to_pdf_fn)(
    const char* input_ofd_path, const char* output_pdf_path, void* user_data);
typedef libofd_status_t (*libofd_external_pdf_to_ofd_fn)(
    const char* input_pdf_path, const char* output_ofd_path, void* user_data);

typedef struct libofd_external_convert_provider {
    libofd_external_ofd_to_pdf_fn ofd_to_pdf_fn;
    libofd_external_pdf_to_ofd_fn pdf_to_ofd_fn;
    void* user_data;
} libofd_external_convert_provider_t;

typedef libofd_status_t (*libofd_external_image_decode_fn)(
    const char* image_path, int* out_width, int* out_height, int* out_bits_per_component, int* out_color_components,
    unsigned char* out_pixels, size_t* inout_pixels_len, void* user_data);

typedef struct libofd_external_image_decode_provider {
    libofd_external_image_decode_fn decode_fn;
    void* user_data;
} libofd_external_image_decode_provider_t;

libofd_handle_t* libofd_create(void);
void libofd_destroy(libofd_handle_t* handle);

libofd_status_t libofd_create_empty(libofd_handle_t* handle, const char* doc_id, const char* creator);
libofd_status_t libofd_load_path(libofd_handle_t* handle, const char* path);
libofd_status_t libofd_load_exploded_package(libofd_handle_t* handle, const char* package_root);
libofd_status_t libofd_save_exploded_package(const libofd_handle_t* handle, const char* output_root);
libofd_status_t libofd_get_doc_info(const libofd_handle_t* handle, libofd_doc_info_t* out_info);
libofd_status_t libofd_set_creator(libofd_handle_t* handle, const char* creator);
size_t libofd_get_page_count(const libofd_handle_t* handle);
libofd_status_t libofd_get_page_info(const libofd_handle_t* handle, size_t page_index, libofd_page_info_t* out_info);
libofd_status_t libofd_add_page_text(libofd_handle_t* handle, const char* text);
libofd_status_t libofd_set_page_text(libofd_handle_t* handle, size_t page_index, const char* text);
libofd_status_t libofd_get_page_text(
    const libofd_handle_t* handle, size_t page_index, char* out_text, size_t out_text_capacity);
libofd_status_t libofd_set_page_content_xml(libofd_handle_t* handle, size_t page_index, const char* content_xml);
libofd_status_t libofd_get_page_content_xml(
    const libofd_handle_t* handle, size_t page_index, char* out_content_xml, size_t out_content_xml_capacity);
libofd_status_t libofd_get_page_content_block_count(
    const libofd_handle_t* handle, size_t page_index, size_t* out_block_count);
libofd_status_t libofd_get_page_content_block_xml(
    const libofd_handle_t* handle, size_t page_index, size_t block_index, char* out_block_xml, size_t out_block_xml_capacity);
libofd_status_t libofd_set_page_content_block_xml(
    libofd_handle_t* handle, size_t page_index, size_t block_index, const char* block_xml);
libofd_status_t libofd_get_block_object_count(
    const libofd_handle_t* handle, size_t page_index, size_t block_index, size_t* out_object_count);
libofd_status_t libofd_get_block_object_xml(
    const libofd_handle_t* handle, size_t page_index, size_t block_index, size_t object_index,
    char* out_object_xml, size_t out_object_xml_capacity);
libofd_status_t libofd_get_block_object_index_by_id(
    const libofd_handle_t* handle, size_t page_index, size_t block_index, const char* object_id, size_t* out_object_index);
libofd_status_t libofd_set_block_object_xml(
    libofd_handle_t* handle, size_t page_index, size_t block_index, size_t object_index, const char* object_xml);
libofd_status_t libofd_add_block_object_xml(
    libofd_handle_t* handle, size_t page_index, size_t block_index, const char* object_xml);
libofd_status_t libofd_get_text_object(
    const libofd_handle_t* handle, size_t page_index, size_t block_index, size_t object_index, libofd_text_object_t* out_object);
libofd_status_t libofd_get_text_object_by_id(
    const libofd_handle_t* handle, size_t page_index, size_t block_index, const char* object_id, libofd_text_object_t* out_object);
libofd_status_t libofd_set_text_object(
    libofd_handle_t* handle, size_t page_index, size_t block_index, size_t object_index, const libofd_text_object_t* object);
libofd_status_t libofd_set_text_object_by_id(
    libofd_handle_t* handle, size_t page_index, size_t block_index, const char* object_id, const libofd_text_object_t* object);
libofd_status_t libofd_add_text_object(
    libofd_handle_t* handle, size_t page_index, size_t block_index, const libofd_text_object_t* object);
libofd_status_t libofd_get_image_object(
    const libofd_handle_t* handle, size_t page_index, size_t block_index, size_t object_index, libofd_image_object_t* out_object);
libofd_status_t libofd_get_image_object_by_id(
    const libofd_handle_t* handle, size_t page_index, size_t block_index, const char* object_id, libofd_image_object_t* out_object);
libofd_status_t libofd_set_image_object(
    libofd_handle_t* handle, size_t page_index, size_t block_index, size_t object_index, const libofd_image_object_t* object);
libofd_status_t libofd_set_image_object_by_id(
    libofd_handle_t* handle, size_t page_index, size_t block_index, const char* object_id, const libofd_image_object_t* object);
libofd_status_t libofd_add_image_object(
    libofd_handle_t* handle, size_t page_index, size_t block_index, const libofd_image_object_t* object);
libofd_status_t libofd_get_path_object(
    const libofd_handle_t* handle, size_t page_index, size_t block_index, size_t object_index, libofd_path_object_t* out_object);
libofd_status_t libofd_get_path_object_by_id(
    const libofd_handle_t* handle, size_t page_index, size_t block_index, const char* object_id, libofd_path_object_t* out_object);
libofd_status_t libofd_set_path_object(
    libofd_handle_t* handle, size_t page_index, size_t block_index, size_t object_index, const libofd_path_object_t* object);
libofd_status_t libofd_set_path_object_by_id(
    libofd_handle_t* handle, size_t page_index, size_t block_index, const char* object_id, const libofd_path_object_t* object);
libofd_status_t libofd_add_path_object(
    libofd_handle_t* handle, size_t page_index, size_t block_index, const libofd_path_object_t* object);
libofd_status_t libofd_get_bookmark_count(const libofd_handle_t* handle, size_t* out_count);
libofd_status_t libofd_get_bookmark(const libofd_handle_t* handle, size_t bookmark_index, libofd_bookmark_t* out_bookmark);
libofd_status_t libofd_add_bookmark(libofd_handle_t* handle, const libofd_bookmark_t* bookmark);
libofd_status_t libofd_set_common_data_xml(libofd_handle_t* handle, const char* common_data_xml);
libofd_status_t libofd_get_common_data_xml(
    const libofd_handle_t* handle, char* out_common_data_xml, size_t out_common_data_xml_capacity);
libofd_status_t libofd_set_outline_xml(libofd_handle_t* handle, const char* outline_xml);
libofd_status_t libofd_get_outline_xml(
    const libofd_handle_t* handle, char* out_outline_xml, size_t out_outline_xml_capacity);
libofd_status_t libofd_set_permissions_xml(libofd_handle_t* handle, const char* permissions_xml);
libofd_status_t libofd_get_permissions_xml(
    const libofd_handle_t* handle, char* out_permissions_xml, size_t out_permissions_xml_capacity);
libofd_status_t libofd_set_form_xml(libofd_handle_t* handle, const char* form_xml);
libofd_status_t libofd_get_form_xml(const libofd_handle_t* handle, char* out_form_xml, size_t out_form_xml_capacity);
libofd_status_t libofd_set_page_annotations_xml(libofd_handle_t* handle, size_t page_index, const char* annotations_xml);
libofd_status_t libofd_get_page_annotations_xml(
    const libofd_handle_t* handle, size_t page_index, char* out_annotations_xml, size_t out_annotations_xml_capacity);
libofd_status_t libofd_set_page_actions_xml(libofd_handle_t* handle, size_t page_index, const char* actions_xml);
libofd_status_t libofd_get_page_actions_xml(
    const libofd_handle_t* handle, size_t page_index, char* out_actions_xml, size_t out_actions_xml_capacity);
libofd_status_t libofd_export_to_text(const libofd_handle_t* handle, const char* output_text_file);
libofd_status_t libofd_import_from_text(
    libofd_handle_t* handle, const char* input_text_file, const char* doc_id, const char* creator);

libofd_status_t libofd_set_sign_backend(libofd_handle_t* handle, libofd_sign_backend_t backend);
libofd_status_t libofd_set_pdf_to_ofd_mode(libofd_handle_t* handle, libofd_pdf_to_ofd_mode_t mode);
libofd_status_t libofd_get_pdf_to_ofd_mode(const libofd_handle_t* handle, libofd_pdf_to_ofd_mode_t* out_mode);
libofd_status_t libofd_set_tassl_root(libofd_handle_t* handle, const char* tassl_root);
libofd_status_t libofd_set_external_sign_provider(
    libofd_handle_t* handle, const libofd_external_sign_provider_t* provider);
libofd_status_t libofd_clear_external_sign_provider(libofd_handle_t* handle);
libofd_status_t libofd_set_external_convert_provider(
    libofd_handle_t* handle, const libofd_external_convert_provider_t* provider);
libofd_status_t libofd_clear_external_convert_provider(libofd_handle_t* handle);
libofd_status_t libofd_set_external_image_decode_provider(
    libofd_handle_t* handle, const libofd_external_image_decode_provider_t* provider);
libofd_status_t libofd_clear_external_image_decode_provider(libofd_handle_t* handle);

libofd_status_t libofd_sign_with_private_key(libofd_handle_t* handle, const char* private_key_pem_path);
libofd_status_t libofd_verify_signatures(
    const libofd_handle_t* handle, const char* public_key_pem_path, libofd_signature_verify_result_t* out_result);
libofd_status_t libofd_convert_ofd_to_pdf(
    libofd_handle_t* handle, const char* input_ofd_path, const char* output_pdf_path);
libofd_status_t libofd_convert_pdf_to_ofd(
    libofd_handle_t* handle, const char* input_pdf_path, const char* output_ofd_path);

const char* libofd_version(void);

#ifdef __cplusplus
}
#endif

#endif

