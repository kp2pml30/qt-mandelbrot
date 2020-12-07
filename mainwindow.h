#pragma once

#include <chrono>
#include <complex>
#include <condition_variable>
#include <queue>
#include <thread>
#include <mutex>
#include <functional>
#include <unordered_set>

#include <QMainWindow>
#include <QPainter>
#include <QTimer>

QT_BEGIN_NAMESPACE
namespace Ui
{
	class MainWindow;
}
QT_END_NAMESPACE

class MainWindow : public QMainWindow
{
	Q_OBJECT

public:
	MainWindow(QWidget* parent = nullptr);
	~MainWindow();

	void paintEvent(QPaintEvent*) override;
	void mouseMoveEvent(QMouseEvent*) override;
	void wheelEvent(QWheelEvent*) override;

	using PrecType = double;
	// this complex would be used as vec2 as well
	using Complex = std::complex<PrecType>;
private:
	std::unique_ptr<Ui::MainWindow> ui;

	struct {
		int lastX = 0;
		int lastY = 0;
		std::chrono::time_point<std::chrono::system_clock> lastUpd = std::chrono::system_clock::now();
	} mouseData;

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

	struct {
		Complex zeroPixelCoord = {-2, -2};
		PrecType scale = 1 / 256.0;
		int
			xcoord = 0,
			ycoord = 0;
	} coordSys;

	struct TileHelper
	{
		std::vector<Tile*> pool;
		using PixCoord = std::pair<int, int>;
		std::map<PixCoord, Tile*> cache;
		Tile* inCaseOfBlack = nullptr;

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

	void Schedule();
};
