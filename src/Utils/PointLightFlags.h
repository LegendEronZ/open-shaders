#pragma once

#include <bit>
#include <cstdint>

namespace PointLightFlags
{
	enum class Flags : std::uint32_t
	{
		PortalStrict = (1u << 0),
		Shadow = (1u << 1),
		Simple = (1u << 2),

		Initialised = (1u << 8),
		Disabled = (1u << 9),
		InverseSquare = (1u << 10),
		Linear = (1u << 11),
		Particle = (1u << 12),
		Spot = (1u << 13),
		OmniDirectional = (1u << 14),
	};

	constexpr std::uint32_t ToMask(Flags a_flag) noexcept
	{
		return static_cast<std::uint32_t>(a_flag);
	}

	/// @brief True when an object still has a vtable pointer, i.e. has not been destroyed.
	inline bool HasLiveVTable(const void* a_object) noexcept
	{
		return a_object && *static_cast<void* const*>(a_object) != nullptr;
	}

	inline std::uint32_t GetRuntimeLightFlags(RE::NiLight* a_niLight) noexcept
	{
		if (!a_niLight)
			return 0;

		auto& runtimeData = a_niLight->GetLightRuntimeData();
		const std::uint32_t flags = std::bit_cast<std::uint32_t>(runtimeData.ambient.red);
		return (flags & ToMask(Flags::Initialised)) != 0 ? flags : 0;
	}

	inline std::uint32_t GetPointLightTypeFlags(RE::BSLight* a_bsLight) noexcept
	{
		if (!a_bsLight || !a_bsLight->pointLight)
			return 0;

		// RTDynamicCast reads the vtable pointer to reach the complete object locator. On a
		// destroyed light that read faults, and the resulting throw crosses noexcept: terminate,
		// not a null return, so the cast must not be reached rather than guarded after the fact.
		if (!HasLiveVTable(a_bsLight))
			return 0;

		auto* shadowLight = skyrim_cast<RE::BSShadowLight*>(a_bsLight);
		return shadowLight && shadowLight->GetIsFrustumLight() ? ToMask(Flags::Spot) : ToMask(Flags::OmniDirectional);
	}

	inline std::uint32_t GetVanillaPointLightFlags(RE::BSLight* a_bsLight, RE::NiLight* a_niLight) noexcept
	{
		constexpr std::uint32_t typeMask = ToMask(Flags::Spot) | ToMask(Flags::OmniDirectional);
		std::uint32_t flags = GetRuntimeLightFlags(a_niLight) & (ToMask(Flags::Linear) | typeMask);
		if ((flags & typeMask) == 0)
			flags |= GetPointLightTypeFlags(a_bsLight);
		return flags;
	}

	inline void SetPointLightTypeFlags(stl::enumeration<Flags>& a_flags, RE::BSLight* a_bsLight) noexcept
	{
		if (!a_bsLight || !a_bsLight->pointLight)
			return;
		if (a_flags.any(Flags::Spot, Flags::OmniDirectional))
			return;

		if ((GetPointLightTypeFlags(a_bsLight) & ToMask(Flags::Spot)) != 0) {
			a_flags.set(Flags::Spot);
			a_flags.reset(Flags::OmniDirectional);
		} else {
			a_flags.reset(Flags::Spot);
			a_flags.set(Flags::OmniDirectional);
		}
	}
}
