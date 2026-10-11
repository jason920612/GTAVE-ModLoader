#pragma once
#include <cstdint>
#include <functional>
#include <optional>
#include <string>

// A local text model for mods (research/phase0.md §40): llama.cpp's server running a small model on the CPU in a
// process of its own, below normal priority, so the game (GPU-bound) keeps its frame rate. Optional: the files are
// downloaded from the settings page, and nothing runs unless the setting is on.
namespace loader::ai
{
	enum class State : int32_t
	{
		Off,          // the setting is off
		NotInstalled, // on, but the files are missing (download from the settings page)
		Downloading,
		Starting,
		Ready,
		Failed, // see Status().message
	};

	struct Status
	{
		State state = State::Off;
		uint64_t done = 0, total = 0; // download progress, bytes
		std::string message;          // last error or what is being downloaded
		int32_t queued = 0;           // requests waiting or running
	};

	// Starts the background worker (once). Any thread.
	void Start();
	Status GetStatus();
	bool Installed();
	// Downloads llama.cpp and the model (checked against pinned SHA-256 hashes), resuming a partial download.
	void Install();
	// The setting changed: starts or stops the model process.
	void SettingChanged();

	struct Request
	{
		std::string system, prompt;
		int32_t maxTokens = 120;
		float temperature = 0.9f;
	};
	// Queues a completion; `done` runs on the worker thread with the text, or nullopt (model off, failure).
	// Requests are served one at a time, in order.
	void Submit(Request request, std::function<void(std::optional<std::string>)> done);
}
