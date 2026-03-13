#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

#include "libofd/libofd.h"

static libofd_status_t DummySign(
    const unsigned char* payload, size_t payload_len, const char* private_key_path, unsigned char* out_signature,
    size_t* inout_signature_len, void* user_data) {
    (void)private_key_path;
    (void)user_data;
    if (payload == nullptr || inout_signature_len == nullptr) {
        return LIBOFD_ERR_INVALID_ARGUMENT;
    }
    const size_t want = payload_len < 16U ? payload_len : 16U;
    if (out_signature == nullptr) {
        *inout_signature_len = want;
        return LIBOFD_OK;
    }
    if (*inout_signature_len < want) {
        return LIBOFD_ERR_INVALID_ARGUMENT;
    }
    std::memcpy(out_signature, payload, want);
    *inout_signature_len = want;
    return LIBOFD_OK;
}

static libofd_status_t DummyVerify(
    const unsigned char* payload, size_t payload_len, const char* public_key_path, const unsigned char* signature,
    size_t signature_len, int* out_verified, void* user_data) {
    (void)public_key_path;
    (void)user_data;
    if (payload == nullptr || signature == nullptr || out_verified == nullptr) {
        return LIBOFD_ERR_INVALID_ARGUMENT;
    }
    const size_t want = payload_len < 16U ? payload_len : 16U;
    *out_verified = (signature_len == want && std::memcmp(payload, signature, want) == 0) ? 1 : 0;
    return LIBOFD_OK;
}

int main() {
    libofd_handle_t* handle = libofd_create();
    if (handle == nullptr) {
        return EXIT_FAILURE;
    }
    if (libofd_create_empty(handle, "doc-provider", "provider-test") != LIBOFD_OK) {
        libofd_destroy(handle);
        return EXIT_FAILURE;
    }
    if (libofd_add_page_text(handle, "external provider page text") != LIBOFD_OK) {
        libofd_destroy(handle);
        return EXIT_FAILURE;
    }

    libofd_external_sign_provider_t provider{};
    provider.sign_fn = DummySign;
    provider.verify_fn = DummyVerify;
    provider.user_data = nullptr;
    if (libofd_set_external_sign_provider(handle, &provider) != LIBOFD_OK) {
        libofd_destroy(handle);
        return EXIT_FAILURE;
    }

    if (libofd_sign_with_private_key(handle, "dummy-private-key") != LIBOFD_OK) {
        libofd_destroy(handle);
        return EXIT_FAILURE;
    }

    libofd_signature_verify_result_t result{};
    if (libofd_verify_signatures(handle, "dummy-public-key", &result) != LIBOFD_OK) {
        libofd_destroy(handle);
        return EXIT_FAILURE;
    }
    if (result.all_valid != 1 || result.verified_signature_count != 1U) {
        std::cerr << "external provider verify failed\n";
        libofd_destroy(handle);
        return EXIT_FAILURE;
    }

    if (libofd_clear_external_sign_provider(handle) != LIBOFD_OK) {
        libofd_destroy(handle);
        return EXIT_FAILURE;
    }
    libofd_destroy(handle);
    return EXIT_SUCCESS;
}

