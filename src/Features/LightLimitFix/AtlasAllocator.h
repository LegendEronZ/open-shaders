#pragma once

#include <array>
#include <cstdint>

// Pure buddy allocator for square power-of-two shadow atlas tiles (unit-testable, no game/D3D dependency).
namespace ShadowCasterManager
{
	class AtlasAllocator
	{
	public:
		// Quadtree depth: supports up to 128x128 cells per axis, covering a
		// 16384 atlas whose cell is a sixteenth-class tile of 2048 shadow
		// maps (128 px cells).
		static constexpr int kMaxLevels = 7;

		struct Tile
		{
			std::uint32_t x = 0;  ///< cell coords (multiply by cellSize for texels)
			std::uint32_t y = 0;
			std::uint32_t order = 0;  ///< tile spans 2^order cells per axis
			bool valid = false;
		};

		/// levels = log2(atlas cells per axis); e.g. an 8192 atlas with a 512
		/// minimum tile uses levels = 4 (16x16 cells).
		void Reset(std::uint32_t levels)
		{
			levelCount = levels > static_cast<std::uint32_t>(kMaxLevels) ? static_cast<std::uint32_t>(kMaxLevels) : levels;
			for (auto& n : nodes)
				n = NodeState::Free;
			usedCells = 0;
			pendingX = 0;
			pendingY = 0;
		}

		std::uint32_t CellsPerAxis() const { return 1u << levelCount; }

		/// Allocates a tile of 2^order cells per axis. Returns invalid when
		/// the atlas cannot fit it (caller walks down to a smaller order).
		Tile Allocate(std::uint32_t order)
		{
			if (order > levelCount)
				return {};
			const int idx = AllocateNode(0, levelCount, 0, 0, order);
			if (idx < 0)
				return {};
			Tile tile;
			tile.x = pendingX;
			tile.y = pendingY;
			tile.order = order;
			tile.valid = true;
			return tile;
		}

		/// Frees a previously allocated tile; merges buddies automatically.
		void Free(const Tile& tile)
		{
			if (tile.valid)
				FreeNode(0, levelCount, 0, 0, tile);
		}

		/// Fraction of cells currently allocated, for stats.
		float Occupancy() const
		{
			const std::uint32_t total = CellsPerAxis() * CellsPerAxis();
			return total ? static_cast<float>(usedCells) / static_cast<float>(total) : 0.0f;
		}

	private:
		enum class NodeState : std::uint8_t
		{
			Free,      ///< whole subtree unallocated
			Split,     ///< children carry the state
			Allocated  ///< whole subtree owned by one tile
		};

		// Full quadtree over kMaxLevels: sum of 4^i for i in [0, kMaxLevels].
		static constexpr std::size_t kNodeCount = (1u << (2 * (kMaxLevels + 1))) / 3;

		static constexpr std::size_t ChildIndex(std::size_t node, int child) { return node * 4 + 1 + child; }

		// Recursive first-fit descent. `level` counts down; a node at `level`
		// spans 2^level cells. The target order matches when level == order.
		int AllocateNode(std::size_t node, std::uint32_t level, std::uint32_t x, std::uint32_t y, std::uint32_t order)
		{
			if (nodes[node] == NodeState::Allocated)
				return -1;
			if (level == order) {
				if (nodes[node] != NodeState::Free)
					return -1;
				nodes[node] = NodeState::Allocated;
				usedCells += (1u << level) * (1u << level);
				pendingX = x;
				pendingY = y;
				return static_cast<int>(node);
			}
			if (nodes[node] == NodeState::Free)
				nodes[node] = NodeState::Split;
			const std::uint32_t half = 1u << (level - 1);
			for (int c = 0; c < 4; c++) {
				const std::uint32_t cx = x + (c & 1 ? half : 0);
				const std::uint32_t cy = y + (c & 2 ? half : 0);
				const int r = AllocateNode(ChildIndex(node, c), level - 1, cx, cy, order);
				if (r >= 0)
					return r;
			}
			// No child fit; collapse back to Free if all children are free so
			// a larger later request isn't blocked by an empty split.
			CollapseIfFree(node);
			return -1;
		}

		void FreeNode(std::size_t node, std::uint32_t level, std::uint32_t x, std::uint32_t y, const Tile& tile)
		{
			if (level == tile.order) {
				if (nodes[node] == NodeState::Allocated) {
					nodes[node] = NodeState::Free;
					usedCells -= (1u << level) * (1u << level);
				}
				return;
			}
			if (nodes[node] != NodeState::Split)
				return;
			const std::uint32_t half = 1u << (level - 1);
			const int cxBit = (tile.x >= x + half) ? 1 : 0;
			const int cyBit = (tile.y >= y + half) ? 2 : 0;
			const int c = cxBit | cyBit;
			FreeNode(ChildIndex(node, c), level - 1,
				x + (cxBit ? half : 0), y + (cyBit ? half : 0), tile);
			CollapseIfFree(node);
		}

		void CollapseIfFree(std::size_t node)
		{
			if (nodes[node] != NodeState::Split)
				return;
			for (int c = 0; c < 4; c++) {
				if (nodes[ChildIndex(node, c)] != NodeState::Free)
					return;
			}
			nodes[node] = NodeState::Free;
		}

		std::array<NodeState, kNodeCount> nodes{};
		std::uint32_t levelCount = 0;
		std::uint32_t usedCells = 0;
		std::uint32_t pendingX = 0;
		std::uint32_t pendingY = 0;
	};
}
