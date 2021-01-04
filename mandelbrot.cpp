#include "tile.h"
#include "mandelbrot.h"

#include <iostream>

using PrecType = MandelbrotHolder::PrecType;
using Complex = MandelbrotHolder::Complex;

MandelbrotHolder::MandelbrotHolder(std::function<void()> scheduler)
	: scheduler(std::move(scheduler))
	, threading(std::thread::hardware_concurrency() > 1 ? std::thread::hardware_concurrency() - 1 : 1)
{
	for (auto& a : threading.threads)
	{
		a.second.thr = &threading;
		a.first = std::thread(&Threading::ThreadData::ThreadFunc, &a.second);
	}
}

void MandelbrotHolder::Threading::ThreadData::ThreadFunc()
{
	// alias
	auto& queue = thr->tasks;
	Tile::UpdateStatus put = Tile::DONE;
	int prior;
	Tile* tile;
	while (running.load())
	{
		{
			auto lck = std::unique_lock(thr->mut);
			if (put == Tile::UPDATED || put == Tile::INTERRUPT_AND_PUT)
			{
				tile->running.store(true);
				queue.push({prior - 1, tile});
			}
			else
			{
				thr->cv.wait(lck, [&]() {
						return !(queue.empty() && running.load());
					});
			}
			if (!running.load())
				break;

			prior = queue.top().prior;
			tile = queue.top().tile;
			queue.pop();
		}
		put = tile->Update();
	}
}

MandelbrotHolder::TileHelper::TileHelper()
{}
MandelbrotHolder::TileHelper::~TileHelper()
{
	InvalidateTiles();
	for (auto const& a : pool)
		delete a;
}
void MandelbrotHolder::TileHelper::InvalidateTiles() noexcept
{
	thumbnail.tile = nullptr;
	for (auto& a : cache)
	{
		a.second->Interrupt();
		pool.push_back(a.second);
	}
	cache.clear();
}
MandelbrotHolder::Tile* MandelbrotHolder::TileHelper::GetTile(int x, int y, Complex corner, Complex diag) noexcept
{
	auto iter = cache.lower_bound({x, y});
	if (iter == cache.end() || iter->first != PixCoord(x, y))
	{
		Tile* ins = GetFromPool();
		ins->Set(corner, diag);
		iter = cache.emplace_hint(iter, PixCoord(x, y), ins);
	}
	return iter->second;
}
MandelbrotHolder::Tile* MandelbrotHolder::TileHelper::Allocate() noexcept
{
	return new Tile(nullptr);
}
MandelbrotHolder::Tile* MandelbrotHolder::TileHelper::GetFromPool() noexcept
{
	if (!pool.empty())
	{
		auto ret = pool.back();
		pool.pop_back();
		return ret;
	}
	return Allocate();
}

void MandelbrotHolder::Scale(PrecType dd)
{
	coordSys.zeroPixelCoord -= coordSys.scale * Complex(coordSys.xcoord, coordSys.ycoord);
	coordSys.xcoord = coordSys.ycoord = 0;
	coordSys.scale *= std::pow(1.09, dd);
	tilesData.InvalidateTiles();
}

void MandelbrotHolder::Move(int dx, int dy)
{
	coordSys.xcoord += dx;
	coordSys.ycoord += dy;
}

void MandelbrotHolder::RenderSmth(QPainter& painter, int width, int height)
{
	int wh = std::max(width, height);
	Complex diag = Complex(wh, wh) * coordSys.scale;
	Complex offset = coordSys.zeroPixelCoord - Complex(Tile::size + coordSys.xcoord, Tile::size + coordSys.ycoord) * coordSys.scale;
	bool needsRerender = false;
	auto*& tile = tilesData.thumbnail.tile;
	auto& tx = tilesData.thumbnail.x; // read as tile_x
	auto& ty = tilesData.thumbnail.y;
	if (tile == nullptr)
	{
		constexpr auto intm = std::numeric_limits<int>::max();
		tile = tilesData.GetTile(intm, intm, offset, diag);
		tile->Update();
		tx = coordSys.xcoord;
		ty = coordSys.ycoord;
	}
	else if (tx != coordSys.xcoord || ty != coordSys.ycoord)
	{
		tile->Set(offset, diag);
		tile->Update();
		tx = coordSys.xcoord;
		ty = coordSys.ycoord;
	}
	auto* img = tile->rendered.load();
	assert(img != nullptr);
	auto ratio = (PrecType)wh / img->width();
	painter.setTransform(
			QTransform(
				ratio, 0,
				0,     ratio,

				0,
				0
		));
	painter.drawImage(0, 0, *img);
}

void MandelbrotHolder::Render(QPainter &painter, int width, int height)
{
	bool needsRerender = false;

	RenderSmth(painter, width, height);

	int xcamoffset = coordSys.xcoord % Tile::size;
	int ycamoffset = coordSys.ycoord % Tile::size;
	if (xcamoffset <= 0)
		xcamoffset += Tile::size;
	if (ycamoffset <= 0)
		ycamoffset += Tile::size;

	Threading::TileWithPrior prevTile = {100, nullptr};
	bool needsInvalidation = tilesData.cache.size() > (height / Tile::size + 2) * (width / Tile::size + 2) * 4;
	for (int y = -Tile::size; y <= height; y += Tile::size)
	{
		int ry = y - coordSys.ycoord;
		if (ry >= 0)
			ry = ry / Tile::size;
		else
			ry = (ry - Tile::size + 1) / Tile::size;
		ry *= Tile::size;
		for (int x = -Tile::size; x <= width; x += Tile::size)
		{
			int rx = x - coordSys.xcoord;
			if (rx >= 0)
				rx = rx / Tile::size;
			else
				rx = (rx - Tile::size + 1) / Tile::size;
			rx *= Tile::size;

			Complex corner = Complex(rx, ry) * coordSys.scale;
			corner += coordSys.zeroPixelCoord;
			auto* tile = tilesData.GetTile(rx, ry, corner, Complex(Tile::size, Tile::size) * coordSys.scale);
			if (needsInvalidation)
				usedTiles.used.emplace(tile);
			auto img = tile->rendered.load();
			if (!tile->IsLast(img))
			{
				needsRerender = true;
				if (!tile->running.load())
				{
					usedTiles.Add(tile);
					auto prior = tile->GetPrior(img);
					Threading::TileWithPrior putMe = {prior, tile};
					tile->running.store(true);
					{
						auto guard = std::lock_guard(threading.mut);
						threading.tasks.push(putMe);
					}
					if (prevTile < putMe)
						prevTile.tile->Interrupt(Tile::INTERRUPT_AND_PUT);
					prevTile = putMe;
					threading.cv.notify_one();
				}
			}
			if (img != nullptr)
			{
				int ratio = Tile::size / img->width();
				painter.setTransform(
						QTransform(
							ratio, 0,
							0,     ratio,

							xcamoffset + x,
							ycamoffset + y
					));
				painter.drawImage(0, 0, *img);
			}
		} // x cycle
	} // y cycle
	if (needsInvalidation)
		std::cout << "invalidating cache: " << usedTiles.InvalidateCache(tilesData) << " removed" << std::endl;
	usedTiles.Finish();
	if (needsRerender)
	{
		// threading.cv.notify_all();
		scheduler();
	}
}

MandelbrotHolder::~MandelbrotHolder()
{
	tilesData.InvalidateTiles();
	for (auto& a : threading.threads)
		a.second.running.store(false);
	threading.cv.notify_all();
	for (auto& a : threading.threads)
		a.first.join();
}
