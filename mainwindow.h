#pragma once

#include <chrono>
#include <complex>
#include <queue>
#include <thread>
#include <mutex>
#include <functional>

#include <QMainWindow>
#include <QPainter>

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

	void resizeEvent(QResizeEvent*) override;
	void paintEvent(QPaintEvent*) override;
	void mouseMoveEvent(QMouseEvent*) override;
	void wheelEvent(QWheelEvent*) override;

	using PrecType = double;
	// this complex would be used as vec2 as well
	using Complex = std::complex<PrecType>;

private:
	std::unique_ptr<Ui::MainWindow> ui;

	int xcoord = 0, ycoord = 0;

	struct
	{
		int lastX = 0;
		int lastY = 0;
		std::chrono::time_point<std::chrono::system_clock> lastUpd = std::chrono::system_clock::now();
	} mouseData;

	struct Tile;
	friend Tile;

	struct Threading
	{
		std::list<Tile*> tasks;
		std::mutex mut;

		Threading(Threading&&) = delete;
		Threading(Threading const&) = delete;

		struct ThreadData
		{
			std::atomic_bool running = true;
			Threading* thr = nullptr;
		};
		static void ThreadFunc(ThreadData* data);
		std::vector<std::pair<std::thread, ThreadData>> threads;

		Threading(std::size_t size)
		: threads(size)
		{}
	} threading;
	friend Threading;

	Complex zeroPixelCoord = {-2, -2};
	PrecType scale = 1 / 256.0;

	struct TileHelper
	{
		std::vector<Tile*> pool;
		using PixCoord = std::pair<int, int>;
		std::map<PixCoord, Tile*> cache;

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

	void Schedule();
};
