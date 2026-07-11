#pragma once

#include <d3d11.h>
#include <functional>
#include <vector>
#include <winrt/base.h>

namespace Util
{
	/**
	 * @brief One D3D11 timestamp-disjoint bracket with paired begin/end
	 * timestamp queries.
	 *
	 * Owns the query objects and the readback protocol (DONOTFLUSH polling,
	 * raw ticks + frequency out, so consumers keep their own precision and
	 * units). Consumers own the batching policy: frame rings, latency,
	 * and any payloads keyed by the returned interval index.
	 *
	 * Interval contract: BeginInterval stamps into the CURRENT slot without
	 * advancing and returns its index; CommitInterval stamps the end and
	 * advances. An interval that is begun but never committed is simply
	 * overwritten by the next BeginInterval and never resolved.
	 */
	class TimestampQueryBatch
	{
	public:
		/// Sets capacity and the debug-name prefix for the D3D objects.
		/// No device work; call once before first use.
		void Configure(uint32_t a_maxPairs, const char* a_debugName);

		/// Eagerly creates the disjoint and every pair, for consumers that
		/// cannot afford lazy creation mid-pass. Lazy consumers can skip
		/// this; BeginInterval creates pairs on demand.
		bool Preallocate(ID3D11Device* a_device);

		void ReleaseQueries();

		/// Opens the disjoint bracket. false when query creation failed.
		bool BeginBatch(ID3D11Device* a_device, ID3D11DeviceContext* a_context);

		/// Stamps the current interval's begin; returns its index, or -1 at
		/// capacity / on creation failure. Does not advance.
		int BeginInterval(ID3D11Device* a_device, ID3D11DeviceContext* a_context);

		/// Stamps the current interval's end and advances to the next slot.
		void CommitInterval(ID3D11DeviceContext* a_context);

		/// Closes the disjoint bracket.
		void EndBatch(ID3D11DeviceContext* a_context);

		enum class Status
		{
			NotReady,  ///< disjoint result not available yet; poll again later
			Disjoint,  ///< bracket spanned a clock event; discard the samples
			Ok
		};

		/// Polls with DONOTFLUSH (never stalls). On Ok, calls
		/// a_visit(intervalIndex, deltaTicks, frequency) for every committed
		/// interval whose two timestamps resolved with end > begin; intervals
		/// still unresolved are skipped, not reported.
		Status TryResolve(ID3D11DeviceContext* a_context,
			const std::function<void(uint32_t, uint64_t, uint64_t)>& a_visit) const;

		/// Committed interval count for the current bracket.
		uint32_t Used() const { return used; }

		bool CreationFailed() const { return creationFailed; }

		/// Forgets committed intervals so the batch can record a new bracket.
		void Reset() { used = 0; }

	private:
		struct Pair
		{
			winrt::com_ptr<ID3D11Query> begin;
			winrt::com_ptr<ID3D11Query> end;
		};

		bool EnsureDisjoint(ID3D11Device* a_device);
		bool EnsurePair(ID3D11Device* a_device, uint32_t a_index);

		winrt::com_ptr<ID3D11Query> disjoint;
		std::vector<Pair> pairs;
		uint32_t maxPairs = 0;
		uint32_t used = 0;
		bool creationFailed = false;
		const char* debugName = "TimestampQueryBatch";
	};
}
