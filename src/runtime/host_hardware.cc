#include "runtime/host_hardware.h"

#include <sched.h>
#include <sys/utsname.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <system_error>
#include <utility>

namespace mocktail {
namespace runtime {
namespace {

std::string Trim(std::string_view value) {
  const auto is_space = [](unsigned char character) {
    return character == ' ' || character == '\t' || character == '\n' ||
           character == '\r';
  };
  while (!value.empty() && is_space(value.front())) {
    value.remove_prefix(1);
  }
  while (!value.empty() && is_space(value.back())) {
    value.remove_suffix(1);
  }
  return std::string(value);
}

std::string Lower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](char c) {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  });
  return value;
}

bool ContainsToken(std::string_view text, std::string_view needle) {
  std::size_t position = 0;
  while (position < text.size()) {
    const std::size_t end = text.find(' ', position);
    const std::string_view word =
        text.substr(position, end == std::string_view::npos
                                   ? std::string_view::npos
                                   : end - position);
    if (word == needle) {
      return true;
    }
    if (end == std::string_view::npos) {
      break;
    }
    position = end + 1;
  }
  return false;
}

bool ContainsWord(std::string_view text, std::string_view word) {
  // Word-boundary substring search ("Xe" must not match "Xeon").
  std::size_t position = 0;
  while ((position = text.find(word, position)) != std::string_view::npos) {
    const bool left_ok = position == 0 ||
        (!std::isalnum(static_cast<unsigned char>(text[position - 1])) &&
         text[position - 1] != '_');
    const std::size_t right = position + word.size();
    const bool right_ok = right >= text.size() ||
        (!std::isalnum(static_cast<unsigned char>(text[right])) &&
         text[right] != '_');
    if (left_ok && right_ok) {
      return true;
    }
    position = right;
  }
  return false;
}

std::string ReadFirstLine(const std::filesystem::path& path) {
  std::ifstream input(path);
  std::string line;
  if (!std::getline(input, line)) {
    return {};
  }
  return Trim(line);
}

bool ReadIntFile(const std::filesystem::path& path, long* value) {
  std::ifstream input(path);
  return static_cast<bool>(input >> *value);
}

std::string ReadCpuInfoModel() {
  std::ifstream input("/proc/cpuinfo");
  std::string line;
  while (std::getline(input, line)) {
    const std::size_t separator = line.find(':');
    if (separator == std::string_view::npos) {
      continue;
    }
    if (Trim(line.substr(0, separator)) == "model name") {
      return Trim(line.substr(separator + 1));
    }
  }
  return {};
}

}  // namespace

std::vector<int> ParseCpuList(std::string_view text) {
  std::vector<int> result;
  std::size_t position = 0;
  while (position <= text.size()) {
    const std::size_t end = text.find(',', position);
    const std::string_view token =
        text.substr(position, end == std::string_view::npos
                                   ? std::string_view::npos
                                   : end - position);
    const std::string trimmed = Trim(token);
    if (!trimmed.empty()) {
      const std::size_t dash = trimmed.find('-');
      long first = -1;
      long last = -1;
      if (dash == std::string::npos) {
        const auto parsed = std::from_chars(trimmed.data(),
                                             trimmed.data() + trimmed.size(),
                                             first);
        if (parsed.ec == std::errc() && parsed.ptr == trimmed.data() + trimmed.size()) {
          last = first;
        }
      } else {
        const std::string_view left = std::string_view(trimmed).substr(0, dash);
        const std::string_view right = std::string_view(trimmed).substr(dash + 1);
        const auto parse_left = std::from_chars(left.data(), left.data() + left.size(), first);
        const auto parse_right = std::from_chars(right.data(), right.data() + right.size(), last);
        if (parse_left.ec != std::errc() || parse_left.ptr != left.data() + left.size() ||
            parse_right.ec != std::errc() || parse_right.ptr != right.data() + right.size()) {
          first = last = -1;
        }
      }
      if (first >= 0 && last >= first) {
        for (long cpu = first; cpu <= last && cpu <= CPU_SETSIZE; ++cpu) {
          result.push_back(static_cast<int>(cpu));
        }
      }
    }
    if (end == std::string_view::npos) {
      break;
    }
    position = end + 1;
  }
  return result;
}

CpuTopology ClassifyCpuTopology(
    std::string_view model, const std::vector<CpuInfoEntry>& entries,
    const std::vector<int>& kernel_performance_cpus,
    const std::vector<int>& kernel_efficiency_cpus) {
  CpuTopology topology;
  topology.model = std::string(model);
  topology.intel = ContainsToken(topology.model, "Intel") ||
                   topology.model.find("Intel") != std::string::npos;
  topology.logical_cores = static_cast<int>(entries.size());

  std::set<std::pair<int, int>> physical;
  long max_khz = 0;
  std::set<int> allowed_ids;
  for (const CpuInfoEntry& entry : entries) {
    physical.emplace(entry.package_id, entry.core_id);
    allowed_ids.insert(entry.logical_id);
    max_khz = std::max(max_khz, entry.max_freq_khz);
  }
  topology.physical_cores = static_cast<int>(physical.size());
  topology.max_freq_mhz = max_khz / 1000;

  // Preferred: the kernel's own P/E classification on Intel hybrid CPUs.
  if (!kernel_performance_cpus.empty() && !kernel_efficiency_cpus.empty()) {
    std::set<int> performance_ids;
    for (const int cpu : kernel_performance_cpus) {
      if (allowed_ids.count(cpu) != 0) {
        performance_ids.insert(cpu);
      }
    }
    if (!performance_ids.empty() && performance_ids.size() < entries.size()) {
      std::set<std::pair<int, int>> performance_physical;
      for (const CpuInfoEntry& entry : entries) {
        if (performance_ids.count(entry.logical_id) != 0) {
          performance_physical.emplace(entry.package_id, entry.core_id);
        }
      }
      topology.hybrid = true;
      topology.performance_threads = static_cast<int>(performance_ids.size());
      topology.performance_cores =
          static_cast<int>(performance_physical.size());
      topology.efficiency_cores =
          topology.physical_cores - topology.performance_cores;
      return topology;
    }
  }

  // Fallback for Intel: cluster per-CPU max frequencies. P-cores clock
  // strictly higher than E-cores (4.40 GHz vs 3.30 GHz on the i5-1235U).
  if (topology.intel && entries.size() >= 2) {
    std::map<long, std::vector<const CpuInfoEntry*>> clusters;
    for (const CpuInfoEntry& entry : entries) {
      clusters[entry.max_freq_khz].push_back(&entry);
    }
    if (clusters.size() >= 2) {
      auto top = clusters.rbegin();
      auto second = std::next(top);
      if (top->first > second->first && top->first > 0) {
        std::set<std::pair<int, int>> performance_physical;
        for (const CpuInfoEntry* entry : top->second) {
          performance_physical.emplace(entry->package_id, entry->core_id);
        }
        topology.hybrid = true;
        topology.performance_threads =
            static_cast<int>(top->second.size());
        topology.performance_cores =
            static_cast<int>(performance_physical.size());
        topology.efficiency_cores =
            topology.physical_cores - topology.performance_cores;
      }
    }
  }
  return topology;
}

CpuTopology DetectCpuTopology() {
  cpu_set_t allowed;
  CPU_ZERO(&allowed);
  const bool have_affinity = sched_getaffinity(0, sizeof(allowed), &allowed) == 0;

  std::vector<CpuInfoEntry> entries;
  for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
    if (have_affinity && !CPU_ISSET(cpu, &allowed)) {
      continue;
    }
    CpuInfoEntry entry;
    entry.logical_id = cpu;
    const std::string base =
        "/sys/devices/system/cpu/cpu" + std::to_string(cpu);
    long package_id = 0;
    long core_id = cpu;  // Fallback: treat each logical CPU as its own core.
    if (ReadIntFile(base + "/topology/physical_package_id", &package_id)) {
      entry.package_id = static_cast<int>(package_id);
    }
    if (ReadIntFile(base + "/topology/core_id", &core_id)) {
      entry.core_id = static_cast<int>(core_id);
    }
    long max_freq = 0;
    if (ReadIntFile(base + "/cpufreq/cpuinfo_max_freq", &max_freq)) {
      entry.max_freq_khz = max_freq;
    }
    entries.push_back(entry);
  }
  if (entries.empty()) {
    CpuInfoEntry fallback;
    fallback.logical_id = 0;
    fallback.core_id = 0;
    entries.push_back(fallback);
  }

  const std::vector<int> kernel_p =
      ParseCpuList(ReadFirstLine("/sys/devices/cpu_core/cpus"));
  const std::vector<int> kernel_e =
      ParseCpuList(ReadFirstLine("/sys/devices/cpu_atom/cpus"));
  return ClassifyCpuTopology(ReadCpuInfoModel(), entries, kernel_p, kernel_e);
}

GpuInfo ClassifyGpu(std::string_view vendor_id, std::string_view device_id,
                    std::string_view driver, std::string_view name) {
  GpuInfo info;
  const auto normalize_id = [](std::string_view value) {
    std::string result = Lower(Trim(value));
    if (result.size() > 2 && result[0] == '0' && result[1] == 'x') {
      result.erase(0, 2);
    }
    return result;
  };
  info.vendor_id = normalize_id(vendor_id);
  info.device_id = normalize_id(device_id);
  info.driver = Lower(Trim(driver));
  info.name = Trim(name);
  info.present = !info.vendor_id.empty() || !info.driver.empty() ||
                 !info.name.empty();

  const bool intel = info.vendor_id == "8086";
  const bool arc = ContainsWord(info.name, "Arc");
  info.intel_igpu = intel && !arc &&
                    (info.driver == "i915" || info.driver == "xe");
  // Virtual and SoC GPUs have no dedicated VRAM either.
  static constexpr std::array<std::string_view, 9> kSharedMemoryDrivers = {
      "virtio_gpu", "vmwgfx", "llvmpipe", "swrast", "panfrost",
      "v3d",        "vc4",    "lima",     "msm",
  };
  info.integrated =
      info.intel_igpu ||
      std::any_of(kSharedMemoryDrivers.begin(), kSharedMemoryDrivers.end(),
                  [&](std::string_view candidate) {
                    return info.driver == candidate;
                  });
  return info;
}

GpuInfo DetectPrimaryGpu() {
  std::error_code error;
  std::vector<std::filesystem::path> render_nodes;
  for (std::filesystem::directory_iterator iterator("/sys/class/drm", error),
       end;
       !error && iterator != end; iterator.increment(error)) {
    const std::string name = iterator->path().filename().string();
    if (name.size() > 7 && name.substr(0, 7) == "renderD" &&
        std::all_of(name.begin() + 7, name.end(), [](char c) {
          return std::isdigit(static_cast<unsigned char>(c)) != 0;
        })) {
      render_nodes.push_back(iterator->path());
    }
  }
  std::sort(render_nodes.begin(), render_nodes.end());
  for (const std::filesystem::path& node : render_nodes) {
    error.clear();
    const std::filesystem::path device =
        std::filesystem::canonical(node / "device", error);
    if (error) {
      continue;
    }
    std::string driver;
    error.clear();
    driver = std::filesystem::read_symlink(device / "driver", error)
                 .filename()
                 .string();
    if (driver.empty()) {
      driver = ReadFirstLine(device / "uevent");
      const std::size_t prefix = driver.find("DRIVER=");
      if (prefix != std::string::npos) {
        driver = Trim(driver.substr(prefix + 7));
      } else {
        driver.clear();
      }
    }
    std::string vendor = ReadFirstLine(device / "vendor");
    if (vendor.size() > 2 && vendor[0] == '0' &&
        (vendor[1] == 'x' || vendor[1] == 'X')) {
      vendor = vendor.substr(2);
    }
    const std::string device_id = ReadFirstLine(device / "device");
    std::string name = ReadFirstLine(device / "product_name");
    if (name.empty()) {
      name = driver.empty() ? "unknown" : driver;
    }
    return ClassifyGpu(vendor, device_id, driver, name);
  }
  return {};
}

}  // namespace runtime
}  // namespace mocktail
