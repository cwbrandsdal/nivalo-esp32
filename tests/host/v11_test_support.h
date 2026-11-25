// Shared helpers for the protocol v1.1 host tests: fixture loading and failure reporting.
#ifndef NIVALO_V11_TEST_SUPPORT_H
#define NIVALO_V11_TEST_SUPPORT_H

#include <ArduinoJson.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>

#include <mbedtls/sha256.h>

namespace v11test
{
inline int &failures()
{
    static int count = 0;
    return count;
}

inline std::string &context()
{
    static std::string value;
    return value;
}

inline void fail(const char *file, int line, const std::string &message)
{
    failures()++;
    std::fprintf(stderr, "%s:%d: [%s] %s\n", file, line, context().c_str(), message.c_str());
}

#define V11_CHECK(condition, message)                                                    \
    do                                                                                   \
    {                                                                                    \
        if (!(condition))                                                                \
        {                                                                                \
            std::ostringstream v11_stream;                                               \
            v11_stream << message;                                                       \
            ::v11test::fail(__FILE__, __LINE__, #condition " -- " + v11_stream.str());   \
        }                                                                                \
    } while (0)

inline int finish(const char *suite, int checked)
{
    if (failures() == 0)
    {
        std::printf("%s: %d cases passed\n", suite, checked);
        return 0;
    }
    std::fprintf(stderr, "%s: %d failure(s) in %d cases\n", suite, failures(), checked);
    return 1;
}

inline std::string protocolPath(const std::string &relative)
{
    return std::string(NIVALO_PROTOCOL_DIR) + "/" + relative;
}

inline std::string readFile(const std::string &path)
{
    std::ifstream input(path.c_str(), std::ios::binary);
    if (!input)
    {
        std::fprintf(stderr, "cannot read %s\n", path.c_str());
        std::exit(2);
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

// Fixture files are parsed with ArduinoJson; numbers keep their integer/float distinction, which
// is what the integer-lexical cases need.
inline std::unique_ptr<DynamicJsonDocument> loadFixture(const std::string &relative)
{
    const std::string text = readFile(protocolPath(relative));
    std::unique_ptr<DynamicJsonDocument> document(new DynamicJsonDocument(text.size() * 6U + 65536U));
    const DeserializationError error =
        deserializeJson(*document, text.c_str(), text.size(), DeserializationOption::NestingLimit(64));
    if (error)
    {
        std::fprintf(stderr, "cannot parse %s: %s\n", relative.c_str(), error.c_str());
        std::exit(2);
    }
    return document;
}

// Compact JSON of a fixture value. The fixtures' payload objects use only integers and plain
// strings, so this equals the reference model's wire_text.
inline std::string compact(JsonVariantConst value)
{
    std::string output;
    serializeJson(value, output);
    return output;
}

inline std::string sha256Hex(const std::string &bytes)
{
    unsigned char digest[32];
    mbedtls_sha256_ret(reinterpret_cast<const unsigned char *>(bytes.data()), bytes.size(), digest, 0);
    static const char hex[] = "0123456789abcdef";
    std::string output;
    for (unsigned char byte : digest)
    {
        output += hex[byte >> 4];
        output += hex[byte & 15];
    }
    return output;
}

// SubjectPublicKeyInfo DER (Base64) as PEM.
inline std::string pemFromSpkiBase64(const std::string &base64)
{
    std::string pem = "-----BEGIN PUBLIC KEY-----\n";
    for (size_t i = 0; i < base64.size(); i += 64U)
    {
        pem += base64.substr(i, 64U) + "\n";
    }
    pem += "-----END PUBLIC KEY-----\n";
    return pem;
}

inline const char *str(JsonVariantConst value, const char *fallback = "")
{
    return value.is<const char *>() ? value.as<const char *>() : fallback;
}
} // namespace v11test

#endif
