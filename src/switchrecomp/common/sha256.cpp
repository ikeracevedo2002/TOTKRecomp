#include "switchrecomp/common/sha256.hpp"

#include <array>
#include <cctype>
#include <fstream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <bcrypt.h>
#else
#include <openssl/evp.h>
#endif

namespace switchrecomp
{
namespace
{

[[nodiscard]] int hex_value(char value) noexcept
{
    if (value >= '0' && value <= '9')
    {
        return value - '0';
    }
    if (value >= 'a' && value <= 'f')
    {
        return value - 'a' + 10;
    }
    if (value >= 'A' && value <= 'F')
    {
        return value - 'A' + 10;
    }
    return -1;
}

[[nodiscard]] Error hash_error(std::string message)
{
    return make_error(ErrorCode::IoError, std::move(message));
}

#ifdef _WIN32
[[nodiscard]] Result<Sha256Digest> sha256_windows(std::span<const std::byte> bytes)
{
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD object_size = 0;
    DWORD result_size = 0;
    NTSTATUS status = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
    if (status < 0)
    {
        return Result<Sha256Digest>::failure(hash_error("BCryptOpenAlgorithmProvider failed"));
    }

    status = BCryptGetProperty(
        algorithm,
        BCRYPT_OBJECT_LENGTH,
        reinterpret_cast<PUCHAR>(&object_size),
        sizeof(object_size),
        &result_size,
        0);
    if (status < 0 || object_size == 0)
    {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return Result<Sha256Digest>::failure(hash_error("could not query SHA-256 object size"));
    }

    std::vector<UCHAR> object(object_size);
    status = BCryptCreateHash(
        algorithm,
        &hash,
        object.data(),
        object_size,
        nullptr,
        0,
        0);
    if (status >= 0)
    {
        status = BCryptHashData(
            hash,
            const_cast<PUCHAR>(reinterpret_cast<const UCHAR*>(bytes.data())),
            static_cast<ULONG>(bytes.size()),
            0);
    }

    Sha256Digest digest;
    if (status >= 0)
    {
        status = BCryptFinishHash(hash, reinterpret_cast<PUCHAR>(digest.bytes.data()), 32, 0);
    }
    if (hash != nullptr)
    {
        BCryptDestroyHash(hash);
    }
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (status < 0)
    {
        return Result<Sha256Digest>::failure(hash_error("Windows SHA-256 operation failed"));
    }
    return Result<Sha256Digest>::success(digest);
}
#else
[[nodiscard]] Result<Sha256Digest> sha256_openssl(std::span<const std::byte> bytes)
{
    using Context = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;
    Context context(EVP_MD_CTX_new(), &EVP_MD_CTX_free);
    if (!context)
    {
        return Result<Sha256Digest>::failure(hash_error("EVP_MD_CTX_new failed"));
    }
    if (EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1 ||
        EVP_DigestUpdate(context.get(), bytes.data(), bytes.size()) != 1)
    {
        return Result<Sha256Digest>::failure(hash_error("OpenSSL SHA-256 initialization failed"));
    }

    Sha256Digest digest;
    unsigned int digest_size = 0;
    if (EVP_DigestFinal_ex(context.get(), reinterpret_cast<unsigned char*>(digest.bytes.data()), &digest_size) != 1 ||
        digest_size != digest.bytes.size())
    {
        return Result<Sha256Digest>::failure(hash_error("OpenSSL SHA-256 finalization failed"));
    }
    return Result<Sha256Digest>::success(digest);
}
#endif

} // namespace

Result<Sha256Digest> sha256_bytes(std::span<const std::byte> bytes)
{
#ifdef _WIN32
    return sha256_windows(bytes);
#else
    return sha256_openssl(bytes);
#endif
}

Result<Sha256Digest> sha256_file(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
    {
        return Result<Sha256Digest>::failure(
            make_error(ErrorCode::MissingFile, "could not open file: " + path.string()));
    }

#ifdef _WIN32
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD object_size = 0;
    DWORD result_size = 0;
    NTSTATUS status = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
    if (status < 0)
    {
        return Result<Sha256Digest>::failure(hash_error("BCryptOpenAlgorithmProvider failed"));
    }
    status = BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&object_size), sizeof(object_size), &result_size, 0);
    if (status < 0 || object_size == 0)
    {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return Result<Sha256Digest>::failure(hash_error("could not query SHA-256 object size"));
    }
    std::vector<UCHAR> object(object_size);
    status = BCryptCreateHash(algorithm, &hash, object.data(), object_size, nullptr, 0, 0);
    std::array<char, 1024 * 1024> buffer{};
    while (status >= 0 && input)
    {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = input.gcount();
        if (count > 0)
        {
            status = BCryptHashData(hash, reinterpret_cast<PUCHAR>(buffer.data()), static_cast<ULONG>(count), 0);
        }
    }
    Sha256Digest digest;
    if (status >= 0)
    {
        status = BCryptFinishHash(hash, reinterpret_cast<PUCHAR>(digest.bytes.data()), 32, 0);
    }
    if (hash != nullptr)
    {
        BCryptDestroyHash(hash);
    }
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (status < 0 || input.bad())
    {
        return Result<Sha256Digest>::failure(hash_error("Windows SHA-256 file operation failed"));
    }
    return Result<Sha256Digest>::success(digest);
#else
    using Context = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;
    Context context(EVP_MD_CTX_new(), &EVP_MD_CTX_free);
    if (!context || EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1)
    {
        return Result<Sha256Digest>::failure(hash_error("OpenSSL SHA-256 initialization failed"));
    }

    std::array<char, 1024 * 1024> buffer{};
    while (input)
    {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = input.gcount();
        if (count > 0 && EVP_DigestUpdate(context.get(), buffer.data(), static_cast<std::size_t>(count)) != 1)
        {
            return Result<Sha256Digest>::failure(hash_error("OpenSSL SHA-256 update failed"));
        }
    }
    if (input.bad())
    {
        return Result<Sha256Digest>::failure(
            make_error(ErrorCode::IoError, "error while reading file: " + path.string()));
    }

    Sha256Digest digest;
    unsigned int digest_size = 0;
    if (EVP_DigestFinal_ex(context.get(), reinterpret_cast<unsigned char*>(digest.bytes.data()), &digest_size) != 1 ||
        digest_size != digest.bytes.size())
    {
        return Result<Sha256Digest>::failure(hash_error("OpenSSL SHA-256 finalization failed"));
    }
    return Result<Sha256Digest>::success(digest);
#endif
}

Result<Sha256Digest> parse_sha256_hex(std::string_view text)
{
    if (text.size() != 64)
    {
        return Result<Sha256Digest>::failure(make_error(
            ErrorCode::InvalidArgument,
            "SHA-256 value must contain exactly 64 hexadecimal characters"));
    }

    Sha256Digest digest;
    for (std::size_t index = 0; index < digest.bytes.size(); ++index)
    {
        const int high = hex_value(text[index * 2]);
        const int low = hex_value(text[index * 2 + 1]);
        if (high < 0 || low < 0)
        {
            return Result<Sha256Digest>::failure(make_error(
                ErrorCode::InvalidArgument,
                "SHA-256 value contains a non-hexadecimal character"));
        }
        digest.bytes[index] = static_cast<std::byte>((high << 4) | low);
    }
    return Result<Sha256Digest>::success(digest);
}

std::string sha256_to_hex(const Sha256Digest& digest)
{
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(64);
    for (const auto byte : digest.bytes)
    {
        const auto value = std::to_integer<unsigned int>(byte);
        result.push_back(digits[(value >> 4U) & 0x0FU]);
        result.push_back(digits[value & 0x0FU]);
    }
    return result;
}

} // namespace switchrecomp
