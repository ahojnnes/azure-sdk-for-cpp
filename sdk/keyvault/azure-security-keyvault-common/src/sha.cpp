// Copyright (c) Microsoft Corporation. All rights reserved.
// SPDX-License-Identifier: MIT

#include <azure/core/platform.hpp>

#if defined(AZ_PLATFORM_WINDOWS)
// Windows needs to go before bcrypt
#include <windows.h>

#include <bcrypt.h>
#elif defined(AZ_PLATFORM_POSIX)
#include <openssl/evp.h>
#endif

#include "azure/keyvault/common/sha.hpp"

#include <memory>
#include <stdexcept>
#include <vector>

using namespace Azure::Security::KeyVault;

#if defined(AZ_PLATFORM_POSIX)

namespace {

enum class SHASize
{
  SHA256,
  SHA384,
  SHA512
};

/*************************** SHA256 *******************/
class SHAWithOpenSSL final : public Azure::Core::Cryptography::Hash {
private:
  EVP_MD_CTX* m_context;

  std::vector<uint8_t> OnFinal(const uint8_t* data, std::size_t length) override
  {
    OnAppend(data, length);
    unsigned int size;
    unsigned char finalHash[EVP_MAX_MD_SIZE];
    if (1 != EVP_DigestFinal(m_context, finalHash, &size))
    {
      throw std::runtime_error("Crypto error while computing SHA256.");
    }
    return std::vector<uint8_t>(std::begin(finalHash), std::begin(finalHash) + size);
  }

  void OnAppend(const uint8_t* data, std::size_t length) override
  {
    if (1 != EVP_DigestUpdate(m_context, data, length))
    {
      throw std::runtime_error("Crypto error while updating SHA256.");
    }
  }

public:
  SHAWithOpenSSL(SHASize size)
  {
    if ((m_context = EVP_MD_CTX_new()) == NULL)
    {
      throw std::runtime_error("Crypto error while creating EVP context.");
    }
    switch (size)
    {
      case SHASize::SHA256: {
        if (1 != EVP_DigestInit_ex(m_context, EVP_sha256(), NULL))
        {
          throw std::runtime_error("Crypto error while init SHA256.");
        }
        break;
      }
      case SHASize::SHA384: {
        if (1 != EVP_DigestInit_ex(m_context, EVP_sha384(), NULL))
        {
          throw std::runtime_error("Crypto error while init SHA384.");
        }
        break;
      }
      case SHASize::SHA512: {
        if (1 != EVP_DigestInit_ex(m_context, EVP_sha512(), NULL))
        {
          throw std::runtime_error("Crypto error while init SHA512.");
        }
        break;
      }
      default:
        // imposible to get here
        AZURE_UNREACHABLE_CODE();
    }
  }

  ~SHAWithOpenSSL() { EVP_MD_CTX_free(m_context); }
};

} // namespace

Azure::Security::KeyVault::SHA256::SHA256()
    : m_portableImplementation(std::make_unique<SHAWithOpenSSL>(SHASize::SHA256))
{
}

Azure::Security::KeyVault::SHA384::SHA384()
    : m_portableImplementation(std::make_unique<SHAWithOpenSSL>(SHASize::SHA384))
{
}

Azure::Security::KeyVault::SHA512::SHA512()
    : m_portableImplementation(std::make_unique<SHAWithOpenSSL>(SHASize::SHA512))
{
}

#endif

#if defined(AZ_PLATFORM_WINDOWS)
namespace {
struct AlgorithmProviderInstance final
{
  BCRYPT_ALG_HANDLE Handle;
  std::size_t ContextSize;
  std::size_t HashLength;

  AlgorithmProviderInstance(LPCWSTR hashAlgorithm)
  {
    NTSTATUS status = BCryptOpenAlgorithmProvider(&Handle, hashAlgorithm, nullptr, 0);
    if (!BCRYPT_SUCCESS(status))
    {
      throw std::runtime_error("BCryptOpenAlgorithmProvider failed");
    }
    DWORD objectLength = 0;
    DWORD dataLength = 0;
    status = BCryptGetProperty(
        Handle,
        BCRYPT_OBJECT_LENGTH,
        reinterpret_cast<PBYTE>(&objectLength),
        sizeof(objectLength),
        &dataLength,
        0);
    if (!BCRYPT_SUCCESS(status))
    {
      throw std::runtime_error("BCryptGetProperty failed");
    }
    ContextSize = objectLength;
    DWORD hashLength = 0;
    status = BCryptGetProperty(
        Handle,
        BCRYPT_HASH_LENGTH,
        reinterpret_cast<PBYTE>(&hashLength),
        sizeof(hashLength),
        &dataLength,
        0);
    if (!BCRYPT_SUCCESS(status))
    {
      throw std::runtime_error("BCryptGetProperty failed");
    }
    HashLength = hashLength;
  }

  ~AlgorithmProviderInstance() { BCryptCloseAlgorithmProvider(Handle, 0); }
};

class SHAWithBCrypt final : public Azure::Core::Cryptography::Hash {
private:
  std::string m_buffer;
  BCRYPT_HASH_HANDLE m_hashHandle = nullptr;
  std::size_t m_hashLength = 0;

  std::vector<uint8_t> OnFinal(const uint8_t* data, std::size_t length) override
  {
    OnAppend(data, length);

    std::vector<uint8_t> hash;
    hash.resize(m_hashLength);
    NTSTATUS status = BCryptFinishHash(
        m_hashHandle, reinterpret_cast<PUCHAR>(&hash[0]), static_cast<ULONG>(hash.size()), 0);
    if (!BCRYPT_SUCCESS(status))
    {
      throw std::runtime_error("BCryptFinishHash failed");
    }
    return hash;
  }

  void OnAppend(const uint8_t* data, std::size_t length) override
  {
    NTSTATUS status = BCryptHashData(
        m_hashHandle,
        reinterpret_cast<PBYTE>(const_cast<uint8_t*>(data)),
        static_cast<ULONG>(length),
        0);
    if (!BCRYPT_SUCCESS(status))
    {
      throw std::runtime_error("BCryptHashData failed");
    }
  }

public:
  SHAWithBCrypt(LPCWSTR hashAlgorithm)
  {
    AlgorithmProviderInstance algorithmProvider(hashAlgorithm);

    m_buffer.resize(algorithmProvider.ContextSize);
    m_hashLength = algorithmProvider.HashLength;

    NTSTATUS status = BCryptCreateHash(
        algorithmProvider.Handle,
        &m_hashHandle,
        reinterpret_cast<PUCHAR>(&m_buffer[0]),
        static_cast<ULONG>(m_buffer.size()),
        nullptr,
        0,
        0);
    if (!BCRYPT_SUCCESS(status))
    {
      throw std::runtime_error("BCryptCreateHash failed");
    }
  }

  ~SHAWithBCrypt() { BCryptDestroyHash(m_hashHandle); }
};

} // namespace

Azure::Security::KeyVault::SHA256::SHA256()
    : m_portableImplementation(std::make_unique<SHAWithBCrypt>(BCRYPT_SHA256_ALGORITHM))
{
}

Azure::Security::KeyVault::SHA384::SHA384()
    : m_portableImplementation(std::make_unique<SHAWithBCrypt>(BCRYPT_SHA384_ALGORITHM))
{
}

Azure::Security::KeyVault::SHA512::SHA512()
    : m_portableImplementation(std::make_unique<SHAWithBCrypt>(BCRYPT_SHA512_ALGORITHM))
{
}
#endif