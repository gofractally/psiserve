#include <pcrypt/sha256.hpp>

#ifdef __APPLE__
#include <CommonCrypto/CommonDigest.h>
#else
#include <openssl/sha.h>
#endif

namespace pcrypt
{

sha256_digest sha256(const void* data, size_t len)
{
   sha256_digest out;
#ifdef __APPLE__
   CC_SHA256(data, static_cast<CC_LONG>(len), out.data());
#else
   SHA256(static_cast<const unsigned char*>(data), len, out.data());
#endif
   return out;
}

}  // namespace pcrypt
