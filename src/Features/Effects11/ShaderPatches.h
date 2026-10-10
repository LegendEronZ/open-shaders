#pragma once

#include <string>
#include <vector>

namespace Util
{
	namespace ShaderPatches
	{
		struct Replacement
		{
			std::string find;
			std::string replace;
		};

		struct Entry
		{
			std::string file;
			std::vector<Replacement> replacements;
		};

		/** @brief Reloads preset source replacements from ShaderPatches.json. */
		void Load();
		/** @brief Applies replacements for the include filename and reports whether its bytes changed. */
		bool Apply(const char* filename, std::vector<char>& buffer);
		/** @brief Applies replacements for the include filename and reports whether its source changed. */
		bool Apply(const char* filename, std::string& content);
	}
}
