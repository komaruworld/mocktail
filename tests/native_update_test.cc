#include <gtest/gtest.h>
#include <minizip/zip.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <vector>

#include "update/apkpure_provider.h"
#include "update/http_download.h"
#include "update/payload_integrity.h"
#include "update/payload_store.h"
#include "update/readiness_canary.h"
#include "update/unsafe_latest_runner.h"
#include "update/update_config.h"
#include "update/zip_archive.h"

namespace mocktail::update {
namespace {

class TemporaryDirectory final {
 public:
  TemporaryDirectory() {
    char pattern[] = "/tmp/mocktail_native_update_XXXXXX";
    const char* created = mkdtemp(pattern);
    if (created != nullptr) root_ = created;
  }

  ~TemporaryDirectory() {
    std::error_code error;
    std::filesystem::remove_all(root_, error);
  }

  const std::filesystem::path& root() const { return root_; }

 private:
  std::filesystem::path root_;
};

void Write(const std::filesystem::path& path, std::string_view contents) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream(path, std::ios::binary) << contents;
}

std::string ReadFile(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(input),
                     std::istreambuf_iterator<char>());
}

std::string MetadataRecord(std::string_view version_name,
                           std::uint64_t version_code,
                           std::string_view artifact, std::string_view url) {
  const std::string code = std::to_string(version_code);
  std::string record("protobuf-prefix\0", 16);
  record.push_back('\x2a');
  record.push_back(static_cast<char>(code.size()));
  record += code;
  record.push_back('\x32');
  record.push_back(static_cast<char>(version_name.size()));
  record += version_name;
  record += ":metadata";
  record.push_back('\0');
  record += artifact;
  record.append("\0\0", 2);
  record += url;
  record.push_back('\0');
  return record;
}

bool CreateZip(const std::filesystem::path& path) {
  zipFile archive = zipOpen64(path.c_str(), APPEND_STATUS_CREATE);
  if (archive == nullptr) return false;
  const std::vector<std::pair<std::string, std::string>> entries = {
      {"ignored.txt", "ignored"},
      {"assets/content/first.txt", "first"},
      {"assets/nested/second.txt", "second"},
  };
  for (const auto& [name, contents] : entries) {
    if (zipOpenNewFileInZip64(archive, name.c_str(), nullptr, nullptr, 0,
                              nullptr, 0, nullptr, Z_DEFLATED,
                              Z_DEFAULT_COMPRESSION, 1) != ZIP_OK ||
        zipWriteInFileInZip(archive, contents.data(), contents.size()) !=
            ZIP_OK ||
        zipCloseFileInZip(archive) != ZIP_OK) {
      zipClose(archive, nullptr);
      return false;
    }
  }
  return zipClose(archive, nullptr) == ZIP_OK;
}

TEST(NativeUpdateConfigTest, ReadsOnlyTypedUpdaterSection) {
  TemporaryDirectory temporary;
  const auto config = temporary.root() / "config.yaml";
  Write(config,
        "version: 1\n"
        "device: pc-windows-11\n"
        "updates:\n"
        "  automatic: false\n"
        "  source: apk-pure\n"
        "  launch_after_update: true\n");
  const UpdateConfigResult result = LoadUpdateConfig(config);
  ASSERT_TRUE(result) << result.error;
  EXPECT_TRUE(result.file_loaded);
  EXPECT_FALSE(result.config.automatic);
  EXPECT_TRUE(result.config.launch_after_update);
  EXPECT_EQ(result.config.source, "apk-pure");
  EXPECT_TRUE(result.warnings.empty());
}

TEST(NativeUpdateConfigTest, IgnoresDeprecatedTestingLatestOnlyWithWarning) {
  TemporaryDirectory temporary;
  const auto config = temporary.root() / "config.yaml";
  Write(config,
        "version: 1\nupdates:\n  testing_latest_only: true\n");
  const UpdateConfigResult result = LoadUpdateConfig(config);
  ASSERT_TRUE(result) << result.error;
  ASSERT_EQ(result.warnings.size(), 1U);
  EXPECT_NE(result.warnings.front().find("no longer supported"),
            std::string::npos);
}

TEST(NativeUpdateConfigTest, MissingFileUsesSafeDefaults) {
  TemporaryDirectory temporary;
  const UpdateConfigResult result =
      LoadUpdateConfig(temporary.root() / "missing.yaml");
  ASSERT_TRUE(result) << result.error;
  EXPECT_FALSE(result.file_loaded);
  EXPECT_TRUE(result.config.automatic);
  EXPECT_EQ(result.config.source, "apk-pure");
}

TEST(NativeUpdateConfigTest, RejectsUnsupportedSource) {
  TemporaryDirectory temporary;
  const auto config = temporary.root() / "config.yaml";
  Write(config,
        "version: 1\nupdates:\n  source: google-play\n");
  const UpdateConfigResult result = LoadUpdateConfig(config);
  EXPECT_FALSE(result);
  EXPECT_NE(result.error.find("apk-pure"), std::string::npos);
}

TEST(ApkPureProviderTest, ParsesExactVersionIdentityAndTrustedUrl) {
  const std::string metadata =
      MetadataRecord("2.727.1199", 2628, "XAPKJ",
                     "https://download.pureapk.com/b/XAPK/roblox");
  const ProviderVersion latest = ParseApkPureLatestMetadata(metadata);
  ASSERT_TRUE(latest) << latest.error;
  EXPECT_EQ(latest.version_name, "2.727.1199");
  EXPECT_EQ(latest.version_code, 2628);
  std::string error;
  const auto urls =
      ParseApkPureExactDownloadUrls(metadata, "2.727.1199", &error);
  ASSERT_TRUE(error.empty()) << error;
  ASSERT_EQ(urls.size(), 1);
  EXPECT_EQ(urls.front(), "https://download.pureapk.com/b/XAPK/roblox");
}

TEST(UnsafeLatestRunnerTest, LaunchesCandidateWithUnapprovedEnvironment) {
  TemporaryDirectory temporary;
  const std::filesystem::path output = temporary.root() / "environment.txt";
  const std::filesystem::path arguments = temporary.root() / "arguments.txt";
  const std::filesystem::path runtime = temporary.root() / "runtime.sh";
  Write(runtime,
        "#!/bin/sh\n"
        "env | sort > \"$ENV_OUTPUT\"\n"
        "printf '%s\\n' \"$@\" > \"$ARGUMENT_OUTPUT\"\n");
  ASSERT_EQ(chmod(runtime.c_str(), 0700), 0);

  const std::filesystem::path payload = temporary.root() / "payload";
  Write(payload / "libroblox.so", "candidate");
  std::filesystem::create_directories(payload / "assets/content");
  const std::filesystem::path compatibility =
      temporary.root() / "compatibility.json";
  const std::filesystem::path profile = temporary.root() / "profile.json";
  Write(compatibility, "{}\n");
  Write(profile, "{}\n");

  UnsafeLatestRunOptions options;
  options.runtime_binary = runtime;
  options.payload_directory = payload;
  options.compatibility_manifest = compatibility;
  options.host_abi_profile = profile;
  options.inherited_environment = {
      "PATH=/usr/bin:/bin", "ENV_OUTPUT=" + output.string(),
      "ARGUMENT_OUTPUT=" + arguments.string(), "UNCHANGED=value",
      "ROBLOX_LIB_PATH=/stale/libroblox.so",
      "MOCKTAIL_HOST_ABI_APPROVAL_RECEIPT=/stale/approval.json",
      "MOCKTAIL_SKIP_UPDATE_CHECK=0",
  };

  const UnsafeLatestRunResult result = RunUnsafeLatestCandidate(options);
  ASSERT_TRUE(result) << result.error;
  const std::string environment = ReadFile(output);
  EXPECT_NE(environment.find("UNCHANGED=value\n"), std::string::npos);
  EXPECT_NE(environment.find("ROBLOX_LIB_PATH=" +
                             (payload / "libroblox.so").string() + "\n"),
            std::string::npos);
  EXPECT_NE(environment.find("MOCKTAIL_COMPATIBILITY_MANIFEST=" +
                             compatibility.string() + "\n"),
            std::string::npos);
  EXPECT_NE(environment.find("MOCKTAIL_HOST_ABI_PROFILE_FILE=" +
                             profile.string() + "\n"),
            std::string::npos);
  EXPECT_NE(environment.find("MOCKTAIL_HOST_ABI_CANARY=1\n"),
            std::string::npos);
  EXPECT_NE(environment.find("MOCKTAIL_ALLOW_CANDIDATE_HOST_ABI=1\n"),
            std::string::npos);
  EXPECT_NE(environment.find("MOCKTAIL_SKIP_UPDATE_CHECK=1\n"),
            std::string::npos);
  EXPECT_NE(environment.find("MOCKTAIL_UNSAFE_LATEST=1\n"),
            std::string::npos);
  EXPECT_EQ(environment.find("/stale/libroblox.so"), std::string::npos);
  EXPECT_EQ(environment.find("/stale/approval.json"), std::string::npos);
  EXPECT_EQ(ReadFile(arguments), "--windowed\n--allow-unverified-build\n");
}

TEST(HttpDownloadPolicyTest, RejectsCredentialsAndHostSuffixTricks) {
  const std::vector<std::string> hosts = {"pureapk.com"};
  EXPECT_TRUE(IsTrustedHttpsUrl("https://download.pureapk.com/a", hosts));
  EXPECT_FALSE(IsTrustedHttpsUrl("http://download.pureapk.com/a", hosts));
  EXPECT_FALSE(IsTrustedHttpsUrl("https://pureapk.com.attacker.test/a", hosts));
  EXPECT_FALSE(IsTrustedHttpsUrl("https://user@pureapk.com/a", hosts));
}

TEST(ReadinessCanaryTest, AcceptsRealPresentWithoutShaderPackSummary) {
  const std::string valid =
      "[compat] legacy binary patches: disabled\n"
      "[compat] signal-recovery handler disabled\n"
      "[compat] native allocator retained; host allocator bridges disabled\n"
      "[window] vkQueuePresentKHR #240 window=0x1234\n"
      "[window] first Roblox Vulkan frame presented\n"
      "[vulkan] SDL WSI adapter shut down\n"
      "[main] Roblox lifecycle shutdown: Stopped\n";
  std::string error;
  EXPECT_TRUE(ValidateReadinessLog(CanaryGraphicsBackend::kDirectVulkan,
                                   valid, &error))
      << error;
  error.clear();
  EXPECT_FALSE(ValidateReadinessLog(CanaryGraphicsBackend::kDirectVulkan,
                                    valid + "[FATAL] crash\n", &error));

  const std::string missing_real_present =
      "[compat] legacy binary patches: disabled\n"
      "[compat] signal-recovery handler disabled\n"
      "[compat] native allocator retained; host allocator bridges disabled\n"
      "Loaded 2988 shaders from pack vulkan_mobile\n"
      "[window] first Roblox Vulkan frame presented\n"
      "[vulkan] SDL WSI adapter shut down\n"
      "[main] Roblox lifecycle shutdown: Stopped\n";
  error.clear();
  EXPECT_FALSE(ValidateReadinessLog(CanaryGraphicsBackend::kDirectVulkan,
                                    missing_real_present, &error));
  EXPECT_NE(error.find("real queue present"), std::string::npos);
}

TEST(ReadinessCanaryTest, ValidatesRealOpenGlEsThreeSwap) {
  const std::string valid =
      "[compat] legacy binary patches: disabled\n"
      "[compat] signal-recovery handler disabled\n"
      "[compat] native allocator retained; host allocator bridges disabled\n"
      "[window] OpenGL ES context version=3.2\n"
      "[window] EGL context and surface initialized via SDL3\n"
      "[window] SwapBuffers #1 window=0x1234\n"
      "[window] first Roblox frame presented\n"
      "[main] Roblox lifecycle shutdown: Stopped\n";
  std::string error;
  EXPECT_TRUE(ValidateReadinessLog(CanaryGraphicsBackend::kOpenGlEs, valid,
                                   &error))
      << error;

  std::string gles2 = valid;
  gles2.replace(gles2.find("version=3.2"), std::string("version=3.2").size(),
                "version=2.0");
  error.clear();
  EXPECT_FALSE(ValidateReadinessLog(CanaryGraphicsBackend::kOpenGlEs, gles2,
                                    &error));
  EXPECT_NE(error.find("version=3."), std::string::npos);

  std::string missing_swap = valid;
  const std::string swap_marker =
      "[window] SwapBuffers #1 window=0x1234\n";
  missing_swap.erase(missing_swap.find(swap_marker), swap_marker.size());
  error.clear();
  EXPECT_FALSE(ValidateReadinessLog(CanaryGraphicsBackend::kOpenGlEs,
                                    missing_swap, &error));
  EXPECT_NE(error.find("real EGL swap"), std::string::npos);
}

TEST(ReadinessCanaryTest, MapsWindowBackendsToTheirPresentPath) {
  CanaryGraphicsBackend backend = CanaryGraphicsBackend::kDirectVulkan;
  EXPECT_TRUE(ParseCanaryGraphicsBackend("direct-vulkan", &backend));
  EXPECT_EQ(backend, CanaryGraphicsBackend::kDirectVulkan);
  EXPECT_TRUE(ParseCanaryGraphicsBackend("opengl", &backend));
  EXPECT_EQ(backend, CanaryGraphicsBackend::kOpenGlEs);
  EXPECT_TRUE(ParseCanaryGraphicsBackend("angle-vulkan", &backend));
  EXPECT_EQ(backend, CanaryGraphicsBackend::kAngleVulkan);
  EXPECT_EQ(CanaryGraphicsBackendName(backend), "angle-vulkan");
  EXPECT_FALSE(ParseCanaryGraphicsBackend("unknown", &backend));
}

TEST(ZipArchiveTest, ExtractsSelectedPrefixInOneSequentialPass) {
  TemporaryDirectory temporary;
  const std::filesystem::path archive = temporary.root() / "fixture.zip";
  ASSERT_TRUE(CreateZip(archive));
  std::size_t files = 0;
  std::string error;
  const std::filesystem::path output = temporary.root() / "output";
  ASSERT_TRUE(
      ExtractZipPrefix(archive, "assets/", output, 1024, &files, &error))
      << error;
  EXPECT_EQ(files, 2);
  EXPECT_EQ(ReadFile(output / "content/first.txt"), "first");
  EXPECT_EQ(ReadFile(output / "nested/second.txt"), "second");
  EXPECT_FALSE(std::filesystem::exists(output / "ignored.txt"));
}

TEST(PayloadStoreTest, StagesAndPromotesVerifiedExactPayload) {
  TemporaryDirectory temporary;
  const std::filesystem::path prepared = temporary.root() / "prepared";
  Write(prepared / "libroblox.so", "native-library");
  Write(prepared / "sober_apk/base.apk", "base-apk");
  Write(prepared / "sober_apk/split_config.x86_64.apk", "split-apk");
  Write(prepared / "assets/content/fixture", "asset");
  std::string error;
  std::size_t asset_count = 0;
  const std::string asset_hash =
      HashAssetTree(prepared / "assets", &asset_count, &error);
  ASSERT_TRUE(error.empty()) << error;
  constexpr std::string_view kBuildId =
      "1686400865ae0e408cd7bd67de7a439625c6fd13";
  nlohmann::json metadata = {
      {"schema_version", 1},
      {"package", "com.roblox.client"},
      {"version_name", "2.727.1199"},
      {"version_code", 2628},
      {"elf_build_id", kBuildId},
      {"sha256",
       {{"libroblox", HashRegularFile(prepared / "libroblox.so")},
        {"base_apk", HashRegularFile(prepared / "sober_apk/base.apk")},
        {"x86_64_split_apk",
         HashRegularFile(prepared / "sober_apk/split_config.x86_64.apk")}}},
      {"assets", {{"file_count", asset_count}, {"sha256_tree", asset_hash}}},
  };
  Write(prepared / "roblox_payload.json", metadata.dump(2) + "\n");
  const std::filesystem::path compatibility =
      temporary.root() / "compatibility.json";
  Write(compatibility,
        "{\"schema_version\":1,\"profiles\":[{"
        "\"version_name\":\"2.727.1199\",\"version_code\":2628,"
        "\"elf_build_id\":\"1686400865ae0e408cd7bd67de7a439625c6fd13\","
        "\"status\":\"supported\",\"default_allowed\":true,"
        "\"allow_legacy_binary_patches\":false}]}\n");
  PayloadStore store(temporary.root() / "store", compatibility);
  const PayloadStoreResult staged = store.Stage(prepared);
  ASSERT_TRUE(staged) << staged.error;
  const PayloadStoreResult promoted = store.Promote(staged.payload_id);
  ASSERT_TRUE(promoted) << promoted.error;
  const PayloadStoreResult current = store.VerifyCurrent();
  ASSERT_TRUE(current) << current.error;
  EXPECT_EQ(current.payload_id,
            "2628-1686400865ae0e408cd7bd67de7a439625c6fd13");
  const nlohmann::json activation = nlohmann::json::parse(
      ReadFile(temporary.root() / "store/current.json"));
  EXPECT_EQ(activation["version_name"], metadata["version_name"]);
  EXPECT_EQ(activation["version_code"], metadata["version_code"]);
  EXPECT_EQ(activation["elf_build_id"], metadata["elf_build_id"]);
  EXPECT_EQ(activation["payload_sha256"],
            metadata["sha256"]["libroblox"]);

  nlohmann::json tampered_activation = activation;
  tampered_activation["payload_sha256"] = std::string(64, '0');
  Write(temporary.root() / "store/current.json",
        tampered_activation.dump(2) + "\n");
  const PayloadStoreResult tampered = store.VerifyCurrent();
  EXPECT_FALSE(tampered);
  EXPECT_EQ(tampered.error,
            "active payload manifest hash does not match payload");
}

TEST(PayloadStoreTest, RestagesReadOnlyCorruptPayloadCollision) {
  TemporaryDirectory temporary;
  const std::filesystem::path prepared = temporary.root() / "prepared";
  Write(prepared / "libroblox.so", "native-library");
  Write(prepared / "sober_apk/base.apk", "base-apk");
  Write(prepared / "sober_apk/split_config.x86_64.apk", "split-apk");
  Write(prepared / "assets/content/fixture", "asset");
  std::string error;
  std::size_t asset_count = 0;
  const std::string asset_hash =
      HashAssetTree(prepared / "assets", &asset_count, &error);
  ASSERT_TRUE(error.empty()) << error;
  constexpr std::string_view kBuildId =
      "1686400865ae0e408cd7bd67de7a439625c6fd13";
  const nlohmann::json metadata = {
      {"schema_version", 1},
      {"package", "com.roblox.client"},
      {"version_name", "2.727.1199"},
      {"version_code", 2628},
      {"elf_build_id", kBuildId},
      {"sha256",
       {{"libroblox", HashRegularFile(prepared / "libroblox.so")},
        {"base_apk", HashRegularFile(prepared / "sober_apk/base.apk")},
        {"x86_64_split_apk",
         HashRegularFile(prepared / "sober_apk/split_config.x86_64.apk")}}},
      {"assets", {{"file_count", asset_count}, {"sha256_tree", asset_hash}}},
  };
  Write(prepared / "roblox_payload.json", metadata.dump(2) + "\n");

  const std::filesystem::path store_root = temporary.root() / "store";
  PayloadStore store(store_root, temporary.root() / "compatibility.json");
  const PayloadStoreResult initial = store.Stage(prepared);
  ASSERT_TRUE(initial) << initial.error;

  const std::filesystem::path stored_metadata =
      initial.payload_directory / "roblox_payload.json";
  std::error_code filesystem_error;
  std::filesystem::permissions(stored_metadata,
                               std::filesystem::perms::owner_write,
                               std::filesystem::perm_options::add,
                               filesystem_error);
  ASSERT_FALSE(filesystem_error);
  Write(stored_metadata, "corrupt\n");
  std::filesystem::permissions(stored_metadata,
                               std::filesystem::perms::owner_read,
                               std::filesystem::perm_options::replace,
                               filesystem_error);
  ASSERT_FALSE(filesystem_error);

  const PayloadStoreResult recovered = store.Stage(prepared);
  ASSERT_TRUE(recovered) << recovered.error;
  EXPECT_TRUE(VerifyPreparedPayload(recovered.payload_directory));

  const std::filesystem::path quarantine = store_root / "quarantine";
  std::filesystem::directory_iterator iterator(quarantine, filesystem_error);
  ASSERT_FALSE(filesystem_error);
  ASSERT_NE(iterator, std::filesystem::directory_iterator());
  const auto quarantined_status = iterator->symlink_status(filesystem_error);
  ASSERT_FALSE(filesystem_error);
  EXPECT_EQ(quarantined_status.permissions() &
                std::filesystem::perms::owner_write,
            std::filesystem::perms::none);
  ++iterator;
  EXPECT_EQ(iterator, std::filesystem::directory_iterator());
}

// Promotion is separated from staging by the readiness canaries, which run the
// real client against the staged directory in a child process. A candidate
// modified in that window must never become current, so promotion rehashes the
// immutable bytes even though the very same instance staged them.
TEST(PayloadStoreTest, PromoteRehashesBytesTamperedAfterStaging) {
  TemporaryDirectory temporary;
  const std::filesystem::path prepared = temporary.root() / "prepared";
  Write(prepared / "libroblox.so", "native-library");
  Write(prepared / "sober_apk/base.apk", "base-apk");
  Write(prepared / "sober_apk/split_config.x86_64.apk", "split-apk");
  Write(prepared / "assets/content/fixture", "asset");
  std::string error;
  std::size_t asset_count = 0;
  const std::string asset_hash =
      HashAssetTree(prepared / "assets", &asset_count, &error);
  ASSERT_TRUE(error.empty()) << error;
  const nlohmann::json metadata = {
      {"schema_version", 1},
      {"package", "com.roblox.client"},
      {"version_name", "2.727.1199"},
      {"version_code", 2628},
      {"elf_build_id", "1686400865ae0e408cd7bd67de7a439625c6fd13"},
      {"sha256",
       {{"libroblox", HashRegularFile(prepared / "libroblox.so")},
        {"base_apk", HashRegularFile(prepared / "sober_apk/base.apk")},
        {"x86_64_split_apk",
         HashRegularFile(prepared / "sober_apk/split_config.x86_64.apk")}}},
      {"assets", {{"file_count", asset_count}, {"sha256_tree", asset_hash}}},
  };
  Write(prepared / "roblox_payload.json", metadata.dump(2) + "\n");
  const std::filesystem::path compatibility =
      temporary.root() / "compatibility.json";
  Write(compatibility,
        "{\"schema_version\":1,\"profiles\":[{"
        "\"version_name\":\"2.727.1199\",\"version_code\":2628,"
        "\"elf_build_id\":\"1686400865ae0e408cd7bd67de7a439625c6fd13\","
        "\"status\":\"supported\",\"default_allowed\":true,"
        "\"allow_legacy_binary_patches\":false}]}\n");

  const std::filesystem::path store_root = temporary.root() / "store";
  PayloadStore store(store_root, compatibility);
  const PayloadStoreResult staged = store.Stage(prepared);
  ASSERT_TRUE(staged) << staged.error;

  // Stands in for the canary interval. Content bytes only: the metadata still
  // describes the original payload, so nothing short of hashing the contents
  // can detect this.
  const std::filesystem::path asset =
      staged.payload_directory / "assets/content/fixture";
  std::error_code filesystem_error;
  std::filesystem::permissions(asset, std::filesystem::perms::owner_write,
                               std::filesystem::perm_options::add,
                               filesystem_error);
  ASSERT_FALSE(filesystem_error);
  Write(asset, "tampered");

  const PayloadStoreResult promoted = store.Promote(staged.payload_id);
  EXPECT_FALSE(promoted);
  EXPECT_EQ(promoted.error, "payload asset tree does not match metadata");
  EXPECT_FALSE(std::filesystem::exists(store_root / "current.json",
                                       filesystem_error));
}

// The same guarantee holds for a library modified in that window.
TEST(PayloadStoreTest, PromoteRejectsALibraryTamperedAfterStaging) {
  TemporaryDirectory temporary;
  const std::filesystem::path prepared = temporary.root() / "prepared";
  Write(prepared / "libroblox.so", "native-library");
  Write(prepared / "sober_apk/base.apk", "base-apk");
  Write(prepared / "sober_apk/split_config.x86_64.apk", "split-apk");
  Write(prepared / "assets/content/fixture", "asset");
  std::string error;
  std::size_t asset_count = 0;
  const std::string asset_hash =
      HashAssetTree(prepared / "assets", &asset_count, &error);
  ASSERT_TRUE(error.empty()) << error;
  const nlohmann::json metadata = {
      {"schema_version", 1},
      {"package", "com.roblox.client"},
      {"version_name", "2.727.1199"},
      {"version_code", 2628},
      {"elf_build_id", "1686400865ae0e408cd7bd67de7a439625c6fd13"},
      {"sha256",
       {{"libroblox", HashRegularFile(prepared / "libroblox.so")},
        {"base_apk", HashRegularFile(prepared / "sober_apk/base.apk")},
        {"x86_64_split_apk",
         HashRegularFile(prepared / "sober_apk/split_config.x86_64.apk")}}},
      {"assets", {{"file_count", asset_count}, {"sha256_tree", asset_hash}}},
  };
  Write(prepared / "roblox_payload.json", metadata.dump(2) + "\n");
  const std::filesystem::path compatibility =
      temporary.root() / "compatibility.json";
  Write(compatibility,
        "{\"schema_version\":1,\"profiles\":[{"
        "\"version_name\":\"2.727.1199\",\"version_code\":2628,"
        "\"elf_build_id\":\"1686400865ae0e408cd7bd67de7a439625c6fd13\","
        "\"status\":\"supported\",\"default_allowed\":true,"
        "\"allow_legacy_binary_patches\":false}]}\n");

  const std::filesystem::path store_root = temporary.root() / "store";
  PayloadStore store(store_root, compatibility);
  const PayloadStoreResult staged = store.Stage(prepared);
  ASSERT_TRUE(staged) << staged.error;

  const std::filesystem::path library =
      staged.payload_directory / "libroblox.so";
  std::error_code filesystem_error;
  std::filesystem::permissions(library, std::filesystem::perms::owner_write,
                               std::filesystem::perm_options::add,
                               filesystem_error);
  ASSERT_FALSE(filesystem_error);
  Write(library, "backdoored");

  const PayloadStoreResult promoted = store.Promote(staged.payload_id);
  EXPECT_FALSE(promoted);
  EXPECT_EQ(promoted.error, "payload hash mismatch: libroblox.so");
  EXPECT_FALSE(std::filesystem::exists(store_root / "current.json",
                                       filesystem_error));
}

TEST(PayloadStoreTest, EmptyResultCannotReportFalsePromotion) {
  const PayloadStoreResult result;
  EXPECT_FALSE(result);
}

TEST(PayloadStoreTest, RequiresTwoRuntimeBoundCanariesForLatestCandidate) {
  TemporaryDirectory temporary;
  const std::filesystem::path prepared = temporary.root() / "prepared";
  Write(prepared / "libroblox.so", "new-native-library");
  Write(prepared / "sober_apk/base.apk", "new-base-apk");
  Write(prepared / "sober_apk/split_config.x86_64.apk", "new-split-apk");
  Write(prepared / "assets/content/fixture", "new-asset");
  std::string error;
  std::size_t asset_count = 0;
  const std::string asset_hash =
      HashAssetTree(prepared / "assets", &asset_count, &error);
  ASSERT_TRUE(error.empty()) << error;
  constexpr std::string_view kBuildId =
      "a6c1f5c57f9d7aa3fb99a5fa30565e07e5c88f6a";
  const nlohmann::json metadata = {
      {"schema_version", 1},
      {"package", "com.roblox.client"},
      {"version_name", "2.732.1043"},
      {"version_code", 2814},
      {"abi", "x86_64"},
      {"elf_build_id", kBuildId},
      {"sha256",
       {{"libroblox", HashRegularFile(prepared / "libroblox.so")},
        {"base_apk", HashRegularFile(prepared / "sober_apk/base.apk")},
        {"x86_64_split_apk",
         HashRegularFile(prepared / "sober_apk/split_config.x86_64.apk")}}},
      {"assets", {{"file_count", asset_count}, {"sha256_tree", asset_hash}}},
  };
  Write(prepared / "roblox_payload.json", metadata.dump(2) + "\n");
  const std::filesystem::path compatibility =
      temporary.root() / "repository-compatibility.json";
  Write(compatibility,
        "{\"schema_version\":1,\"profiles\":[{"
        "\"version_name\":\"2.727.1199\",\"version_code\":2628,"
        "\"elf_build_id\":\"1686400865ae0e408cd7bd67de7a439625c6fd13\","
        "\"status\":\"supported\",\"default_allowed\":true,"
        "\"allow_legacy_binary_patches\":false}]}\n");
  std::error_code filesystem_error;
  const std::filesystem::path runtime =
      std::filesystem::canonical("/proc/self/exe", filesystem_error);
  ASSERT_FALSE(filesystem_error);
  PayloadStore store(temporary.root() / "store", compatibility, runtime);
  const PayloadStoreResult staged = store.Stage(prepared);
  ASSERT_TRUE(staged) << staged.error;

  const std::string payload_id =
      "2814-a6c1f5c57f9d7aa3fb99a5fa30565e07e5c88f6a";
  const std::filesystem::path profile = temporary.root() / "candidate.json";
  Write(profile,
        nlohmann::json(
            {{"schema_version", 1},
             {"elf_build_id", kBuildId},
             {"payload_sha256", metadata["sha256"]["libroblox"]},
             {"payload_id", payload_id},
             {"payload_path", "payloads/" + payload_id},
             {"reference",
              {{"elf_build_id", "1686400865ae0e408cd7bd67de7a439625c6fd13"},
               {"payload_sha256",
                "3e9c26c81186f93458ff65d8a8bc240c220974db02b8"
                "388b91340b77b336997d"}}},
             {"profile", {{"elf_build_id", kBuildId}}},
             {"derivation_anchors", {{"signature_version", 1}}}})
                .dump(2) +
            "\n");
  const std::filesystem::path candidate_compatibility =
      temporary.root() / "candidate-compatibility.json";
  Write(candidate_compatibility,
        nlohmann::json(
            {{"schema_version", 1},
             {"profiles", nlohmann::json::array(
                              {{{"version_name", "2.732.1043"},
                                {"version_code", 2814},
                                {"elf_build_id", kBuildId},
                                {"status", "experimental"},
                                {"default_allowed", true},
                                {"allow_legacy_binary_patches", false},
                                {"allow_host_abi_bridges", true},
                                {"allow_host_constructor_replay", true}}})}})
                .dump(2) +
            "\n");
  const std::array<std::filesystem::path, 2> canary_logs = {
      temporary.root() / "canary-1.log",
      temporary.root() / "canary-2.log",
  };
  Write(canary_logs[0], "first independent Tier C pass\n");
  Write(canary_logs[1], "second independent Tier C pass\n");

  const PayloadStoreResult promoted = store.PromoteProbation(
      staged.payload_id, profile, candidate_compatibility, canary_logs);
  ASSERT_TRUE(promoted) << promoted.error;
  EXPECT_FALSE(promoted.approval_receipt.empty());
  const PayloadStoreResult current = store.VerifyCurrent();
  ASSERT_TRUE(current) << current.error;
  EXPECT_EQ(current.payload_id, payload_id);
  EXPECT_EQ(current.approval_receipt, promoted.approval_receipt);

  // A Mocktail rebuild does not invalidate a canary result for unchanged
  // Roblox bytes. The immutable receipt still binds the two original canary
  // runs, payload, profile, and compatibility manifest to one evidence set.
  nlohmann::json legacy_activation = nlohmann::json::parse(
      ReadFile(temporary.root() / "store/current.json"));
  legacy_activation.erase("payload_sha256");
  Write(temporary.root() / "store/current.json",
        legacy_activation.dump(2) + "\n");
  PayloadStore rebuilt_runtime_store(temporary.root() / "store", compatibility);
  const PayloadStoreResult cached = rebuilt_runtime_store.VerifyCurrent();
  ASSERT_TRUE(cached) << cached.error;
  EXPECT_EQ(cached.payload_id, payload_id);
  EXPECT_EQ(cached.approval_receipt, promoted.approval_receipt);
  const nlohmann::json activation = nlohmann::json::parse(
      ReadFile(temporary.root() / "store/current.json"));
  EXPECT_EQ(activation["payload_sha256"],
            metadata["sha256"]["libroblox"]);
}

}  // namespace
}  // namespace mocktail::update
