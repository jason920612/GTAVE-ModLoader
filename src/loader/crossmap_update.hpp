#pragma once
#include <chrono>
#include <filesystem>

namespace loader::crossmap
{
	// Starts refreshing `file` from the configured URL on a background thread.
	void StartUpdate(const std::filesystem::path& file);
	// Waits for the refresh to finish (or the timeout). Returns false on timeout.
	bool WaitForUpdate(std::chrono::milliseconds timeout);
}
