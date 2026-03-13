#ifndef LIBOFD_DOCUMENT_H
#define LIBOFD_DOCUMENT_H

#include <string>
#include <unordered_map>
#include <vector>
#include "libofd/status.h"

namespace libofd {

enum class SignBackend {
    kOpenSSL = 0,
    kTaSSL = 1
};

struct OfdPageInfo {
    std::string page_id;
    std::string base_loc;
    std::string text;
    std::string content_xml;
};

struct OfdDocumentInfo {
    std::string package_root;
    std::string ofd_xml_path;
    std::string document_xml_path;
    std::string creator;
    std::string creation_date;
    std::string doc_id;
    std::string max_unit_id;
};

class OfdDocument {
public:
    OfdDocument() = default;

    void CreateEmpty(const std::string& doc_id, const std::string& creator);
    libofd_status_t LoadFromPath(const std::string& path);
    libofd_status_t LoadFromExplodedPackage(const std::string& package_root);
    libofd_status_t SaveToExplodedPackage(const std::string& output_root) const;
    libofd_status_t ExportToText(const std::string& output_text_file) const;
    libofd_status_t ImportFromText(const std::string& input_text_file, const std::string& doc_id, const std::string& creator);
    libofd_status_t SetCreator(const std::string& creator);
    libofd_status_t AddPageText(const std::string& page_text);
    libofd_status_t SetPageText(size_t page_index, const std::string& page_text);
    libofd_status_t GetPageText(size_t page_index, std::string* out_page_text) const;
    libofd_status_t SetPageContentXml(size_t page_index, const std::string& content_xml);
    libofd_status_t GetPageContentXml(size_t page_index, std::string* out_content_xml) const;
    libofd_status_t GetPageContentBlockCount(size_t page_index, size_t* out_count) const;
    libofd_status_t GetPageContentBlockXml(size_t page_index, size_t block_index, std::string* out_block_xml) const;
    libofd_status_t SetPageContentBlockXml(size_t page_index, size_t block_index, const std::string& block_xml);
    libofd_status_t GetBlockObjectCount(size_t page_index, size_t block_index, size_t* out_count) const;
    libofd_status_t GetBlockObjectXml(
        size_t page_index, size_t block_index, size_t object_index, std::string* out_object_xml) const;
    libofd_status_t SetBlockObjectXml(
        size_t page_index, size_t block_index, size_t object_index, const std::string& object_xml);
    libofd_status_t AddBlockObjectXml(size_t page_index, size_t block_index, const std::string& object_xml);

    libofd_status_t SetCommonDataXml(const std::string& common_data_xml);
    libofd_status_t GetCommonDataXml(std::string* out_common_data_xml) const;
    libofd_status_t SetOutlineXml(const std::string& outline_xml);
    libofd_status_t GetOutlineXml(std::string* out_outline_xml) const;
    libofd_status_t SetPermissionsXml(const std::string& permissions_xml);
    libofd_status_t GetPermissionsXml(std::string* out_permissions_xml) const;
    libofd_status_t SetFormXml(const std::string& form_xml);
    libofd_status_t GetFormXml(std::string* out_form_xml) const;
    libofd_status_t SetPageAnnotationsXml(size_t page_index, const std::string& annotations_xml);
    libofd_status_t GetPageAnnotationsXml(size_t page_index, std::string* out_annotations_xml) const;
    libofd_status_t SetPageActionsXml(size_t page_index, const std::string& actions_xml);
    libofd_status_t GetPageActionsXml(size_t page_index, std::string* out_actions_xml) const;

    libofd_status_t SetSignBackend(SignBackend backend);
    libofd_status_t SetTaSSLRoot(const std::string& tassl_root);

    libofd_status_t SignWithPrivateKeyPem(const std::string& private_key_pem_path);
    libofd_status_t VerifyWithPublicKeyPem(const std::string& public_key_pem_path, bool* out_verified) const;
    libofd_status_t BuildSignPayload(std::string* out_payload) const;
    libofd_status_t SetSignatureBlob(const std::vector<unsigned char>& signature_blob);
    const std::vector<unsigned char>& SignatureBlob() const { return signature_blob_; }

    const OfdDocumentInfo& Info() const { return info_; }
    const std::vector<OfdPageInfo>& Pages() const { return pages_; }
    bool IsLoaded() const { return loaded_; }

private:
    bool loaded_ = false;
    OfdDocumentInfo info_;
    std::vector<OfdPageInfo> pages_;
    std::vector<unsigned char> signature_blob_;
    SignBackend sign_backend_ = SignBackend::kTaSSL;
    std::string tassl_root_ = "/home/yb/tassl/";
    std::string common_data_xml_;
    std::string outline_xml_;
    std::string permissions_xml_;
    std::string form_xml_;
    std::unordered_map<size_t, std::string> page_annotations_xml_;
    std::unordered_map<size_t, std::string> page_actions_xml_;
};

} // namespace libofd

#endif

