#include "mocktail/graphics/etc2_decoder.h"

#include <algorithm>
#include <array>
#include <pthread.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <limits>
#include <mutex>
#include <thread>
#include <vector>

namespace mocktail::graphics {
namespace {

constexpr std::array<int, 8> kDistance = {3, 6, 11, 16, 23, 32, 41, 64};

// Indexed by (msb << 1) | lsb.
constexpr int kEtc1Modifiers[8][4] = {
    {2, 8, -2, -8},     {5, 17, -5, -17},   {9, 29, -9, -29},
    {13, 42, -13, -42}, {18, 60, -18, -60}, {24, 80, -24, -80},
    {33, 106, -33, -106}, {47, 183, -47, -183},
};

constexpr int kEacModifiers[16][8] = {
    {-3, -6, -9, -15, 2, 5, 8, 14},  {-3, -7, -10, -13, 2, 6, 9, 12},
    {-2, -5, -8, -13, 1, 4, 7, 12},  {-2, -4, -6, -13, 1, 3, 5, 12},
    {-3, -6, -8, -12, 2, 5, 7, 11},  {-3, -7, -9, -11, 2, 6, 8, 10},
    {-4, -7, -8, -11, 3, 6, 7, 10},  {-3, -5, -8, -11, 2, 4, 7, 10},
    {-2, -6, -8, -10, 1, 5, 7, 9},   {-2, -5, -8, -10, 1, 4, 7, 9},
    {-2, -4, -8, -10, 1, 3, 7, 9},   {-2, -5, -7, -10, 1, 4, 6, 9},
    {-3, -4, -7, -10, 2, 3, 6, 9},   {-1, -2, -3, -10, 0, 1, 2, 9},
    {-4, -6, -8, -9, 3, 5, 7, 8},    {-3, -5, -7, -9, 2, 4, 6, 8},
};

std::uint8_t Clamp255(int value) {
  return static_cast<std::uint8_t>(std::clamp(value, 0, 255));
}

int Expand4(int value) { return (value << 4) | value; }
int Expand5(int value) { return (value << 3) | (value >> 2); }
int Expand6(int value) { return (value << 2) | (value >> 4); }
int Expand7(int value) { return (value << 1) | (value >> 6); }

// Texels use column-major indexing: x * 4 + y.
void DecodeRgbBlock(const std::uint8_t* b, bool punchthrough,
                    std::array<std::uint8_t, 64>* out) {
  static constexpr int kDelta[8] = {0, 1, 2, 3, -4, -3, -2, -1};
  const bool diff_or_opaque = (b[3] & 2) != 0;
  const bool opaque = !punchthrough || diff_or_opaque;
  const int msb = (b[4] << 8) | b[5];
  const int lsb = (b[6] << 8) | b[7];
  auto index_of = [msb, lsb](int texel) {
    return (((msb >> texel) & 1) << 1) | ((lsb >> texel) & 1);
  };
  auto write = [out](int texel, int r, int g, int bl, int a) {
    const std::size_t o = static_cast<std::size_t>(texel) * 4;
    (*out)[o] = Clamp255(r);
    (*out)[o + 1] = Clamp255(g);
    (*out)[o + 2] = Clamp255(bl);
    (*out)[o + 3] = static_cast<std::uint8_t>(a);
  };

  const int r_sum = (b[0] >> 3) + kDelta[b[0] & 7];
  const int g_sum = (b[1] >> 3) + kDelta[b[1] & 7];
  const int b_sum = (b[2] >> 3) + kDelta[b[2] & 7];
  const bool individual = !punchthrough && !diff_or_opaque;

  if (!individual && (r_sum < 0 || r_sum > 31 || g_sum < 0 || g_sum > 31)) {
    std::array<std::array<int, 3>, 4> paint{};
    if (r_sum < 0 || r_sum > 31) {
      const std::array<int, 3> c1 = {
          Expand4((((b[0] >> 3) & 3) << 2) | (b[0] & 3)),
          Expand4(b[1] >> 4), Expand4(b[1] & 15)};
      const std::array<int, 3> c2 = {Expand4(b[2] >> 4), Expand4(b[2] & 15),
                                     Expand4(b[3] >> 4)};
      const int d = kDistance[(((b[3] >> 2) & 3) << 1) | (b[3] & 1)];
      for (int c = 0; c < 3; ++c) {
        paint[0][c] = c1[c];
        paint[1][c] = c2[c] + d;
        paint[2][c] = c2[c];
        paint[3][c] = c2[c] - d;
      }
    } else {
      const std::array<int, 3> c1 = {
          Expand4((b[0] >> 3) & 15),
          Expand4(((b[0] & 7) << 1) | ((b[1] >> 4) & 1)),
          Expand4((b[1] & 8) | ((b[1] & 3) << 1) | (b[2] >> 7))};
      const std::array<int, 3> c2 = {
          Expand4((b[2] >> 3) & 15), Expand4(((b[2] & 7) << 1) | (b[3] >> 7)),
          Expand4((b[3] >> 3) & 15)};
      const int v1 = (c1[0] << 16) | (c1[1] << 8) | c1[2];
      const int v2 = (c2[0] << 16) | (c2[1] << 8) | c2[2];
      const int d =
          kDistance[(b[3] & 4) | ((b[3] & 1) << 1) | (v1 >= v2 ? 1 : 0)];
      for (int c = 0; c < 3; ++c) {
        paint[0][c] = c1[c] + d;
        paint[1][c] = c1[c] - d;
        paint[2][c] = c2[c] + d;
        paint[3][c] = c2[c] - d;
      }
    }
    for (int texel = 0; texel < 16; ++texel) {
      const int index = index_of(texel);
      if (!opaque && index == 2) {
        write(texel, 0, 0, 0, 0);
      } else {
        write(texel, paint[index][0], paint[index][1], paint[index][2], 255);
      }
    }
    return;
  }

  if (!individual && (b_sum < 0 || b_sum > 31)) {
    const int ro = Expand6((b[0] >> 1) & 63);
    const int go = Expand7(((b[0] & 1) << 6) | ((b[1] >> 1) & 63));
    const int bo = Expand6(((b[1] & 1) << 5) | (b[2] & 0x18) |
                           ((b[2] & 3) << 1) | (b[3] >> 7));
    const int rh = Expand6(((b[3] & 0x7c) >> 1) | (b[3] & 1));
    const int gh = Expand7(b[4] >> 1);
    const int bh = Expand6(((b[4] & 1) << 5) | (b[5] >> 3));
    const int rv = Expand6(((b[5] & 7) << 3) | (b[6] >> 5));
    const int gv = Expand7(((b[6] & 31) << 2) | (b[7] >> 6));
    const int bv = Expand6(b[7] & 63);
    for (int x = 0; x < 4; ++x) {
      for (int y = 0; y < 4; ++y) {
        write(x * 4 + y, (x * (rh - ro) + y * (rv - ro) + 4 * ro + 2) >> 2,
              (x * (gh - go) + y * (gv - go) + 4 * go + 2) >> 2,
              (x * (bh - bo) + y * (bv - bo) + 4 * bo + 2) >> 2, 255);
      }
    }
    return;
  }

  std::array<std::array<int, 3>, 2> base{};
  for (int c = 0; c < 3; ++c) {
    if (individual) {
      base[0][c] = Expand4(b[c] >> 4);
      base[1][c] = Expand4(b[c] & 15);
    } else {
      base[0][c] = Expand5(b[c] >> 3);
      base[1][c] = Expand5((b[c] >> 3) + kDelta[b[c] & 7]);
    }
  }
  const int tables[2] = {b[3] >> 5, (b[3] >> 2) & 7};
  const bool flipped = (b[3] & 1) != 0;
  for (int x = 0; x < 4; ++x) {
    for (int y = 0; y < 4; ++y) {
      const int texel = x * 4 + y;
      const int index = index_of(texel);
      if (!opaque && index == 2) {
        write(texel, 0, 0, 0, 0);
        continue;
      }
      const int sub = flipped ? (y >= 2 ? 1 : 0) : (x >= 2 ? 1 : 0);
      const int modifier =
          (!opaque && index == 0) ? 0 : kEtc1Modifiers[tables[sub]][index];
      write(texel, base[sub][0] + modifier, base[sub][1] + modifier,
            base[sub][2] + modifier, 255);
    }
  }
}

int EacIndex(const std::uint8_t* b, int texel) {
  std::uint64_t bits = 0;
  for (int i = 2; i < 8; ++i) {
    bits = (bits << 8) | b[i];
  }
  return static_cast<int>((bits >> (45 - texel * 3)) & 7);
}

int EacModifier(const std::uint8_t* b, int texel) {
  return kEacModifiers[b[1] & 15][EacIndex(b, texel)];
}

std::uint16_t DecodeR11(const std::uint8_t* b, int texel, bool is_signed) {
  const int multiplier = b[1] >> 4;
  const int modifier = EacModifier(b, texel);
  const int scaled = multiplier != 0 ? modifier * multiplier * 8 : modifier;
  if (!is_signed) {
    const int value = std::clamp(b[0] * 8 + 4 + scaled, 0, 2047);
    return static_cast<std::uint16_t>((value << 5) | (value >> 6));
  }
  const int base = std::max(-127, static_cast<int>(static_cast<std::int8_t>(b[0])));
  const int value = std::clamp(base * 8 + scaled, -1023, 1023);
  const int magnitude = value < 0 ? -value : value;
  const int expanded = (magnitude << 5) | (magnitude >> 5);
  return static_cast<std::uint16_t>(
      static_cast<std::int16_t>(value < 0 ? -expanded : expanded));
}

}  // namespace

std::size_t EtcBlockBytes(EtcFormat format) {
  switch (format) {
    case EtcFormat::kEtc2Rgba8:
    case EtcFormat::kEacRg11:
    case EtcFormat::kEacRg11Signed:
      return 16;
    default:
      return 8;
  }
}

std::size_t EtcDecodedTexelBytes(EtcFormat format) {
  switch (format) {
    case EtcFormat::kEacR11:
    case EtcFormat::kEacR11Signed:
      return 2;
    default:
      return 4;
  }
}

namespace {

constexpr std::uint64_t kBlocksPerBand = 1024;
constexpr std::uint64_t kParallelMinimumBlocks = 4096;

bool ImageFits(EtcFormat format, const std::uint8_t* source,
               std::size_t source_bytes, std::uint32_t width,
               std::uint32_t height, const std::uint8_t* destination,
               std::size_t destination_bytes) {
  if (source == nullptr || destination == nullptr || width == 0 ||
      height == 0) {
    return false;
  }
  const std::uint64_t blocks_wide = (static_cast<std::uint64_t>(width) + 3) / 4;
  const std::uint64_t blocks_high =
      (static_cast<std::uint64_t>(height) + 3) / 4;
  return blocks_wide * blocks_high * EtcBlockBytes(format) <= source_bytes &&
         static_cast<std::uint64_t>(width) * height *
                 EtcDecodedTexelBytes(format) <=
             destination_bytes;
}

}  // namespace

bool DecodeEtcImage(EtcFormat format, const std::uint8_t* source,
                    std::size_t source_bytes, std::uint32_t width,
                    std::uint32_t height, std::uint8_t* destination,
                    std::size_t destination_bytes) {
  return DecodeEtcImageBlockRows(format, source, source_bytes, width, height, 0,
                                 (height + 3) / 4, destination,
                                 destination_bytes);
}

namespace {

struct DecodeBand {
  EtcDecodeJob* job;
  std::uint32_t first_block_row;
  std::uint32_t block_row_count;
};

struct DecodeBandQueue {
  std::vector<DecodeBand> bands;
  std::atomic<std::size_t> next{0};
  std::mutex mutex;
  std::condition_variable completed;
  unsigned workers = 0;

  void Run() {
    for (;;) {
      const std::size_t index = next.fetch_add(1, std::memory_order_relaxed);
      if (index >= bands.size()) {
        return;
      }
      const DecodeBand& band = bands[index];
      const EtcDecodeJob& job = *band.job;
      DecodeEtcImageBlockRows(job.format, job.source, job.source_bytes,
                              job.width, job.height, band.first_block_row,
                              band.block_row_count, job.destination,
                              job.destination_bytes);
    }
  }
};

class DecodeWorkerPool {
 public:
  DecodeWorkerPool() {
    const unsigned hardware = std::max(1U, std::thread::hardware_concurrency());
    const unsigned limit = std::min(7U, hardware - 1);
    while (worker_count_ < limit) {
      if (pthread_create(&workers_[worker_count_], nullptr, Entry, this) != 0) {
        break;
      }
      ++worker_count_;
    }
  }

  ~DecodeWorkerPool() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stopping_ = true;
    }
    ready_.notify_all();
    for (unsigned index = 0; index < worker_count_; ++index) {
      pthread_join(workers_[index], nullptr);
    }
  }

  void Run(DecodeBandQueue* queue, unsigned workers) {
    const unsigned helpers = std::min(workers, worker_count_);
    if (helpers == 0) {
      queue->Run();
      return;
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      queue->workers = helpers;
      for (unsigned index = 0; index < helpers; ++index) {
        pending_.push_back(queue);
      }
    }
    ready_.notify_all();
    queue->Run();
    std::unique_lock<std::mutex> lock(queue->mutex);
    queue->completed.wait(lock, [queue] { return queue->workers == 0; });
  }

 private:
  static void* Entry(void* context) {
    static_cast<DecodeWorkerPool*>(context)->RunWorker();
    return nullptr;
  }

  void RunWorker() {
    for (;;) {
      DecodeBandQueue* queue;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        ready_.wait(lock, [this] { return stopping_ || !pending_.empty(); });
        if (pending_.empty()) {
          return;
        }
        queue = pending_.front();
        pending_.pop_front();
      }
      queue->Run();
      std::lock_guard<std::mutex> lock(queue->mutex);
      if (--queue->workers == 0) {
        queue->completed.notify_one();
      }
    }
  }

  std::mutex mutex_;
  std::condition_variable ready_;
  std::deque<DecodeBandQueue*> pending_;
  std::array<pthread_t, 7> workers_{};
  unsigned worker_count_ = 0;
  bool stopping_ = false;
};

}  // namespace

void DecodeEtcJobs(EtcDecodeJob* jobs, std::size_t count,
                   unsigned worker_count) {
  if (jobs == nullptr) {
    return;
  }
  DecodeBandQueue queue;
  std::vector<DecodeBand>& bands = queue.bands;
  std::uint64_t total_blocks = 0;
  for (std::size_t index = 0; index < count; ++index) {
    EtcDecodeJob& job = jobs[index];
    job.ok = ImageFits(job.format, job.source, job.source_bytes, job.width,
                       job.height, job.destination, job.destination_bytes);
    if (!job.ok) {
      continue;
    }
    const std::uint32_t blocks_wide =
        (static_cast<std::uint64_t>(job.width) + 3) / 4;
    const std::uint32_t blocks_high =
        (static_cast<std::uint64_t>(job.height) + 3) / 4;
    const std::uint32_t rows_per_band = static_cast<std::uint32_t>(
        std::max<std::uint64_t>(1, kBlocksPerBand / blocks_wide));
    for (std::uint32_t row = 0; row < blocks_high; row += rows_per_band) {
      bands.push_back(
          {&job, row, std::min(rows_per_band, blocks_high - row)});
    }
    total_blocks += static_cast<std::uint64_t>(blocks_wide) * blocks_high;
  }

  const std::size_t threads =
      total_blocks < kParallelMinimumBlocks
          ? 1
          : std::min<std::size_t>(std::max(worker_count, 1U), bands.size());
  if (threads <= 1) {
    queue.Run();
  } else {
    static DecodeWorkerPool pool;
    pool.Run(&queue, static_cast<unsigned>(threads - 1));
  }
}

bool DecodeEtcImageBlockRows(EtcFormat format, const std::uint8_t* source,
                             std::size_t source_bytes, std::uint32_t width,
                             std::uint32_t height,
                             std::uint32_t first_block_row,
                             std::uint32_t block_row_count,
                             std::uint8_t* destination,
                             std::size_t destination_bytes) {
  if (!ImageFits(format, source, source_bytes, width, height, destination,
                 destination_bytes)) {
    return false;
  }
  const std::uint64_t blocks_wide = (static_cast<std::uint64_t>(width) + 3) / 4;
  const std::uint64_t blocks_high =
      (static_cast<std::uint64_t>(height) + 3) / 4;
  if (static_cast<std::uint64_t>(first_block_row) + block_row_count >
      blocks_high) {
    return false;
  }
  const std::size_t texel_bytes = EtcDecodedTexelBytes(format);
  const std::size_t block_bytes = EtcBlockBytes(format);

  std::array<std::uint8_t, 64> rgba{};
  const std::uint64_t end_block_row =
      static_cast<std::uint64_t>(first_block_row) + block_row_count;
  for (std::uint64_t by = first_block_row; by < end_block_row; ++by) {
    for (std::uint64_t bx = 0; bx < blocks_wide; ++bx) {
      const std::uint8_t* block =
          source + (by * blocks_wide + bx) * block_bytes;
      if (format == EtcFormat::kEtc2Rgb8 || format == EtcFormat::kEtc2Rgb8A1) {
        DecodeRgbBlock(block, format == EtcFormat::kEtc2Rgb8A1, &rgba);
      } else if (format == EtcFormat::kEtc2Rgba8) {
        DecodeRgbBlock(block + 8, false, &rgba);
      }
      for (std::uint32_t y = 0; y < 4 && by * 4 + y < height; ++y) {
        for (std::uint32_t x = 0; x < 4 && bx * 4 + x < width; ++x) {
          const int texel = static_cast<int>(x * 4 + y);
          std::uint8_t* out =
              destination + ((by * 4 + y) * width + bx * 4 + x) * texel_bytes;
          switch (format) {
            case EtcFormat::kEtc2Rgb8:
            case EtcFormat::kEtc2Rgb8A1:
            case EtcFormat::kEtc2Rgba8:
              std::copy_n(rgba.data() + texel * 4, 4, out);
              if (format == EtcFormat::kEtc2Rgba8) {
                const int alpha =
                    block[0] + EacModifier(block, texel) * (block[1] >> 4);
                out[3] = Clamp255(alpha);
              }
              break;
            case EtcFormat::kEacR11:
            case EtcFormat::kEacR11Signed:
            case EtcFormat::kEacRg11:
            case EtcFormat::kEacRg11Signed: {
              const bool is_signed = format == EtcFormat::kEacR11Signed ||
                                     format == EtcFormat::kEacRg11Signed;
              const int channels = texel_bytes / 2;
              for (int channel = 0; channel < channels; ++channel) {
                const std::uint16_t value =
                    DecodeR11(block + channel * 8, texel, is_signed);
                out[channel * 2] = static_cast<std::uint8_t>(value);
                out[channel * 2 + 1] = static_cast<std::uint8_t>(value >> 8);
              }
              break;
            }
          }
        }
      }
    }
  }
  return true;
}

}  // namespace mocktail::graphics
