// PKCE.cpp
// RFC 7636 PKCE implementation using OpenSSL EVP_Digest (libcrypto, already
// linked via vcpkg) and BCryptGenRandom for the CSPRNG.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")

#include <openssl/evp.h>
#include <openssl/bio.h>
#include <openssl/buffer.h>

#include "PKCE.h"
#include <vector>
#include <stdexcept>

namespace ChatGPT
{
    // base64url-no-pad: standard base64, then + -> -, / -> _, strip =
    static std::string toBase64URL(const std::vector<unsigned char>& data)
    {
        // Use OpenSSL BIO base64 encode
        BIO* b64 = BIO_new(BIO_f_base64());
        BIO* mem = BIO_new(BIO_s_mem());
        b64 = BIO_push(b64, mem);
        BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
        BIO_write(b64, data.data(), static_cast<int>(data.size()));
        BIO_flush(b64);

        BUF_MEM* bptr = nullptr;
        BIO_get_mem_ptr(b64, &bptr);
        std::string result(bptr->data, bptr->length);
        BIO_free_all(b64);

        // Convert to base64url (no padding)
        for (char& c : result)
        {
            if (c == '+') c = '-';
            else if (c == '/') c = '_';
        }
        // Strip trailing '='
        while (!result.empty() && result.back() == '=')
            result.pop_back();
        return result;
    }

    static std::vector<unsigned char> randomBytes(size_t count)
    {
        std::vector<unsigned char> buf(count);
        NTSTATUS status = BCryptGenRandom(nullptr, buf.data(),
            static_cast<ULONG>(count), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
        if (!BCRYPT_SUCCESS(status))
            throw std::runtime_error("BCryptGenRandom failed");
        return buf;
    }

    std::string PKCEMakeVerifier()
    {
        return toBase64URL(randomBytes(32));
    }

    std::string PKCEChallenge(const std::string& verifier)
    {
        // SHA-256 via OpenSSL EVP
        unsigned char hash[EVP_MAX_MD_SIZE];
        unsigned int  hashLen = 0;
        EVP_MD_CTX* ctx = EVP_MD_CTX_new();
        EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr);
        EVP_DigestUpdate(ctx, verifier.data(), verifier.size());
        EVP_DigestFinal_ex(ctx, hash, &hashLen);
        EVP_MD_CTX_free(ctx);

        return toBase64URL(std::vector<unsigned char>(hash, hash + hashLen));
    }

    std::string PKCEMakeState()
    {
        return toBase64URL(randomBytes(32));
    }
}
