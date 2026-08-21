#include "runtime/command_line.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <string>
#include <vector>

namespace mocktail {
namespace runtime {
namespace {

TEST(CommandLineTest, ParsesCompleteRunContractOnce) {
  const std::array<const char*, 11> arguments = {
      "mocktail",
      "--roblox-lib",
      "/payload/libroblox.so",
      "--headless",
      "--graphics",
      "direct-vulkan",
      "--windowed",
      "--allow-unverified-build",
      "--launch-uri",
      "roblox://experiences/start?placeId=17580461965",
      nullptr};

  const CommandLineParseResult result = ParseCommandLine(10, arguments.data());

  ASSERT_TRUE(result);
  EXPECT_EQ(result.options.mode, CommandMode::kRun);
  EXPECT_EQ(result.options.roblox_library_path, "/payload/libroblox.so");
  EXPECT_EQ(result.options.graphics_backend, "direct-vulkan");
  EXPECT_EQ(result.options.window_mode, WindowMode::kWindowed);
  EXPECT_TRUE(result.options.allow_unverified_build);
  EXPECT_EQ(result.options.raw_launch_argument,
            "roblox://experiences/start?placeId=17580461965");
  EXPECT_FALSE(result.options.launch_request_json.empty());
  EXPECT_EQ(result.options.launch_argument_index, 9);
}

TEST(CommandLineTest, PreservesDesktopLaunchUriAsOneOpaqueArgument) {
  constexpr char kLaunchUri[] =
      "roblox://experiences/start?placeId=1962086868&"
      "gameInstanceId=97ea-49e4&launchData=%257B%2522key%2522%253A%2522a%"
      "252Bb%2522%257D";
  const std::array<const char*, 3> arguments = {"mocktail", "--launch-uri",
                                                kLaunchUri};

  const CommandLineParseResult result =
      ParseCommandLine(arguments.size(), arguments.data());

  ASSERT_TRUE(result) << result.error;
  EXPECT_EQ(result.options.mode, CommandMode::kRun);
  EXPECT_EQ(result.options.raw_launch_argument, kLaunchUri);
  EXPECT_FALSE(result.options.launch_request_json.empty());
  EXPECT_EQ(result.options.launch_argument_index, 2);
}

TEST(CommandLineTest, AcceptsDesktopUriWithoutBreakingMenuLaunch) {
  constexpr char kUri[] =
      "roblox://experiences/start?placeId=17580461965&"
      "gameInstanceId=instance%2Bserver";
  const std::array<const char*, 2> arguments = {"mocktail", kUri};

  const CommandLineParseResult result =
      ParseCommandLine(arguments.size(), arguments.data());

  ASSERT_TRUE(result) << result.error;
  EXPECT_EQ(result.options.raw_launch_argument, kUri);
  EXPECT_FALSE(result.options.launch_request_json.empty());
  EXPECT_EQ(result.options.launch_argument_index, 1);

  std::vector<std::string> reexec_arguments;
  std::string reexec_error;
  ASSERT_TRUE(BuildCommandLineReexecArguments(result.options, arguments.size(),
                                              arguments.data(),
                                              &reexec_arguments, &reexec_error))
      << reexec_error;
  ASSERT_EQ(reexec_arguments.size(), 2u);
  EXPECT_EQ(reexec_arguments[0], "--launch-request-json");
  EXPECT_EQ(reexec_arguments[1], result.options.launch_request_json);

  const std::array<const char*, 1> menu_launch = {"mocktail"};
  const CommandLineParseResult menu_result =
      ParseCommandLine(menu_launch.size(), menu_launch.data());
  ASSERT_TRUE(menu_result) << menu_result.error;
  EXPECT_TRUE(menu_result.options.raw_launch_argument.empty());
  EXPECT_TRUE(menu_result.options.launch_request_json.empty());
}

TEST(CommandLineTest, RedactsBrowserTicketAcrossCgroupReexec) {
  constexpr char kSecret[] = "SECRET-BROWSER-GAMEINFO-TICKET";
  constexpr char kLaunchUri[] =
      "roblox-player:1+launchmode:play+"
      "gameinfo:SECRET-BROWSER-GAMEINFO-TICKET+"
      "placelauncherurl:https%3A%2F%2Fwww.roblox.com%2FGame%2F"
      "PlaceLauncher.ashx%3Frequest%3DRequestGame%26placeId%3D17580461965";
  const std::array<const char*, 3> arguments = {"mocktail", "--launch-uri",
                                                kLaunchUri};
  const CommandLineParseResult result =
      ParseCommandLine(arguments.size(), arguments.data());
  ASSERT_TRUE(result) << result.error;
  ASSERT_EQ(result.options.launch_request_json.find(kSecret),
            std::string::npos);

  std::vector<std::string> reexec_arguments;
  std::string error;
  ASSERT_TRUE(BuildCommandLineReexecArguments(result.options, arguments.size(),
                                              arguments.data(),
                                              &reexec_arguments, &error))
      << error;
  ASSERT_EQ(reexec_arguments.size(), 2u);
  EXPECT_EQ(reexec_arguments[0], "--launch-request-json");
  EXPECT_EQ(reexec_arguments[1], result.options.launch_request_json);
  EXPECT_EQ(reexec_arguments[1].find(kSecret), std::string::npos);

  const std::array<const char*, 3> reexec = {
      "mocktail", reexec_arguments[0].c_str(), reexec_arguments[1].c_str()};
  const CommandLineParseResult reparsed =
      ParseCommandLine(reexec.size(), reexec.data());
  ASSERT_TRUE(reparsed) << reparsed.error;
  EXPECT_FALSE(reparsed.options.raw_launch_argument.empty());
  EXPECT_EQ(reparsed.options.launch_argument_index, 2);
  EXPECT_EQ(reparsed.options.launch_request_json,
            result.options.launch_request_json);
}

TEST(CommandLineTest, DoesNotEchoInvalidInternalLaunchDocument) {
  constexpr char kSecret[] = "SECRET-INTERNAL-LAUNCH-DOCUMENT";
  const std::array<const char*, 3> arguments = {
      "mocktail", "--launch-request-json", kSecret};

  const CommandLineParseResult result =
      ParseCommandLine(arguments.size(), arguments.data());

  EXPECT_FALSE(result);
  EXPECT_EQ(result.error, "invalid controlled launch request");
  EXPECT_EQ(result.error.find(kSecret), std::string::npos);
}

TEST(CommandLineTest, ScrubsInternalLaunchDocumentFromOwnedAndProcessArgv) {
  char program[] = "mocktail";
  char option[] = "--launch-request-json";
  char document[] =
      R"({"placeId":17580461965,"accessCode":"private-access-code"})";
  char* mutable_arguments[] = {program, option, document};
  const char* parse_arguments[] = {program, option, document};
  CommandLineParseResult result = ParseCommandLine(3, parse_arguments);
  ASSERT_TRUE(result) << result.error;
  ASSERT_FALSE(result.options.raw_launch_argument.empty());
  ASSERT_FALSE(result.options.launch_request_json.empty());

  ScrubCommandLineLaunchArguments(&result.options, 3, mutable_arguments);

  EXPECT_TRUE(result.options.raw_launch_argument.empty());
  EXPECT_TRUE(result.options.launch_request_json.empty());
  EXPECT_EQ(result.options.launch_argument_index, -1);
  EXPECT_TRUE(std::all_of(std::begin(document), std::end(document) - 1,
                          [](char byte) { return byte == '\0'; }));
}

TEST(CommandLineTest, RejectsUnsupportedPositionalArgument) {
  const std::array<const char*, 2> arguments = {"mocktail", "/tmp/payload"};

  const CommandLineParseResult result =
      ParseCommandLine(arguments.size(), arguments.data());

  EXPECT_FALSE(result);
  EXPECT_EQ(result.error, "unknown argument");
}

TEST(CommandLineTest, RejectsUnknownOptionBeforeRuntimeWork) {
  const std::array<const char*, 2> arguments = {"mocktail", "--unknown"};

  const CommandLineParseResult result =
      ParseCommandLine(arguments.size(), arguments.data());

  EXPECT_FALSE(result);
  EXPECT_EQ(result.error, "unknown option: --unknown");
}

TEST(CommandLineTest, RejectsEveryMissingValue) {
  for (const char* option :
       {"--roblox-lib", "--graphics", "--launch-uri"}) {
    const std::array<const char*, 2> arguments = {"mocktail", option};
    const CommandLineParseResult result =
        ParseCommandLine(arguments.size(), arguments.data());
    EXPECT_FALSE(result) << option;
    EXPECT_EQ(result.error, std::string("missing value for ") + option)
        << option;
  }
}

TEST(CommandLineTest, RejectsDuplicateLaunchUri) {
  const std::array<const char*, 5> arguments = {
      "mocktail", "--launch-uri",
      "roblox://experiences/start?placeId=17580461965", "--launch-uri",
      "roblox://experiences/start?placeId=1962086868"};

  const CommandLineParseResult result =
      ParseCommandLine(arguments.size(), arguments.data());

  EXPECT_FALSE(result);
  EXPECT_NE(result.error.find("--launch-uri"), std::string::npos);
}

TEST(CommandLineTest, DoesNotEchoASecondRawLaunchTicket) {
  constexpr char kSecret[] = "SECRET-BROWSER-TICKET";
  const std::array<const char*, 3> arguments = {
      "mocktail", "roblox://experiences/start?placeId=17580461965",
      "roblox-player:1+launchmode:play+gameinfo:SECRET-BROWSER-TICKET"};

  const CommandLineParseResult result =
      ParseCommandLine(arguments.size(), arguments.data());

  EXPECT_FALSE(result);
  EXPECT_EQ(result.error, "unknown argument");
  EXPECT_EQ(result.error.find(kSecret), std::string::npos);
}

TEST(CommandLineTest, RejectsInvalidLaunchUriBeforeRuntimeWork) {
  const std::array<const char*, 3> arguments = {
      "mocktail", "--launch-uri",
      "https://www.roblox.com/experiences/start?placeId=17580461965"};

  const CommandLineParseResult result =
      ParseCommandLine(arguments.size(), arguments.data());

  EXPECT_FALSE(result);
  EXPECT_TRUE(result.options.raw_launch_argument.empty());
  EXPECT_NE(result.error.find("invalid Roblox launch URI"), std::string::npos);
}

TEST(CommandLineTest, RejectsRemovedStandaloneLoginOptions) {
  for (const char* option : {"--login", "--import-cookie"}) {
    const std::array<const char*, 2> arguments = {"mocktail", option};
    const CommandLineParseResult result =
        ParseCommandLine(arguments.size(), arguments.data());

    EXPECT_FALSE(result) << option;
    EXPECT_EQ(result.error, std::string("unknown option: ") + option);
  }
}

TEST(CommandLineTest, RejectsAnotherOptionAsMissingValue) {
  const std::array<const char*, 3> arguments = {"mocktail", "--roblox-lib",
                                                "--headless"};

  const CommandLineParseResult result =
      ParseCommandLine(arguments.size(), arguments.data());

  EXPECT_FALSE(result);
  EXPECT_EQ(result.error, "missing value for --roblox-lib");
}

TEST(CommandLineTest, HelpIsATypedAuxiliaryMode) {
  const std::array<const char*, 2> help = {"mocktail", "--help"};
  const CommandLineParseResult help_result =
      ParseCommandLine(help.size(), help.data());
  ASSERT_TRUE(help_result);
  EXPECT_EQ(help_result.options.mode, CommandMode::kHelp);
}

TEST(CommandLineTest, UsageContainsEverySupportedOption) {
  const std::string usage = CommandLineUsage("mocktail-test");
  EXPECT_NE(usage.find("mocktail-test"), std::string::npos);
  for (const char* option :
       {"--roblox-lib", "--headless", "--windowed", "--graphics",
        "--allow-unverified-build", "--launch-uri", "--help"}) {
    EXPECT_NE(usage.find(option), std::string::npos) << option;
  }
  EXPECT_EQ(usage.find("--login"), std::string::npos);
  EXPECT_EQ(usage.find("--import-cookie"), std::string::npos);
  EXPECT_NE(usage.find("native sign-in flow"), std::string::npos);
  EXPECT_NE(usage.find("verified managed x86_64 Roblox payload"),
            std::string::npos);
  EXPECT_EQ(usage.find("Normal startup uses rbx_bin/libroblox.so"),
            std::string::npos);
  EXPECT_EQ(usage.find("--launch-request-json"), std::string::npos);
}

}  // namespace
}  // namespace runtime
}  // namespace mocktail
