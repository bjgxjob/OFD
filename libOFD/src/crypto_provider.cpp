#include "crypto_provider.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace libofd {

static std::string ShellQuote(const std::string& value) {
    std::string out = "'";
    for (char c : value) {
        if (c == '\'') {
            out += "'\\''";
        } else {
            out.push_back(c);
        }
    }
    out.push_back('\'');
    return out;
}

static libofd_status_t ReadBinaryFile(const fs::path& path, std::vector<unsigned char>* out_content) {
    if (out_content == nullptr) {
        return LIBOFD_ERR_INVALID_ARGUMENT;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        return LIBOFD_ERR_IO;
    }
    out_content->assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return LIBOFD_OK;
}

static libofd_status_t WriteTextFile(const fs::path& path, const std::string& content) {
    std::ofstream out(path, std::ios::binary);
    if (!out.is_open()) {
        return LIBOFD_ERR_IO;
    }
    out << content;
    return LIBOFD_OK;
}

static libofd_status_t WriteBinaryFile(const fs::path& path, const std::vector<unsigned char>& content) {
    std::ofstream out(path, std::ios::binary);
    if (!out.is_open()) {
        return LIBOFD_ERR_IO;
    }
    out.write(reinterpret_cast<const char*>(content.data()), static_cast<std::streamsize>(content.size()));
    return LIBOFD_OK;
}

class CliCryptoProvider final : public CryptoProvider {
public:
    explicit CliCryptoProvider(std::string executable) : executable_(std::move(executable)) {}

    libofd_status_t Sign(
        const std::string& payload, const std::string& private_key_pem_path,
        std::vector<unsigned char>* out_signature) const override {
        if (private_key_pem_path.empty() || out_signature == nullptr) {
            return LIBOFD_ERR_INVALID_ARGUMENT;
        }
        if (!fs::exists(private_key_pem_path)) {
            return LIBOFD_ERR_NOT_FOUND;
        }
        if (!fs::exists(executable_)) {
            // Allow PATH executable fallback, e.g. "openssl".
            if (executable_.find('/') != std::string::npos) {
                return LIBOFD_ERR_NOT_FOUND;
            }
        }

        const fs::path temp_dir = fs::temp_directory_path();
        const fs::path payload_path = temp_dir / "libofd_cli_payload.txt";
        const fs::path sig_path = temp_dir / "libofd_cli_signature.bin";
        libofd_status_t status = WriteTextFile(payload_path, payload);
        if (status != LIBOFD_OK) {
            return status;
        }
        const std::string cmd = ShellQuote(executable_) + " dgst -sha256 -sign " + ShellQuote(private_key_pem_path) + " -out " +
                                ShellQuote(sig_path.string()) + " " + ShellQuote(payload_path.string()) + " >/dev/null 2>&1";
        const int rc = std::system(cmd.c_str());
        std::remove(payload_path.string().c_str());
        if (rc != 0) {
            std::remove(sig_path.string().c_str());
            return LIBOFD_ERR_IO;
        }
        status = ReadBinaryFile(sig_path, out_signature);
        std::remove(sig_path.string().c_str());
        return status;
    }

    libofd_status_t Verify(
        const std::string& payload, const std::string& public_key_pem_path,
        const std::vector<unsigned char>& signature, bool* out_verified) const override {
        if (public_key_pem_path.empty() || out_verified == nullptr) {
            return LIBOFD_ERR_INVALID_ARGUMENT;
        }
        *out_verified = false;
        if (signature.empty()) {
            return LIBOFD_ERR_NOT_FOUND;
        }
        if (!fs::exists(public_key_pem_path)) {
            return LIBOFD_ERR_NOT_FOUND;
        }
        if (!fs::exists(executable_)) {
            if (executable_.find('/') != std::string::npos) {
                return LIBOFD_ERR_NOT_FOUND;
            }
        }

        const fs::path temp_dir = fs::temp_directory_path();
        const fs::path payload_path = temp_dir / "libofd_cli_payload_verify.txt";
        const fs::path sig_path = temp_dir / "libofd_cli_signature_verify.bin";
        libofd_status_t status = WriteTextFile(payload_path, payload);
        if (status != LIBOFD_OK) {
            return status;
        }
        status = WriteBinaryFile(sig_path, signature);
        if (status != LIBOFD_OK) {
            std::remove(payload_path.string().c_str());
            return status;
        }

        const std::string cmd = ShellQuote(executable_) + " dgst -sha256 -verify " + ShellQuote(public_key_pem_path) +
                                " -signature " + ShellQuote(sig_path.string()) + " " + ShellQuote(payload_path.string()) +
                                " >/dev/null 2>&1";
        const int rc = std::system(cmd.c_str());
        std::remove(payload_path.string().c_str());
        std::remove(sig_path.string().c_str());
        *out_verified = (rc == 0);
        return LIBOFD_OK;
    }

private:
    std::string executable_;
};

std::string ResolveCryptoExecutable(SignBackend backend, const std::string& tassl_root) {
    if (backend == SignBackend::kTaSSL) {
        const fs::path root(tassl_root.empty() ? "/home/yb/tassl/" : tassl_root);
        return (root / "bin" / "openssl").string();
    }
    return "openssl";
}

libofd_status_t CreateCryptoProvider(
    SignBackend backend, const std::string& tassl_root, std::unique_ptr<CryptoProvider>* out_provider) {
    if (out_provider == nullptr) {
        return LIBOFD_ERR_INVALID_ARGUMENT;
    }
    *out_provider = std::make_unique<CliCryptoProvider>(ResolveCryptoExecutable(backend, tassl_root));
    return LIBOFD_OK;
}

} // namespace libofd

