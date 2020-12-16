#include <chrono>
#include <mutex>

#include "tile.h"
#include "./ui_mainwindow.h"

#include <QMouseEvent>
#include <QTimer>

#include <iostream>
#include <qnamespace.h>

using PrecType = MainWindow::PrecType;
using Complex = MainWindow::Complex;

MainWindow::MainWindow(QWidget* parent)
	: QMainWindow(parent)
	, ui(new Ui::MainWindow)
	, threading(std::thread::hardware_concurrency() > 1 ? std::thread::hardware_concurrency() - 1 : 1)
{
	ui->setupUi(this);

	for (auto& a : threading.threads)
	{
		a.second.thr = &threading;
		a.first = std::thread(&Threading::ThreadData::ThreadFunc, &a.second);
	}

	Schedule();
}

void MainWindow::Threading::ThreadData::ThreadFunc()
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

MainWindow::TileHelper::TileHelper()
{}
MainWindow::TileHelper::~TileHelper()
{
	InvalidateTiles();
	for (auto const& a : pool)
		delete a;
}
void MainWindow::TileHelper::InvalidateTiles() noexcept
{
	inCaseOfBlack = nullptr;
	for (auto& a : cache)
	{
		a.second->Interrupt();
		pool.push_back(a.second);
	}
	cache.clear();
}
MainWindow::Tile* MainWindow::TileHelper::GetTile(int x, int y, Complex corner, Complex diag) noexcept
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
MainWindow::Tile* MainWindow::TileHelper::Allocate() noexcept
{
	return new Tile(nullptr);
}
MainWindow::Tile* MainWindow::TileHelper::GetFromPool() noexcept
{
	if (!pool.empty())
	{
		auto ret = pool.back();
		pool.pop_back();
		return ret;
	}
	return Allocate();
}

void MainWindow::paintEvent(QPaintEvent* ev)
{
	QMainWindow::paintEvent(ev);
	bool needsRerender = false;

	int width = this->width();
	int height = this->height();

	QPainter painter(this);

	if (coordSys.xcoord == 0 && coordSys.ycoord == 0)
	{
		int wh = std::max(width, height);
		if (tilesData.inCaseOfBlack == nullptr)
		{
			constexpr auto intm = std::numeric_limits<int>::max();
			auto offset = Complex(Tile::size, Tile::size) * coordSys.scale;
			tilesData.inCaseOfBlack = tilesData.GetTile(intm, intm, coordSys.zeroPixelCoord - offset, Complex(wh, wh) * coordSys.scale);
			// for interrupt here :)
			tilesData.inCaseOfBlack->Update();
			tilesData.inCaseOfBlack->Update();
		}
		auto* img = tilesData.inCaseOfBlack->rendered.load();
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
	// render at least something to show
	if (needsRerender)
	{
		// threading.cv.notify_all();
		Schedule();
	}
	ev->accept();
}

void MainWindow::Schedule()
{
	QTimer::singleShot(10, this, [this](){ this->update(); });
}
void MainWindow::wheelEvent(QWheelEvent* ev)
{
	QMainWindow::wheelEvent(ev);

	auto d = ev->angleDelta().ry();
	if (d == 0)
		return;
	coordSys.zeroPixelCoord -= coordSys.scale * Complex(coordSys.xcoord, coordSys.ycoord);
	coordSys.xcoord = coordSys.ycoord = 0;
	PrecType dd = d / 90.0;
	coordSys.scale *= std::pow(1.09, dd);
	tilesData.InvalidateTiles();
	this->update();
	ev->accept();
}
void MainWindow::mouseReleaseEvent(QMouseEvent* ev)
{
	if (ev->button() == Qt::LeftButton)
	{
		mouseData.enabled = false;
		ev->accept();
	}
	else
	{
		ev->ignore();
	}
}
void MainWindow::mousePressEvent(QMouseEvent* ev)
{
	if (ev->button() == Qt::LeftButton)
	{
		mouseData.enabled = true;
		auto cp = ev->screenPos();
		mouseData.lastX = cp.x();
		mouseData.lastY = cp.y();
		ev->accept();
	}
	else
	{
		ev->ignore();
	}
}
void MainWindow::mouseMoveEvent(QMouseEvent* ev)
{
	QMainWindow::mouseMoveEvent(ev);
	if (!mouseData.enabled)
	{
		ev->ignore();
		return;
	}
	auto curt = std::chrono::system_clock::now();
	auto delta = std::chrono::duration_cast<std::chrono::milliseconds>(curt - mouseData.lastUpd).count();
	mouseData.lastUpd = curt;
	auto cp = ev->screenPos();
	int
		lx = mouseData.lastX,
		ly = mouseData.lastY,
		cx = cp.x(),
		cy = cp.y();
	mouseData.lastX = cx;
	mouseData.lastY = cy;
	// if (delta > 50)
	// 	return;
	coordSys.xcoord += (cx - lx);
	coordSys.ycoord += (cy - ly);

	ev->accept();
	QMainWindow::update();
}

MainWindow::~MainWindow()
{
	tilesData.InvalidateTiles();
	for (auto& a : threading.threads)
		a.second.running.store(false);
	threading.cv.notify_all();
	for (auto& a : threading.threads)
		a.first.join();
}
