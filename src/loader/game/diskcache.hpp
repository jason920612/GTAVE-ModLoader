#pragma once
#include <filesystem>
#include <string>
#include <vector>

// The world builder's lands on a hard disk (research/phase0.md §69; the user: the lands on the new 2 TB drive, memory taken
// for its cache). The game reads a land's pack through Windows' file cache - synchronous reads on its streaming threads, 26
// thousand of them (half under 4 KB) on the way into a land -, each a seek on a hard disk (9 ms): the packs of the land the
// player is on and of the lands they near are read in ahead, whole and in a row (at the disk's 105-115 MB/s), and held in
// memory, locked there - the game's reads of them come from it. The game links DirectStorage too (it has not used it on
// these drives); if it does, it is told to read through the cache as well (DirectStorage 1.2's ForceFileBuffering).
namespace loader::game::diskcache
{
	// At the start, MinHook initialised: DirectStorage told to read through the file cache when config bufferedIo is "auto"
	// and the game or a land folder (config landFolders) lies on a drive that seeks, or it is "on".
	void Start();
	// Whether a path lies on a drive that seeks (a hard disk; asked of Windows once a drive).
	bool Seeks(const std::filesystem::path& path);
	// Any thread: the packs wanted in memory now, the most wanted first. Those on a drive that seeks are read in, one at a
	// time - the first at the game's priority, the others behind its reads - and held, as many as config diskCacheGB takes;
	// the others let go.
	void Want(const std::vector<std::filesystem::path>& packs);
	// What it does and holds (the pipe's "diskcache").
	std::string Report();
	// The lands' cache on a fast drive (config landCache, paths::Cached): a land on a hard disk whose pack is all in memory
	// is copied there - its folder's files, the pack's reads coming from memory -, at the thread's background priority, a
	// land at a time; the least lately used lets go past config landCacheGB (one in use is not let go). Used from the
	// next start.
}
