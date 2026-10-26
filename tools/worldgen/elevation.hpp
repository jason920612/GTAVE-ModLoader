#pragma once
#include <array>
#include <cstdint>
#include <filesystem>
#include <list>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

// Measured land (research/phase0.md §61.18): the rasters tools/worldgen/lidar.py and aerial.py write in game
// coordinates, read a tile at a time as they are asked for (the least recently used dropped past a limit).
//   Scan (t_<i>_<j>.bin, 'WBLR' version 2): heights from airborne laser scans, 0.5 m cells: the highest return (roofs,
//   crowns, bare ground), the ground's mean (class 2), the highest building return (class 6); returns and classes.
//   Aerial (a_<i>_<j>.bin, 'WBAI'): orthophotos, 0.25 m pixels: red, green, blue, near infrared.
// Heights are metres above NAVD88 (in Miami within a few centimetres of the mean sea level); NaN: not measured.
namespace worldgen::elevation
{
	enum Flags : uint8_t
	{
		kWater = 1,
		kBridge = 2,
		kBuilding = 4,
		kVegetation = 8,
	};

	template <typename Tile>
	class Tiles
	{
	public:
		bool Open(const std::filesystem::path& folder, const char* prefix, size_t keep);
		bool Empty() const { return names_.empty(); }
		float Size() const { return size_; }
		// the tile holding (x, y) (nullptr: none there); the cell inside it
		const Tile* At(float x, float y, int& i, int& j);

	private:
		std::map<std::pair<int, int>, std::filesystem::path> names_;
		std::map<std::pair<int, int>, std::pair<std::unique_ptr<Tile>, std::list<std::pair<int, int>>::iterator>> loaded_;
		std::list<std::pair<int, int>> order_; // most recently used first
		size_t keep_ = 64;
		float size_ = 256, cell_ = 0.5f;
		int n_ = 512;
	};

	struct ScanTile
	{
		float x0 = 0, y0 = 0, cell = 0.5f;
		int n = 0;
		std::vector<int16_t> top, ground, roof;
		std::vector<uint8_t> count, flags;
		bool Load(const std::filesystem::path& file);
	};

	struct AerialTile
	{
		float x0 = 0, y0 = 0, cell = 0.25f;
		int n = 0;
		std::vector<std::array<uint8_t, 4>> pixels;
		bool Load(const std::filesystem::path& file);
	};

	class Scan
	{
	public:
		// `offset`: added to every height (the game's sea is at 0; a little above the scan's datum keeps low land dry)
		bool Open(const std::filesystem::path& folder, float offset = 0)
		{
			offset_ = offset;
			return tiles_.Open(folder, "t_", 256);
		}
		bool Empty() const { return tiles_.Empty(); }
		float Cell() const { return 0.5f; }
		float Top(float x, float y) { return Value(x, y, &ScanTile::top); }
		float Ground(float x, float y) { return Value(x, y, &ScanTile::ground); }
		float Roof(float x, float y) { return Value(x, y, &ScanTile::roof); }
		uint8_t Flags(float x, float y);
		uint8_t Count(float x, float y);

	private:
		float Value(float x, float y, std::vector<int16_t> ScanTile::*layer);
		Tiles<ScanTile> tiles_;
		float offset_ = 0;
	};

	class Aerial
	{
	public:
		bool Open(const std::filesystem::path& folder) { return tiles_.Open(folder, "a_", 128); }
		bool Empty() const { return tiles_.Empty(); }
		// bilinear; false where there is no picture
		bool Sample(float x, float y, std::array<float, 4>& out);

	private:
		Tiles<AerialTile> tiles_;
	};
}
