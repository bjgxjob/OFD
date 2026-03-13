#include "libofd/signature.h"

#include "libofd/document.h"

namespace libofd {

libofd_status_t SignatureVerifier::Verify(
    const OfdDocument& document, const std::string& public_key_pem_path, SignatureVerifyResult* out_result) const {
    if (out_result == nullptr) {
        return LIBOFD_ERR_INVALID_ARGUMENT;
    }
    if (!document.IsLoaded()) {
        return LIBOFD_ERR_NOT_FOUND;
    }

    bool verified = false;
    libofd_status_t status = document.VerifyWithPublicKeyPem(public_key_pem_path, &verified);
    if (status != LIBOFD_OK) {
        return status;
    }
    out_result->declared_signature_count = 1;
    out_result->verified_signature_count = verified ? 1 : 0;
    out_result->all_valid = verified;
    return LIBOFD_OK;
}

} // namespace libofd

