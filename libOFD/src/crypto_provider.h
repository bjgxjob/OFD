#ifndef LIBOFD_CRYPTO_PROVIDER_H
#define LIBOFD_CRYPTO_PROVIDER_H

#include <memory>
#include <string>
#include <vector>

#include "libofd/document.h"
#include "libofd/status.h"

namespace libofd {

class CryptoProvider {
public:
    virtual ~CryptoProvider() = default;
    virtual libofd_status_t Sign(
        const std::string& payload, const std::string& private_key_pem_path, std::vector<unsigned char>* out_signature) const = 0;
    virtual libofd_status_t Verify(
        const std::string& payload, const std::string& public_key_pem_path,
        const std::vector<unsigned char>& signature, bool* out_verified) const = 0;
};

std::string ResolveCryptoExecutable(SignBackend backend, const std::string& tassl_root);
libofd_status_t CreateCryptoProvider(
    SignBackend backend, const std::string& tassl_root, std::unique_ptr<CryptoProvider>* out_provider);

} // namespace libofd

#endif

