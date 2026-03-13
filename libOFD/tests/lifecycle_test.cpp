#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include "libofd/libofd.h"

namespace fs = std::filesystem;

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

int main() {
#ifndef LIBOFD_SOURCE_DIR
#define LIBOFD_SOURCE_DIR "."
#endif
    const fs::path work_dir = fs::path(LIBOFD_SOURCE_DIR) / "tests" / "tmp_lifecycle";
    std::error_code ec;
    fs::remove_all(work_dir, ec);
    fs::create_directories(work_dir, ec);

    libofd_handle_t* h = libofd_create();
    if (h == nullptr) {
        std::cerr << "failed to create handle\n";
        return EXIT_FAILURE;
    }

    libofd_status_t status = libofd_create_empty(h, "doc-lifecycle", "libofd-test");
    if (status != LIBOFD_OK) {
        std::cerr << "create_empty failed\n";
        libofd_destroy(h);
        return EXIT_FAILURE;
    }
    if (libofd_add_page_text(h, "first page") != LIBOFD_OK || libofd_add_page_text(h, "second page") != LIBOFD_OK) {
        std::cerr << "add_page_text failed\n";
        libofd_destroy(h);
        return EXIT_FAILURE;
    }
    if (libofd_set_page_text(h, 1, "second page edited") != LIBOFD_OK) {
        std::cerr << "set_page_text failed\n";
        libofd_destroy(h);
        return EXIT_FAILURE;
    }
    const char* page0_xml =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<ofd:Page xmlns:ofd=\"http://www.ofdspec.org/2016\">\n"
        "  <ofd:Content>\n"
        "    <ofd:Layer Type=\"Body\" ID=\"20\">\n"
        "      <ofd:TextObject ID=\"21\" Font=\"1\" Size=\"3.5\" Boundary=\"10 10 180 4.5\">\n"
        "        <ofd:TextCode X=\"0\" Y=\"3.5\">first page custom xml</ofd:TextCode>\n"
        "      </ofd:TextObject>\n"
        "      <ofd:PathObject ID=\"22\" Boundary=\"0 0 210 297\" LineWidth=\"0.3\" Stroke=\"true\" Fill=\"false\"\n"
        "                      AbbreviatedData=\"M 10 10 L 40 10 L 40 20 L 10 20 B\"/>\n"
        "    </ofd:Layer>\n"
        "  </ofd:Content>\n"
        "</ofd:Page>\n";
    if (libofd_set_page_content_xml(h, 0, page0_xml) != LIBOFD_OK) {
        std::cerr << "set_page_content_xml failed\n";
        libofd_destroy(h);
        return EXIT_FAILURE;
    }
    char page0_xml_out[8192] = {0};
    if (libofd_get_page_content_xml(h, 0, page0_xml_out, sizeof(page0_xml_out)) != LIBOFD_OK ||
        std::string(page0_xml_out).find("PathObject") == std::string::npos) {
        std::cerr << "get_page_content_xml failed\n";
        libofd_destroy(h);
        return EXIT_FAILURE;
    }

    const fs::path ofd_dir = work_dir / "doc_out";
    status = libofd_save_exploded_package(h, ofd_dir.string().c_str());
    if (status != LIBOFD_OK) {
        std::cerr << "save_exploded_package failed: " << libofd_status_message(status) << "\n";
        libofd_destroy(h);
        return EXIT_FAILURE;
    }

    const fs::path private_key = work_dir / "private.pem";
    const fs::path public_key = work_dir / "public.pem";
    const fs::path tassl_root = "/home/yb/tassl";
    const fs::path tassl_bin = tassl_root / "bin" / "openssl";
    std::string crypto_exec = "openssl";
    if (fs::exists(tassl_bin)) {
        if (libofd_set_tassl_root(h, tassl_root.string().c_str()) != LIBOFD_OK ||
            libofd_set_sign_backend(h, LIBOFD_SIGN_BACKEND_TASSL) != LIBOFD_OK) {
            std::cerr << "failed to set tassl backend\n";
            libofd_destroy(h);
            return EXIT_FAILURE;
        }
        crypto_exec = tassl_bin.string();
    } else {
        if (libofd_set_sign_backend(h, LIBOFD_SIGN_BACKEND_OPENSSL) != LIBOFD_OK) {
            std::cerr << "failed to set openssl backend\n";
            libofd_destroy(h);
            return EXIT_FAILURE;
        }
    }

    const std::string gen_priv_cmd = ShellQuote(crypto_exec) + " genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:2048 -out " +
                                     ShellQuote(private_key.string()) + " >/dev/null 2>&1";
    if (std::system(gen_priv_cmd.c_str()) != 0) {
        std::cerr << "failed to generate private key, openssl may be missing\n";
        libofd_destroy(h);
        return EXIT_FAILURE;
    }
    const std::string gen_pub_cmd = ShellQuote(crypto_exec) + " rsa -pubout -in " + ShellQuote(private_key.string()) + " -out " +
                                    ShellQuote(public_key.string()) + " >/dev/null 2>&1";
    if (std::system(gen_pub_cmd.c_str()) != 0) {
        std::cerr << "failed to generate public key\n";
        libofd_destroy(h);
        return EXIT_FAILURE;
    }

    status = libofd_sign_with_private_key(h, private_key.string().c_str());
    if (status != LIBOFD_OK) {
        std::cerr << "sign failed: " << libofd_status_message(status) << "\n";
        libofd_destroy(h);
        return EXIT_FAILURE;
    }
    libofd_signature_verify_result_t verify_result{};
    status = libofd_verify_signatures(h, public_key.string().c_str(), &verify_result);
    if (status != LIBOFD_OK || verify_result.all_valid != 1) {
        std::cerr << "verify failed\n";
        libofd_destroy(h);
        return EXIT_FAILURE;
    }

    status = libofd_save_exploded_package(h, ofd_dir.string().c_str());
    if (status != LIBOFD_OK) {
        std::cerr << "save signed package failed\n";
        libofd_destroy(h);
        return EXIT_FAILURE;
    }

    const fs::path txt_file = work_dir / "doc.txt";
    status = libofd_export_to_text(h, txt_file.string().c_str());
    if (status != LIBOFD_OK) {
        std::cerr << "export_to_text failed\n";
        libofd_destroy(h);
        return EXIT_FAILURE;
    }
    libofd_destroy(h);

    libofd_handle_t* imported = libofd_create();
    if (imported == nullptr) {
        return EXIT_FAILURE;
    }
    status = libofd_import_from_text(imported, txt_file.string().c_str(), "doc-imported", "importer");
    if (status != LIBOFD_OK) {
        std::cerr << "import_from_text failed\n";
        libofd_destroy(imported);
        return EXIT_FAILURE;
    }
    const fs::path imported_dir = work_dir / "doc_imported";
    status = libofd_save_exploded_package(imported, imported_dir.string().c_str());
    if (status != LIBOFD_OK) {
        std::cerr << "save imported package failed\n";
        libofd_destroy(imported);
        return EXIT_FAILURE;
    }
    if (!fs::exists(imported_dir / "OFD.xml")) {
        std::cerr << "imported package missing OFD.xml\n";
        libofd_destroy(imported);
        return EXIT_FAILURE;
    }

    libofd_destroy(imported);
    fs::remove_all(work_dir, ec);
    return EXIT_SUCCESS;
}

