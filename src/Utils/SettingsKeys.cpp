#include "Utils/SettingsPatch.h"

// Separate translation unit from SettingsPatch.cpp: this function is pure
// nlohmann::json recursion with no Feature dependency, so it can compile
// (and be unit-tested) without the engine headers ApplyPatch needs.
namespace Util::Settings
{
	void CollectUnknownSettingKeys(const json& a_incoming, const json& a_known,
		const std::string& a_prefix, std::vector<std::string>& a_out)
	{
		if (!a_incoming.is_object())
			return;
		for (auto it = a_incoming.begin(); it != a_incoming.end(); ++it) {
			const std::string path = a_prefix.empty() ? it.key() : a_prefix + "." + it.key();
			if (!a_known.is_object() || !a_known.contains(it.key()))
				a_out.push_back(path);
			else if (it.value().is_object())
				CollectUnknownSettingKeys(it.value(), a_known[it.key()], path, a_out);
		}
	}
}
