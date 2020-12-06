#include <chrono>
#include <mutex>

#include "mainwindow.h"
#include "./ui_mainwindow.h"

#include <QMouseEvent>
#include <QTimer>

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

struct MainWindow::Tile
{
public:
	static constexpr int size = 256;

	std::atomic<QImage*> rendered;
	std::atomic_bool running = false;
private:
	QImage* dflt;
	std::array<QImage, 4> mips;
	int currentMip = 0;
	int currentY = 0;
	Complex corner, diag;
	bool interrupt = false;
	std::mutex mut;
	friend MainWindow;

	static std::uint8_t mand(Complex c) noexcept
	{
		Complex z = {0, 0};

		constexpr int MSTEPS = 255;
		for (int i = 0; i < MSTEPS; i++)
			if (std::abs(z) >= 2)
				return i % 64;
			else
				z = z * z + c;
		return 0;
	}
public:
	Tile(QImage* dflt) : dflt(dflt), rendered(dflt)
	{
		assert(dflt != nullptr);
		mips[0] = QImage(size / 32, size / 32, QImage::Format::Format_RGB888);
		mips[1] = QImage(size /  8, size /  8, QImage::Format::Format_RGB888);
		mips[2] = QImage(size /  2, size /  2, QImage::Format::Format_RGB888);
		mips[3] = QImage(size, size, QImage::Format::Format_RGB888);
	}
	Tile(Tile const&) = delete;
	Tile(Tile&&) = delete;
	void operator=(Tile const&) = delete;
	void operator=(Tile&&) = delete;

	void Interrupt()
	{
		auto locker = std::lock_guard(mut);
		interrupt = true;
	}

	// thread safe
	int GetPrior(QImage const* img) const noexcept
	{
		if (img == dflt)
			return 25;
		return mips.size() - (img - &mips.front());
	}
	// thread safe
	bool IsLast(QImage const* img) const noexcept
	{
		return img == &mips.back();
	}

	// to call from main thread
	void Set(Complex corner, Complex diag) noexcept
	{
		auto locker = std::lock_guard(mut);
		interrupt = true;
		this->corner = corner;
		this->diag = diag;
		currentMip = 0;
		currentY = 0;

		rendered.store(dflt);
	}
	// to call from drawer
	bool Update() noexcept
	{
		int yd = 0;
		std::unique_ptr<Tile, std::function<void(Tile*)>> runningResetter = {this, [](Tile* a) { a->running.store(false); }};
		while (true)
		{
			int y;
			{
				auto lk = std::lock_guard(mut);
				if (currentMip == mips.size())
					return false;
				if (interrupt)
				{
					interrupt = false;
					return false;
				}
				// may fire only during first iteration
				currentY += yd;
				if (currentY == mips[currentMip].height())
				{
					rendered.store(&mips[currentMip]);
					currentY = 0;
					currentMip++;
					return currentMip != mips.size();
				}
				y = currentY;
			}

			yd = 1;

			auto& img = mips[currentMip];
			int
				h = img.height(),
				w = img.width();

			std::uint8_t* data = img.bits() + y * img.bytesPerLine();
			auto yy = (PrecType)y / h * diag.imag() + corner.imag();
			for (int x = 0; x < w; x++)
			{
				auto xx = (PrecType)x / w * diag.real() + corner.real();
				auto val = mand({xx, yy});
				data[x * 3 + 0] = val * 4;
				data[x * 3 + 1] = val / 2;
				data[x * 3 + 2] = val % 3 * 127;
			}
		}
	}
};

void MainWindow::Threading::ThreadData::ThreadFunc()
{
	// alias
	auto& queue = thr->tasks;
	bool put = false;
	int prior;
	Tile* tile;
	while (running.load())
	{
		{
			auto lck = std::unique_lock(thr->mut);
			if (put)
			{
				tile->running.store(true);
				queue.push({prior - 1, tile});
				put = false;
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

MainWindow::TileHelper::TileHelper() : dflt(Tile::size, Tile::size, QImage::Format::Format_RGB888)
{
	dflt.fill(0);
}
MainWindow::TileHelper::~TileHelper()
{
	InvalidateTiles();
	for (auto const& a : pool)
		delete a;
}
void MainWindow::TileHelper::InvalidateTiles() noexcept
{
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
	auto ret = new Tile(&dflt);
	return ret;
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

void MainWindow::resizeEvent(QResizeEvent* ev)
{
	QMainWindow::resizeEvent(ev);
	tilesData.InvalidateTiles();
}
void MainWindow::paintEvent(QPaintEvent* ev)
{
	QMainWindow::paintEvent(ev);
	bool needsRerender = false;

	int width = this->width();
	int height = this->height();

	QPainter painter(this);
	int xcamoffset = xcoord % Tile::size;
	int ycamoffset = ycoord % Tile::size;
	if (xcamoffset <= 0)
		xcamoffset += Tile::size;
	if (ycamoffset <= 0)
		ycamoffset += Tile::size;
	for (int y = -Tile::size; y <= height; y += Tile::size)
	{
		int ry = y - ycoord;
		if (ry >= 0)
			ry = ry / Tile::size;
		else
			ry = (ry - Tile::size + 1) / Tile::size;
		ry *= Tile::size;
		for (int x = -Tile::size; x <= width; x += Tile::size)
		{
			int rx = x - xcoord;
			if (rx >= 0)
				rx = rx / Tile::size;
			else
				rx = (rx - Tile::size + 1) / Tile::size;
			rx *= Tile::size;

			Complex corner = {
				rx * scale,
				ry * scale
			};
			corner += zeroPixelCoord;
			auto* tile = tilesData.GetTile(rx, ry, corner, {Tile::size * scale, Tile::size * scale});
			auto img = tile->rendered.load();
			{
				if (!tile->running.load())
				{
					auto prior = tile->GetPrior(img);
					tile->running.store(true);
					{
						auto guard = std::lock_guard(threading.mut);
						threading.tasks.push({prior, tile});
					}
					// bad, because we may render low priority task
					// threading.cv.notify_one();
				}
			}
			needsRerender |= !tile->IsLast(img);
			int ratio = Tile::size / img->width();
#if 0
			painter.setViewport(xcamoffset + x, ycamoffset + y, width * ratio, height * ratio);
#else
			painter.setTransform(
					QTransform(
						ratio, 0,
						0,     ratio,

						xcamoffset + x,
						ycamoffset + y
				));
#endif
			painter.drawImage(0, 0, *tile->rendered.load());
			auto txt = std::to_string(rx);
			// painter.drawText(0, 0, txt.c_str());
		} // x cycle
	} // y cycle
	if (needsRerender)
	{
		threading.cv.notify_all();
		Schedule();
	}
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
	zeroPixelCoord -= scale * Complex(xcoord, ycoord);
	xcoord = ycoord = 0;
	PrecType dd = d / 500.0;
	if (d > 0)
		scale *= 1.1;
	else
		scale /= 1.1;
	tilesData.InvalidateTiles();
	this->update();
}
void MainWindow::mouseMoveEvent(QMouseEvent* e)
{
	QMainWindow::mouseMoveEvent(e);
	/*
	if (e->button() != Qt::LeftButton)
		return;
		*/
	auto curt = std::chrono::system_clock::now();
	auto delta = std::chrono::duration_cast<std::chrono::milliseconds>(curt - mouseData.lastUpd).count();
	mouseData.lastUpd = curt;
	auto cp = e->screenPos();
	int
		lx = mouseData.lastX,
		ly = mouseData.lastY,
		cx = cp.x(),
		cy = cp.y();
	mouseData.lastX = cx;
	mouseData.lastY = cy;
	if (delta > 50)
		return;
	xcoord += (cx - lx);
	ycoord += (cy - ly);

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
