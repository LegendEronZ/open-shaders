#include "WeatherIDParser.h"

#include <algorithm>
#include <sstream>

namespace WeatherIDParser
{
	uint32_t ParseHexID(const std::string& hexStr)
	{
		if (hexStr.empty()) {
			return 0;
		}

		return static_cast<uint32_t>(std::stoul(hexStr, nullptr, 16));
	}

	Result Parse(const std::string& weatherIDsStr)
	{
		Result result;

		// ENB weatherlist.ini uses commas or whitespace to separate IDs; normalize to whitespace.
		std::string normalized = weatherIDsStr;
		std::replace(normalized.begin(), normalized.end(), ',', ' ');

		std::stringstream ss(normalized);
		std::string token;

		while (ss >> token) {
			try {
				uint32_t weatherID = ParseHexID(token);
				if (weatherID != 0) {
					result.weatherIDs.push_back(weatherID);
				}
			} catch (const std::exception& e) {
				result.invalidTokens.emplace_back(token, e.what());
			}
		}

		return result;
	}
}
