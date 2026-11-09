#ifndef MOCKTAIL_UPDATE_STATE_FILE_H_
#define MOCKTAIL_UPDATE_STATE_FILE_H_

#include <filesystem>
#include <nlohmann/json.hpp>

namespace mocktail::update {

nlohmann::json ReadStateFile(const std::filesystem::path& path);
bool WriteStateFile(const std::filesystem::path& path,
                    const nlohmann::json& document);

}

#endif
