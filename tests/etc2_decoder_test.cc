#include "mocktail/graphics/etc2_decoder.h"

#include <gtest/gtest.h>
#include <pthread.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

namespace {
std::atomic<unsigned> thread_creations{0};
}

extern "C" int __real_pthread_create(pthread_t*, const pthread_attr_t*,
                                     void* (*)(void*), void*);
extern "C" int __wrap_pthread_create(pthread_t* thread,
                                     const pthread_attr_t* attributes,
                                     void* (*entry)(void*), void* argument) {
  thread_creations.fetch_add(1, std::memory_order_relaxed);
  return __real_pthread_create(thread, attributes, entry, argument);
}

namespace mocktail::graphics {
namespace {

struct ImageFixture {
  explicit ImageFixture(EtcFormat format, std::uint32_t width = 257,
                        std::uint32_t height = 129, unsigned seed = 191)
      : source(((width + 3) / 4) * ((height + 3) / 4) * EtcBlockBytes(format)),
        expected(width * height * EtcDecodedTexelBytes(format)),
        output(expected.size() + 2, 0xa5) {
    std::uint32_t value = seed;
    for (auto& byte : source) {
      value ^= value << 13;
      value ^= value >> 17;
      value ^= value << 5;
      byte = static_cast<std::uint8_t>(value);
    }
    job = {format, source.data(),     source.size(),   width,
           height, output.data() + 1, expected.size(), false};
    EXPECT_TRUE(DecodeEtcImage(format, source.data(), source.size(), width,
                               height, expected.data(), expected.size()));
  }

  void Check() const {
    EXPECT_TRUE(job.ok);
    EXPECT_EQ(output.front(), 0xa5);
    EXPECT_EQ(output.back(), 0xa5);
    EXPECT_EQ(std::vector<std::uint8_t>(output.begin() + 1, output.end() - 1),
              expected);
  }

  std::vector<std::uint8_t> source;
  std::vector<std::uint8_t> expected;
  std::vector<std::uint8_t> output;
  EtcDecodeJob job;
};

TEST(EtcDecoderTest, ParallelBatchesMatchSerialForEveryFormat) {
  for (EtcFormat format :
       {EtcFormat::kEtc2Rgb8, EtcFormat::kEtc2Rgb8A1, EtcFormat::kEtc2Rgba8,
        EtcFormat::kEacR11, EtcFormat::kEacR11Signed, EtcFormat::kEacRg11,
        EtcFormat::kEacRg11Signed}) {
    for (unsigned workers : {0U, 1U, 2U, 8U}) {
      SCOPED_TRACE(::testing::Message()
                   << static_cast<int>(format) << " workers=" << workers);
      ImageFixture image(format);
      DecodeEtcJobs(&image.job, 1, workers);
      image.Check();
    }
  }
}

TEST(EtcDecoderTest, ReusesThreadsAcrossBatches) {
  ImageFixture image(EtcFormat::kEtc2Rgba8, 512, 512);
  DecodeEtcJobs(&image.job, 1, 8);
  const unsigned created = thread_creations.load();
  for (int repeat = 0; repeat < 12; ++repeat) {
    DecodeEtcJobs(&image.job, 1, 8);
    image.Check();
  }
  EXPECT_EQ(thread_creations.load(), created);
}

TEST(EtcDecoderTest, ConcurrentCallersKeepTheirResultsIndependent) {
  std::array<std::thread, 4> callers;
  for (unsigned index = 0; index < callers.size(); ++index) {
    callers[index] = std::thread([index] {
      ImageFixture image(EtcFormat::kEtc2Rgba8, 256, 256, index + 1);
      for (int repeat = 0; repeat < 12; ++repeat) {
        DecodeEtcJobs(&image.job, 1, 8);
        image.Check();
      }
    });
  }
  for (auto& caller : callers) caller.join();
}

TEST(EtcDecoderTest, InvalidJobsDoNotPreventValidJobsFromCompleting) {
  ImageFixture image(EtcFormat::kEtc2Rgba8);
  auto truncated = image.job;
  --truncated.source_bytes;
  auto short_output = image.job;
  --short_output.destination_bytes;
  std::array<EtcDecodeJob, 4> jobs{{{}, truncated, image.job, short_output}};
  DecodeEtcJobs(jobs.data(), jobs.size(), 8);
  EXPECT_FALSE(jobs[0].ok);
  EXPECT_FALSE(jobs[1].ok);
  EXPECT_FALSE(jobs[3].ok);
  image.job = jobs[2];
  image.Check();
  DecodeEtcJobs(nullptr, 1, 8);
  DecodeEtcJobs(jobs.data(), 0, 8);
}

TEST(EtcDecoderTest, SmallBatchesDoNotCreateThreads) {
  const unsigned created = thread_creations.load();
  ImageFixture image(EtcFormat::kEtc2Rgb8, 37, 33);
  DecodeEtcJobs(&image.job, 1, 8);
  image.Check();
  EXPECT_EQ(thread_creations.load(), created);
}

}
}
