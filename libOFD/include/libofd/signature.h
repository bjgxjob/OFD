#ifndef LIBOFD_SIGNATURE_H
#define LIBOFD_SIGNATURE_H

#include <stddef.h>
#include <string>
#include "libofd/status.h"

namespace libofd {

class OfdDocument;

struct SignatureVerifyResult {
    size_t declared_signature_count = 0;
    size_t verified_signature_count = 0;
    bool all_valid = false;
};

class SignatureVerifier {
public:
    libofd_status_t Verify(
        const OfdDocument& document, const std::string& public_key_pem_path, SignatureVerifyResult* out_result) const;
};

} // namespace libofd

#endif

