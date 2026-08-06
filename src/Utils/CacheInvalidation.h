#pragma once

// Disk shader-cache invalidation logic, kept free of game/SKSE dependencies so
// tests/cpp can exercise it directly (see test_cacheinvalidation.cpp). Policy:
// every unknown or failed path degrades to "invalidate more", never less --
// serving a blob compiled under a different feature set is silent corruption
// (feature defines change every shader's bytecode; cache paths don't encode them).

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <string>
#include <vector>

namespace Util::CacheInvalidation
{
	/// One disk-cache/runtime state divergence.
	struct CacheMismatch
	{
		enum class Kind
		{
			PluginVersion,   ///< plugin updated (or no Info.ini) — expected, rebuild silently
			FeatureVersion,  ///< feature updated — expected, rebuild (partially when possible)
			EnabledFlip,     ///< feature set changed — likely unintentional, hold + prompt
		};
		Kind kind;
		std::string shortName;    ///< stable key ("Plugin" for plugin-version entries)
		std::string feature;      ///< display name for UI
		std::string detail;       ///< English direction of the mismatch (logs + fallback)
		bool nowPresent = false;  ///< EnabledFlip only: feature is loaded now (added) vs not (removed); lets the UI localize the direction
	};

	/// Runtime-side view of a feature, decoupled from the Feature class.
	struct FeatureState
	{
		std::string shortName;
		std::string name;
		bool loaded = false;
		std::string version;
		std::string define;  ///< global shader define; empty => unknown reach (full wipe)
	};

	/// Parsed Info.ini feature section.
	struct CacheIniEntry
	{
		bool enabled = false;
		std::optional<std::string> version;
	};

	/// Compare the cache manifest against current state. Pure function of its inputs.
	inline std::vector<CacheMismatch> ClassifyMismatches(
		const std::string& currentPluginVersion,
		const std::optional<std::string>& cachedPluginVersion,
		const std::vector<FeatureState>& features,
		const std::map<std::string, CacheIniEntry>& cacheEntries)
	{
		std::vector<CacheMismatch> mismatches;
		if (!cachedPluginVersion) {
			mismatches.push_back({ CacheMismatch::Kind::PluginVersion, "Plugin", "Plugin", "no plugin version found in cache" });
		} else if (*cachedPluginVersion != currentPluginVersion) {
			mismatches.push_back({ CacheMismatch::Kind::PluginVersion, "Plugin", "Plugin",
				std::format("version changed (current: {}, cached: {})", currentPluginVersion, *cachedPluginVersion) });
		}
		for (const auto& feature : features) {
			const auto it = cacheEntries.find(feature.shortName);
			const bool enabledInCache = it != cacheEntries.end() && it->second.enabled;
			if (enabledInCache != feature.loaded) {
				mismatches.push_back({ CacheMismatch::Kind::EnabledFlip, feature.shortName, feature.name,
					feature.loaded ?
						"installed/enabled now, but the cache was built without it" :
						"the cache was built with it, but it is now uninstalled or disabled at boot",
					feature.loaded });
				continue;
			}
			if (feature.loaded) {
				const auto& cachedVersion = it->second.version;
				if (!cachedVersion || *cachedVersion != feature.version) {
					mismatches.push_back({ CacheMismatch::Kind::FeatureVersion, feature.shortName, feature.name,
						std::format("version changed (installed: {}, cached: {})", feature.version,
							cachedVersion ? *cachedVersion : "<none>") });
				}
			}
		}
		return mismatches;
	}

	/// True when every mismatch is a Disable-at-Boot toggle change (as opposed to
	/// a version bump), the case eligible for rollback rotation rather than a wipe.
	inline bool OnlyEnabledFlips(const std::vector<CacheMismatch>& mismatches)
	{
		return std::ranges::all_of(mismatches,
			[](const CacheMismatch& mismatch) { return mismatch.kind == CacheMismatch::Kind::EnabledFlip; });
	}

	/// Predicate for "is this feature deliberately disabled at boot", injected so
	/// this stays free of the `globals::state` singleton the real caller reads.
	using IsFeatureDeliberatelyDisabledFn = std::function<bool(const std::string& shortName)>;

	/// A cached feature that is gone but NOT deliberately disabled at boot is a
	/// broken install: hold rather than rotate, so a fixed install reuses the cache.
	inline bool HasMissingFeature(const std::vector<CacheMismatch>& mismatches,
		const IsFeatureDeliberatelyDisabledFn& isDeliberatelyDisabled)
	{
		return std::ranges::any_of(mismatches,
			[&](const CacheMismatch& mismatch) {
				if (mismatch.kind != CacheMismatch::Kind::EnabledFlip || mismatch.nowPresent)
					return false;
				return !isDeliberatelyDisabled(mismatch.shortName);
			});
	}

	/// True when a set of current-vs-rollback-slot mismatches is eligible for
	/// restore: non-empty, pure toggle flips, and no broken-install case among them.
	inline bool AreCacheMismatchesRestorable(const std::vector<CacheMismatch>& mismatches,
		const IsFeatureDeliberatelyDisabledFn& isDeliberatelyDisabled)
	{
		return !mismatches.empty() && OnlyEnabledFlips(mismatches) && !HasMissingFeature(mismatches, isDeliberatelyDisabled);
	}

	/// Decide whether `mismatches` (current state vs. the rollback slot's
	/// manifest) qualify as a restore candidate, and if so record them. Whether
	/// the rollback slot's Info.ini actually exists on disk is the caller's
	/// concern (a filesystem check), passed in as `previousCacheOnDisk` so this
	/// stays pure and testable without a real ShaderCache.Previous directory.
	inline bool TrySetRestoreCandidate(std::vector<CacheMismatch> mismatches, bool previousCacheOnDisk,
		const IsFeatureDeliberatelyDisabledFn& isDeliberatelyDisabled,
		bool& outAvailable, std::vector<CacheMismatch>& outMismatches)
	{
		if (!previousCacheOnDisk || !AreCacheMismatchesRestorable(mismatches, isDeliberatelyDisabled))
			return false;

		outMismatches = std::move(mismatches);
		outAvailable = true;
		return true;
	}

	/// The first EnabledFlip mismatch (cache had it, now missing) whose feature
	/// failed to load rather than just being toggled off -- matching such a
	/// mismatch can't be resolved by a settings change alone.
	inline const CacheMismatch* FindMatchBlockingFeature(const std::vector<CacheMismatch>& mismatches,
		const std::function<bool(const std::string& shortName)>& featureFailedToLoad)
	{
		for (const auto& mismatch : mismatches) {
			if (mismatch.kind == CacheMismatch::Kind::EnabledFlip && !mismatch.nowPresent && featureFailedToLoad(mismatch.shortName))
				return &mismatch;
		}
		return nullptr;
	}

	/// Scan a root shader's include closure for a token. nullopt on any IO failure
	/// so callers fall back to the conservative full wipe.
	inline std::optional<bool> RootShaderReferencesToken(
		const std::filesystem::path& root, const std::string& token, const std::filesystem::path& shadersRoot)
	{
		try {
			static const std::regex includeRe(R"#(^\s*#\s*include\s+"([^"]+)")#");
			std::set<std::filesystem::path> visited;
			std::vector<std::filesystem::path> queue{ root };
			while (!queue.empty()) {
				// Normalize so relative spellings (Sub/../A.hlsli) can't defeat the
				// visited set and spin on a cycle.
				auto file = queue.back().lexically_normal();
				queue.pop_back();
				if (!visited.insert(file).second)
					continue;
				std::ifstream stream(file);
				if (!stream)
					return std::nullopt;
				std::string line;
				while (std::getline(stream, line)) {
					// Identifier-boundary match: UNIFIED_WATER must not hit UNIFIED_WATERX.
					for (size_t pos = line.find(token); pos != std::string::npos; pos = line.find(token, pos + 1)) {
						const auto isIdent = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; };
						const bool beforeOk = pos == 0 || !isIdent(line[pos - 1]);
						const bool afterOk = pos + token.size() >= line.size() || !isIdent(line[pos + token.size()]);
						if (beforeOk && afterOk)
							return true;
					}
					std::smatch m;
					if (std::regex_search(line, m, includeRe)) {
						// Includes resolve against the merged root, then the includer's dir.
						auto byRoot = shadersRoot / m[1].str();
						auto byLocal = file.parent_path() / m[1].str();
						if (std::filesystem::exists(byRoot))
							queue.push_back(byRoot);
						else if (std::filesystem::exists(byLocal))
							queue.push_back(byLocal);
						// Unresolvable includes are permutation-gated feature paths; the
						// token test above already covers the gating define on this line.
					}
				}
			}
			return false;
		} catch (...) {
			return std::nullopt;
		}
	}

	/// Rotate the active cache into the rollback slot (active -> previous; any old
	/// previous is discarded via the swap path), then recreate an empty active dir.
	/// Transactional: every failure moves directories back, so the active cache
	/// path is never left missing. English failure detail in outError.
	inline bool BackupCacheDirectory(
		const std::filesystem::path& active, const std::filesystem::path& previous,
		const std::filesystem::path& swap, std::string* outError = nullptr)
	{
		const auto fail = [&](std::string message) {
			if (outError)
				*outError = std::move(message);
			return false;
		};
		std::error_code ec;
		std::filesystem::remove_all(swap, ec);
		if (ec)
			return fail(std::format("could not clear the swap path: {}", ec.message()));

		const bool hadPrevious = std::filesystem::exists(previous, ec) && !ec;
		if (hadPrevious) {
			std::filesystem::rename(previous, swap, ec);
			if (ec)
				return fail(std::format("could not stash the old rollback cache: {}", ec.message()));
		}

		std::filesystem::rename(active, previous, ec);
		if (ec) {
			std::error_code undoEc;
			if (hadPrevious)
				std::filesystem::rename(swap, previous, undoEc);
			return fail(std::format("could not move the active cache into the rollback slot: {}", ec.message()));
		}

		std::filesystem::create_directories(active, ec);
		if (ec) {
			std::error_code undoEc;
			std::filesystem::rename(previous, active, undoEc);
			if (hadPrevious)
				std::filesystem::rename(swap, previous, undoEc);
			return fail(std::format("could not create the new active cache folder: {}", ec.message()));
		}

		if (hadPrevious)
			std::filesystem::remove_all(swap, ec);  // best effort; a stale swap is cleared next rotation
		return true;
	}

	/// Swap the rollback cache back into the active slot; the displaced active
	/// cache becomes the new rollback slot when possible. Transactional: on
	/// failure the active cache path is restored. A non-fatal inability to keep
	/// the displaced cache is reported via outWarning (restore still succeeds).
	inline bool RestoreCacheDirectory(
		const std::filesystem::path& active, const std::filesystem::path& previous,
		const std::filesystem::path& swap, std::string* outError = nullptr, std::string* outWarning = nullptr)
	{
		const auto fail = [&](std::string message) {
			if (outError)
				*outError = std::move(message);
			return false;
		};
		std::error_code ec;
		std::filesystem::remove_all(swap, ec);
		if (ec)
			return fail(std::format("could not clear the swap path: {}", ec.message()));

		const bool activeExists = std::filesystem::exists(active, ec) && !ec;
		if (activeExists) {
			std::filesystem::rename(active, swap, ec);
			if (ec)
				return fail(std::format("could not stash the active cache: {}", ec.message()));
		}

		std::filesystem::rename(previous, active, ec);
		if (ec) {
			std::error_code undoEc;
			if (activeExists)
				std::filesystem::rename(swap, active, undoEc);
			return fail(std::format("could not move the rollback cache into the active slot: {}", ec.message()));
		}

		if (activeExists) {
			std::filesystem::rename(swap, previous, ec);
			if (ec) {
				if (outWarning)
					*outWarning = std::format("the replaced cache could not be kept as the new rollback slot: {}", ec.message());
				std::filesystem::remove_all(swap, ec);
			}
		}
		return true;
	}

	/// Delete only the cache dirs whose root shader references any of the defines.
	/// Returns false (caller must full-wipe) on any empty define, missing root
	/// source, or scan failure -- conservative by construction.
	inline bool TryPartialInvalidation(
		const std::filesystem::path& cacheRoot, const std::filesystem::path& shadersRoot,
		const std::vector<std::string>& defines, size_t* outDeleted = nullptr, size_t* outKept = nullptr)
	{
		try {
			for (const auto& define : defines)
				if (define.empty())
					return false;
			size_t deleted = 0, kept = 0;
			for (const auto& entry : std::filesystem::directory_iterator(cacheRoot)) {
				if (!entry.is_directory())
					continue;
				const auto root = shadersRoot / (entry.path().filename().wstring() + L".hlsl");
				if (!std::filesystem::exists(root))
					return false;
				bool affected = false;
				for (const auto& define : defines) {
					auto refs = RootShaderReferencesToken(root, define, shadersRoot);
					if (!refs.has_value())
						return false;
					if (*refs) {
						affected = true;
						break;
					}
				}
				if (affected) {
					std::filesystem::remove_all(entry.path());
					++deleted;
				} else {
					++kept;
				}
			}
			if (outDeleted)
				*outDeleted = deleted;
			if (outKept)
				*outKept = kept;
			return true;
		} catch (...) {
			return false;
		}
	}
}
