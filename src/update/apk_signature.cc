#include "update/apk_signature.h"

#include <fcntl.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace mocktail::update {
namespace {

constexpr std::uint32_t kZipEocdSignature = 0x06054b50U;
constexpr std::uint32_t kV2BlockId = 0x7109871aU;
constexpr std::uint32_t kV3BlockId = 0xf05368c0U;
constexpr std::uint32_t kV31BlockId = 0x1b93ad61U;
constexpr std::size_t kChunkBytes = 1024U * 1024U;
constexpr std::array<unsigned char, 16> kSigningMagic = {
    'A', 'P', 'K', ' ', 'S', 'i', 'g', ' ',
    'B', 'l', 'o', 'c', 'k', ' ', '4', '2'};

std::uint32_t U32(const unsigned char* value) {
  return value[0] | static_cast<std::uint32_t>(value[1]) << 8U |
         static_cast<std::uint32_t>(value[2]) << 16U |
         static_cast<std::uint32_t>(value[3]) << 24U;
}

std::uint64_t U64(const unsigned char* value) {
  return static_cast<std::uint64_t>(U32(value)) |
         static_cast<std::uint64_t>(U32(value + 4)) << 32U;
}

void PutU32(std::uint32_t value, unsigned char* output) {
  output[0] = static_cast<unsigned char>(value);
  output[1] = static_cast<unsigned char>(value >> 8U);
  output[2] = static_cast<unsigned char>(value >> 16U);
  output[3] = static_cast<unsigned char>(value >> 24U);
}

struct Span {
  const unsigned char* data = nullptr;
  std::size_t size = 0;
};

class Reader final {
 public:
  explicit Reader(Span span) : span_(span) {}

  bool U32Value(std::uint32_t* value) {
    if (remaining() < 4) return false;
    *value = U32(span_.data + position_);
    position_ += 4;
    return true;
  }

  bool LengthPrefixed(Span* value) {
    std::uint32_t size = 0;
    if (!U32Value(&size) || size > remaining()) return false;
    value->data = span_.data + position_;
    value->size = size;
    position_ += size;
    return true;
  }

  std::size_t remaining() const { return span_.size - position_; }

 private:
  Span span_;
  std::size_t position_ = 0;
};

struct Mapping final {
  int descriptor = -1;
  const unsigned char* bytes = nullptr;
  std::size_t size = 0;

  ~Mapping() {
    if (bytes != nullptr) munmap(const_cast<unsigned char*>(bytes), size);
    if (descriptor >= 0) close(descriptor);
  }

  Mapping(const Mapping&) = delete;
  Mapping& operator=(const Mapping&) = delete;
  Mapping() = default;
};

template <typename T, void (*Free)(T*)>
struct OpenSslDeleter {
  void operator()(T* value) const {
    if (value != nullptr) Free(value);
  }
};

using EvpKey =
    std::unique_ptr<EVP_PKEY, OpenSslDeleter<EVP_PKEY, EVP_PKEY_free>>;
using X509Certificate = std::unique_ptr<X509, OpenSslDeleter<X509, X509_free>>;
using DigestContext =
    std::unique_ptr<EVP_MD_CTX, OpenSslDeleter<EVP_MD_CTX, EVP_MD_CTX_free>>;

bool MapFile(const std::filesystem::path& path, Mapping* mapping,
             std::string* error) {
  mapping->descriptor = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (mapping->descriptor < 0) {
    *error = "cannot open APK for signature verification";
    return false;
  }
  struct stat metadata = {};
  if (fstat(mapping->descriptor, &metadata) != 0 ||
      !S_ISREG(metadata.st_mode) || metadata.st_size < 32 ||
      static_cast<std::uintmax_t>(metadata.st_size) >
          std::numeric_limits<std::size_t>::max()) {
    *error = "APK is not a bounded regular file";
    return false;
  }
  mapping->size = static_cast<std::size_t>(metadata.st_size);
  void* bytes = mmap(nullptr, mapping->size, PROT_READ, MAP_PRIVATE,
                     mapping->descriptor, 0);
  if (bytes == MAP_FAILED) {
    *error = "cannot map APK for signature verification";
    mapping->bytes = nullptr;
    return false;
  }
  mapping->bytes = static_cast<const unsigned char*>(bytes);
  return true;
}

bool FindEocd(const Mapping& mapping, std::size_t* eocd, std::string* error) {
  const std::size_t minimum = mapping.size > 65557U ? mapping.size - 65557U : 0;
  for (std::size_t position = mapping.size - 22U;; --position) {
    if (U32(mapping.bytes + position) == kZipEocdSignature) {
      const std::uint16_t comment =
          mapping.bytes[position + 20] |
          static_cast<std::uint16_t>(mapping.bytes[position + 21]) << 8U;
      if (position + 22U + comment == mapping.size) {
        *eocd = position;
        return true;
      }
    }
    if (position == minimum) break;
  }
  *error = "APK has no valid ZIP end record";
  return false;
}

struct SigningBlock {
  Span v2;
  Span v3;
  Span v31;
  std::size_t begin = 0;
  std::size_t central_directory = 0;
  std::size_t eocd = 0;
};

bool ParseSigningBlock(const Mapping& mapping, SigningBlock* block,
                       std::string* error) {
  if (!FindEocd(mapping, &block->eocd, error)) return false;
  block->central_directory = U32(mapping.bytes + block->eocd + 16U);
  if (block->central_directory < 32U ||
      block->central_directory > block->eocd ||
      std::memcmp(mapping.bytes + block->central_directory - 16U,
                  kSigningMagic.data(), kSigningMagic.size()) != 0) {
    *error = "APK has no Signature Scheme v2/v3 block";
    return false;
  }
  const std::uint64_t footer_size =
      U64(mapping.bytes + block->central_directory - 24U);
  if (footer_size < 24U || footer_size > block->central_directory - 8U ||
      footer_size > std::numeric_limits<std::size_t>::max() - 8U) {
    *error = "APK signing block size is invalid";
    return false;
  }
  const std::size_t total_size = static_cast<std::size_t>(footer_size) + 8U;
  block->begin = block->central_directory - total_size;
  if (U64(mapping.bytes + block->begin) != footer_size) {
    *error = "APK signing block size headers disagree";
    return false;
  }
  std::size_t cursor = block->begin + 8U;
  const std::size_t pairs_end = block->central_directory - 24U;
  while (cursor < pairs_end) {
    if (pairs_end - cursor < 12U) {
      *error = "APK signing block pair is truncated";
      return false;
    }
    const std::uint64_t pair_size = U64(mapping.bytes + cursor);
    cursor += 8U;
    if (pair_size < 4U || pair_size > pairs_end - cursor) {
      *error = "APK signing block pair has an invalid size";
      return false;
    }
    const std::uint32_t id = U32(mapping.bytes + cursor);
    const Span value{mapping.bytes + cursor + 4U,
                     static_cast<std::size_t>(pair_size - 4U)};
    Span* selected = nullptr;
    if (id == kV2BlockId) selected = &block->v2;
    if (id == kV3BlockId) selected = &block->v3;
    if (id == kV31BlockId) selected = &block->v31;
    if (selected != nullptr) {
      if (selected->data != nullptr) {
        *error = "APK contains a duplicate signature scheme block";
        return false;
      }
      *selected = value;
    }
    cursor += static_cast<std::size_t>(pair_size);
  }
  if (cursor != pairs_end ||
      (block->v2.data == nullptr && block->v3.data == nullptr &&
       block->v31.data == nullptr)) {
    *error = "APK has no supported signing scheme";
    return false;
  }
  return true;
}

struct SignatureAlgorithm {
  std::uint32_t id = 0;
  const EVP_MD* digest = nullptr;
  bool pss = false;
};

std::optional<SignatureAlgorithm> SupportedAlgorithm(std::uint32_t id) {
  switch (id) {
    case 0x0101:
      return SignatureAlgorithm{id, EVP_sha256(), true};
    case 0x0102:
      return SignatureAlgorithm{id, EVP_sha512(), true};
    case 0x0103:
      return SignatureAlgorithm{id, EVP_sha256(), false};
    case 0x0104:
      return SignatureAlgorithm{id, EVP_sha512(), false};
    case 0x0201:
      return SignatureAlgorithm{id, EVP_sha256(), false};
    case 0x0202:
      return SignatureAlgorithm{id, EVP_sha512(), false};
    case 0x0301:
      return SignatureAlgorithm{id, EVP_sha256(), false};
    default:
      return std::nullopt;
  }
}

struct IdBytes {
  std::uint32_t id = 0;
  Span bytes;
};

bool ParseIdSequence(Span sequence, std::vector<IdBytes>* values,
                     std::string* error) {
  Reader records(sequence);
  while (records.remaining() > 0) {
    Span record;
    if (!records.LengthPrefixed(&record)) {
      *error = "APK signer record is malformed";
      return false;
    }
    Reader fields(record);
    IdBytes value;
    if (!fields.U32Value(&value.id) || !fields.LengthPrefixed(&value.bytes) ||
        fields.remaining() != 0) {
      *error = "APK signer algorithm record is malformed";
      return false;
    }
    values->push_back(value);
  }
  return !values->empty();
}

std::string Hex(const unsigned char* bytes, std::size_t size) {
  constexpr char kHex[] = "0123456789abcdef";
  std::string output(size * 2U, '0');
  for (std::size_t index = 0; index < size; ++index) {
    output[index * 2U] = kHex[bytes[index] >> 4U];
    output[index * 2U + 1U] = kHex[bytes[index] & 0x0fU];
  }
  return output;
}

bool DigestParts(const EVP_MD* digest, const std::vector<Span>& parts,
                 std::vector<unsigned char>* output) {
  DigestContext context(EVP_MD_CTX_new());
  if (!context || EVP_DigestInit_ex(context.get(), digest, nullptr) != 1) {
    return false;
  }
  for (const Span& part : parts) {
    if (part.size > 0 &&
        EVP_DigestUpdate(context.get(), part.data, part.size) != 1) {
      return false;
    }
  }
  output->resize(static_cast<std::size_t>(EVP_MD_get_size(digest)));
  unsigned int size = 0;
  return EVP_DigestFinal_ex(context.get(), output->data(), &size) == 1 &&
         size == output->size();
}

bool ComputeContentDigest(const Mapping& mapping, const SigningBlock& block,
                          const EVP_MD* digest,
                          std::vector<unsigned char>* output,
                          std::string* error) {
  std::vector<unsigned char> patched_eocd(mapping.bytes + block.eocd,
                                          mapping.bytes + mapping.size);
  if (patched_eocd.size() < 20U || block.begin > 0xffffffffU) {
    *error = "APK is too large for Signature Scheme v2/v3";
    return false;
  }
  PutU32(static_cast<std::uint32_t>(block.begin), patched_eocd.data() + 16U);
  const std::vector<Span> sections = {
      {mapping.bytes, block.begin},
      {mapping.bytes + block.central_directory,
       block.eocd - block.central_directory},
      {patched_eocd.data(), patched_eocd.size()},
  };
  std::size_t chunk_count = 0;
  for (const Span& section : sections) {
    chunk_count += (section.size + kChunkBytes - 1U) / kChunkBytes;
  }
  if (chunk_count == 0 || chunk_count > 0xffffffffU) {
    *error = "APK content chunk count is invalid";
    return false;
  }
  const std::size_t digest_size =
      static_cast<std::size_t>(EVP_MD_get_size(digest));
  std::vector<unsigned char> chunk_digests;
  chunk_digests.reserve(chunk_count * digest_size);
  for (const Span& section : sections) {
    for (std::size_t offset = 0; offset < section.size; offset += kChunkBytes) {
      const std::size_t bytes = std::min(kChunkBytes, section.size - offset);
      std::array<unsigned char, 5> prefix{};
      prefix[0] = 0xa5;
      PutU32(static_cast<std::uint32_t>(bytes), prefix.data() + 1U);
      std::vector<unsigned char> chunk;
      if (!DigestParts(
              digest,
              {{prefix.data(), prefix.size()}, {section.data + offset, bytes}},
              &chunk)) {
        *error = "cannot hash APK content chunk";
        return false;
      }
      chunk_digests.insert(chunk_digests.end(), chunk.begin(), chunk.end());
    }
  }
  std::array<unsigned char, 5> prefix{};
  prefix[0] = 0x5a;
  PutU32(static_cast<std::uint32_t>(chunk_count), prefix.data() + 1U);
  if (!DigestParts(digest,
                   {{prefix.data(), prefix.size()},
                    {chunk_digests.data(), chunk_digests.size()}},
                   output)) {
    *error = "cannot hash APK content digest list";
    return false;
  }
  return true;
}

bool VerifySignedData(Span signed_data, Span signature,
                      const SignatureAlgorithm& algorithm, EVP_PKEY* key,
                      std::string* error) {
  DigestContext context(EVP_MD_CTX_new());
  EVP_PKEY_CTX* key_context = nullptr;
  if (!context || EVP_DigestVerifyInit(context.get(), &key_context,
                                       algorithm.digest, nullptr, key) != 1) {
    *error = "cannot initialise APK signer verification";
    return false;
  }
  if (algorithm.pss &&
      (EVP_PKEY_CTX_set_rsa_padding(key_context, RSA_PKCS1_PSS_PADDING) <= 0 ||
       EVP_PKEY_CTX_set_rsa_pss_saltlen(
           key_context, EVP_MD_get_size(algorithm.digest)) <= 0 ||
       EVP_PKEY_CTX_set_rsa_mgf1_md(key_context, algorithm.digest) <= 0)) {
    *error = "cannot configure APK RSA-PSS verification";
    return false;
  }
  if (EVP_DigestVerify(context.get(), signature.data, signature.size,
                       signed_data.data, signed_data.size) != 1) {
    *error = "APK signer signature verification failed";
    return false;
  }
  return true;
}

bool VerifySigner(const Mapping& mapping, const SigningBlock& block,
                  Span signer, bool v3, std::vector<std::string>* certificates,
                  std::string* error) {
  Reader signer_reader(signer);
  Span signed_data;
  Span signatures;
  Span public_key_bytes;
  std::uint32_t minimum_sdk = 0;
  std::uint32_t maximum_sdk = 0;
  if (!signer_reader.LengthPrefixed(&signed_data)) {
    *error = "APK signer has no signed data";
    return false;
  }
  if (v3 &&
      (!signer_reader.U32Value(&minimum_sdk) ||
       !signer_reader.U32Value(&maximum_sdk) || minimum_sdk > maximum_sdk)) {
    *error = "APK v3 signer SDK range is invalid";
    return false;
  }
  if (!signer_reader.LengthPrefixed(&signatures) ||
      !signer_reader.LengthPrefixed(&public_key_bytes) ||
      signer_reader.remaining() != 0) {
    *error = "APK signer fields are malformed (signer=" +
             std::to_string(signer.size) +
             ", signed=" + std::to_string(signed_data.size) +
             ", remaining=" + std::to_string(signer_reader.remaining()) + ")";
    return false;
  }

  Reader signed_reader(signed_data);
  Span digests;
  Span certificate_sequence;
  Span attributes;
  std::uint32_t signed_minimum_sdk = 0;
  std::uint32_t signed_maximum_sdk = 0;
  if (!signed_reader.LengthPrefixed(&digests)) {
    *error = "APK signed-data digest list is malformed";
    return false;
  }
  if (!signed_reader.LengthPrefixed(&certificate_sequence)) {
    *error = "APK signed-data certificate list is malformed";
    return false;
  }
  if (v3 && (!signed_reader.U32Value(&signed_minimum_sdk) ||
             !signed_reader.U32Value(&signed_maximum_sdk))) {
    *error = "APK v3 signed-data SDK range is malformed";
    return false;
  }
  if (!signed_reader.LengthPrefixed(&attributes)) {
    *error = "APK signed-data attributes are malformed";
    return false;
  }
  if (!v3 && signed_reader.remaining() != 0) {
    // Current AOSP apksig appends one reserved empty length-prefixed field to
    // v2 signed-data. Android's verifier ignores it after parsing attributes;
    // accept only that exact representation instead of arbitrary trailing data.
    Span reserved;
    if (!signed_reader.LengthPrefixed(&reserved) || reserved.size != 0) {
      *error = "APK v2 signed-data reserved field is malformed";
      return false;
    }
  }
  if (signed_reader.remaining() != 0) {
    *error = "APK signed-data has trailing fields: " +
             std::to_string(signed_reader.remaining());
    return false;
  }
  if (v3 && (minimum_sdk != signed_minimum_sdk ||
             maximum_sdk != signed_maximum_sdk)) {
    *error = "APK signed-data has a mismatched SDK range";
    return false;
  }

  std::vector<IdBytes> digest_records;
  std::vector<IdBytes> signature_records;
  if (!ParseIdSequence(digests, &digest_records, error) ||
      !ParseIdSequence(signatures, &signature_records, error)) {
    return false;
  }
  if (digest_records.size() != signature_records.size() ||
      !std::equal(digest_records.begin(), digest_records.end(),
                  signature_records.begin(),
                  [](const IdBytes& digest, const IdBytes& signature) {
                    return digest.id == signature.id;
                  })) {
    *error = "APK signer digest and signature algorithms disagree";
    return false;
  }
  std::optional<SignatureAlgorithm> selected;
  Span selected_signature;
  Span selected_digest;
  for (const IdBytes& signature_record : signature_records) {
    const auto algorithm = SupportedAlgorithm(signature_record.id);
    if (!algorithm.has_value()) continue;
    const auto digest_record = std::find_if(
        digest_records.begin(), digest_records.end(),
        [&](const auto& record) { return record.id == algorithm->id; });
    if (digest_record == digest_records.end()) continue;
    selected = algorithm;
    selected_signature = signature_record.bytes;
    selected_digest = digest_record->bytes;
    break;
  }
  if (!selected.has_value()) {
    *error = "APK signer has no supported matching signature and digest";
    return false;
  }

  const unsigned char* public_cursor = public_key_bytes.data;
  EvpKey public_key(d2i_PUBKEY(nullptr, &public_cursor, public_key_bytes.size));
  if (!public_key ||
      public_cursor != public_key_bytes.data + public_key_bytes.size) {
    *error = "APK signer public key is malformed";
    return false;
  }
  if (!VerifySignedData(signed_data, selected_signature, *selected,
                        public_key.get(), error)) {
    return false;
  }

  std::vector<unsigned char> calculated_digest;
  if (!ComputeContentDigest(mapping, block, selected->digest,
                            &calculated_digest, error) ||
      selected_digest.size != calculated_digest.size() ||
      CRYPTO_memcmp(selected_digest.data, calculated_digest.data(),
                    calculated_digest.size()) != 0) {
    if (error->empty()) *error = "APK content digest verification failed";
    return false;
  }

  Reader certificate_reader(certificate_sequence);
  bool first = true;
  while (certificate_reader.remaining() > 0) {
    Span encoded;
    if (!certificate_reader.LengthPrefixed(&encoded) || encoded.size == 0) {
      *error = "APK signer certificate sequence is malformed";
      return false;
    }
    const unsigned char* certificate_cursor = encoded.data;
    X509Certificate certificate(
        d2i_X509(nullptr, &certificate_cursor, encoded.size));
    if (!certificate || certificate_cursor != encoded.data + encoded.size) {
      *error = "APK signer certificate is malformed";
      return false;
    }
    const bool leaf = first;
    if (leaf) {
      EvpKey certificate_key(X509_get_pubkey(certificate.get()));
      if (!certificate_key ||
          EVP_PKEY_eq(certificate_key.get(), public_key.get()) != 1) {
        *error = "APK signer public key does not match its certificate";
        return false;
      }
      first = false;
    }
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int digest_size = 0;
    if (EVP_Digest(encoded.data, encoded.size, digest.data(), &digest_size,
                   EVP_sha256(), nullptr) != 1 ||
        digest_size != 32) {
      *error = "cannot hash APK signing certificate";
      return false;
    }
    if (leaf) certificates->push_back(Hex(digest.data(), digest_size));
  }
  if (first) {
    *error = "APK signer has no certificate";
    return false;
  }
  return true;
}

bool VerifyScheme(const Mapping& mapping, const SigningBlock& block,
                  Span scheme, bool v3, std::vector<std::string>* certificates,
                  std::string* error) {
  Reader scheme_reader(scheme);
  Span signer_sequence;
  if (!scheme_reader.LengthPrefixed(&signer_sequence) ||
      scheme_reader.remaining() != 0) {
    *error = "APK signature scheme signer container is malformed";
    return false;
  }
  Reader signers(signer_sequence);
  std::size_t signer_count = 0;
  while (signers.remaining() > 0) {
    Span signer;
    if (!signers.LengthPrefixed(&signer) || signer.size == 0 ||
        !VerifySigner(mapping, block, signer, v3, certificates, error)) {
      if (error->empty()) *error = "APK signer sequence is malformed";
      return false;
    }
    if (++signer_count > 16) {
      *error = "APK contains too many signers";
      return false;
    }
  }
  return signer_count > 0;
}

}  // namespace

ApkSignatureResult VerifyApkSignature(const std::filesystem::path& apk_path) {
  ApkSignatureResult result;
  Mapping mapping;
  if (!MapFile(apk_path, &mapping, &result.error)) return result;
  SigningBlock block;
  if (!ParseSigningBlock(mapping, &block, &result.error)) return result;

  // V2 is preferred because Roblox targets Android versions below 28 and its
  // APKs must carry this scheme. V3 is accepted for future exact profiles.
  const Span scheme = block.v2.data != nullptr
                          ? block.v2
                          : (block.v31.data != nullptr ? block.v31 : block.v3);
  const bool v3 = block.v2.data == nullptr;
  if (!VerifyScheme(mapping, block, scheme, v3, &result.certificate_sha256,
                    &result.error)) {
    result.certificate_sha256.clear();
    return result;
  }
  std::sort(result.certificate_sha256.begin(), result.certificate_sha256.end());
  result.certificate_sha256.erase(std::unique(result.certificate_sha256.begin(),
                                              result.certificate_sha256.end()),
                                  result.certificate_sha256.end());
  return result;
}

}  // namespace mocktail::update
