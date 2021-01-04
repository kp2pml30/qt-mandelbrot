#pragma once

#include <complex>
#include <condition_variable>
#include <queue>
#include <thread>
#include <mutex>
#include <QPainter>
#include <unordered_set>
#include <functional>

class MandelbrotHolder
{
public:
	using PrecType = double;
	// this complex would be used as vec2 as well
	using Complex = std::complex<PrecType>;
private:

	struct Tile;
	friend Tile;

	struct Threading
	{
		struct TileWithPrior
		{
			int prior;
			Tile* tile;

			int operator<=>(TileWithPrior const& r) const
			{
				return prior - r.prior;
			}
		};

		std::priority_queue<TileWithPrior> tasks;
		std::mutex mut;
		std::condition_variable cv;

		Threading(Threading&&) = delete;
		Threading(Threading const&) = delete;

		struct ThreadData
		{
			std::atomic_bool running = true;
			Threading* thr = nullptr;

			void ThreadFunc();
		};
		std::vector<std::pair<std::thread, ThreadData>> threads;

		Threading(std::size_t size)
		: threads(size)
		{}
	} threading;
	friend Threading;

public:
	struct {
		Complex zeroPixelCoord = {-2, -2};
		PrecType scale = 1 / 256.0;
		int
			xcoord = 0,
			ycoord = 0;
	} coordSys;
private:

	struct TileHelper
	{
	public:
		std::vector<Tile*> pool;
		using PixCoord = std::pair<int, int>;
		std::map<PixCoord, Tile*> cache;

		struct {
			std::unique_ptr<Tile> tile;
			int x = 0, y = 0;
			PrecType scale = -1;
		} thumbnail;

		// from main thread only
		TileHelper();
		~TileHelper();
		void InvalidateTiles() noexcept;
		Tile* GetTile(int x, int y, Complex cornder, Complex diag) noexcept;
	private:
		Tile* GetFromPool() noexcept;
		Tile* Allocate() noexcept;

		QImage dflt;
	} tilesData;

	struct UsedTiles
	{
		std::unordered_set<Tile*>
			prev,
			cur,
			used;

		void Add(Tile*) noexcept;
		void Finish() noexcept;
		std::size_t InvalidateCache(TileHelper&) noexcept;
	} usedTiles;

	// to call update
	std::function<void()> scheduler;

	void RenderSmth(QPainter& painter, int width, int height);
public:
	MandelbrotHolder(std::function<void()> scheduler);
	void Render(QPainter& painter, int width, int height);
	void Scale(PrecType by);
	void Move(int dx, int dy);

	~MandelbrotHolder();
};

