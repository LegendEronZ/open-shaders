#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// Pure string parsing, no engine/game dependency -- kept separate from
// WeatherManager.cpp (which pulls in RE:: types) so the unit-test target can
// compile and exercise this file directly.
namespace WeatherIDParser
{
	struct Result
	{
		std::vector<uint32_t> weatherIDs;
		// {token, error message} for entries that failed to parse.
		std::vector<std::pair<std::string, std::string>> invalidTokens;
	};

	/// @brief Parses a WeatherIDs= value into hex form IDs. ENB weatherlist.ini
	/// presets separate IDs with either commas or plain whitespace; both are accepted.
	Result Parse(const std::string& weatherIDsStr);

	/// @brief Parses a single hex form ID (e.g. a section name or a _locationweather.ini value).
	/// @return The parsed ID, or 0 for an empty string.
	/// @throws std::invalid_argument / std::out_of_range on a malformed hex string.
	uint32_t ParseHexID(const std::string& hexStr);
}
