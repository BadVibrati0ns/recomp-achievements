#pragma once

// Private helpers shared by the library writer and reader.

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace recomp::achievements {
struct LibraryEvent;
}

namespace recomp::achievements::detail {

inline constexpr std::string_view kInstallSchema = "recomp-achievements-install/1";
inline constexpr std::string_view kProgressSchema = "recomp-achievements-library-progress/1";

std::string Sha256Hex(std::span<const uint8_t> data);
std::string Sha256Hex(std::string_view text);
uint32_t Crc32(std::span<const uint8_t> data, uint32_t crc = 0);

// Random UUID v4, lower-case with dashes.
std::string NewUuid();
uint64_t UnixMillis();

// Keeps [A-Za-z0-9._-] and replaces the rest, so ids from games are safe
// folder and file names everywhere. Never empty, "." or "..".
std::string SafeName(std::string_view name);

bool IsPng(std::span<const uint8_t> bytes);

// Writes to a unique temp file beside path, then renames over it, so readers
// see the old file or the new one, never half of one. False on any failure
// (the temp file is removed); never throws.
bool WriteFileAtomic(const std::filesystem::path& path, std::span<const uint8_t> bytes);
bool WriteJsonAtomic(const std::filesystem::path& path, const nlohmann::json& doc);
// Keys in the order written, for files people read.
bool WriteJsonAtomic(const std::filesystem::path& path, const nlohmann::ordered_json& doc);
// One events.log line (no newline), keys in the documented order.
std::string EventLine(const LibraryEvent& event);

// Null when missing or not a JSON object.
nlohmann::json ReadJsonFile(const std::filesystem::path& path);

}  // namespace recomp::achievements::detail
