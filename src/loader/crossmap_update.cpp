#include "crossmap_update.hpp"

#include <Windows.h>
#include <winhttp.h>

#include <fstream>
#include <future>
#include <optional>
#include <sstream>
#include <string>

#include "config.hpp"
#include "log.hpp"

namespace loader::crossmap
{
	namespace
	{
		// A real table has thousands of entries; anything far smaller is an error page or truncated.
		constexpr size_t kMinEntries = 1000;
		constexpr size_t kMaxBytes = 8 * 1024 * 1024;

		std::shared_future<void> g_update;

		struct Handle
		{
			HINTERNET h = nullptr;
			~Handle()
			{
				if (h)
					WinHttpCloseHandle(h);
			}
		};

		std::optional<std::string> Download(const std::string& url)
		{
			const std::wstring wurl(url.begin(), url.end());
			URL_COMPONENTS parts{sizeof(parts)};
			wchar_t host[256]{}, path[2048]{};
			parts.lpszHostName = host;
			parts.dwHostNameLength = static_cast<DWORD>(std::size(host));
			parts.lpszUrlPath = path;
			parts.dwUrlPathLength = static_cast<DWORD>(std::size(path));
			if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &parts) || parts.nScheme != INTERNET_SCHEME_HTTPS)
			{
				log::Error("crossmap: URL must be https: {}", url);
				return std::nullopt;
			}

			Handle session{WinHttpOpen(L"GTAVE-ModLoader", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0)};
			if (!session.h)
				return std::nullopt;
			WinHttpSetTimeouts(session.h, 5000, 5000, 10000, 10000);
			Handle connect{WinHttpConnect(session.h, host, parts.nPort, 0)};
			if (!connect.h)
				return std::nullopt;
			Handle request{WinHttpOpenRequest(connect.h, L"GET", path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)};
			if (!request.h || !WinHttpSendRequest(request.h, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
			    !WinHttpReceiveResponse(request.h, nullptr))
				return std::nullopt;

			DWORD status = 0, size = sizeof(status);
			WinHttpQueryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
			if (status != 200)
			{
				log::Warn("crossmap: server answered HTTP {}", status);
				return std::nullopt;
			}

			std::string body;
			for (;;)
			{
				DWORD available = 0;
				if (!WinHttpQueryDataAvailable(request.h, &available))
					return std::nullopt;
				if (available == 0)
					break;
				if (body.size() + available > kMaxBytes)
					return std::nullopt;
				const size_t offset = body.size();
				body.resize(offset + available);
				DWORD read = 0;
				if (!WinHttpReadData(request.h, body.data() + offset, available, &read))
					return std::nullopt;
				body.resize(offset + read);
			}
			return body;
		}

		size_t CountValidEntries(const std::string& text)
		{
			std::istringstream in(text);
			std::string line;
			size_t valid = 0;
			while (std::getline(in, line))
			{
				const auto comma = line.find(',');
				if (comma == std::string::npos)
					continue;
				try
				{
					const auto pub = std::stoull(line.substr(0, comma), nullptr, 16);
					const auto runtime = std::stoull(line.substr(comma + 1), nullptr, 16);
					valid += (pub != 0 && runtime != 0) ? 1 : 0;
				}
				catch (const std::exception&)
				{
				}
			}
			return valid;
		}

		void Update(std::filesystem::path file, std::string url)
		{
			const auto body = Download(url);
			if (!body)
			{
				log::Warn("crossmap: download failed, {}",
				    std::filesystem::exists(file) ? "keeping the existing file" : "no crossmap available yet");
				return;
			}
			const size_t entries = CountValidEntries(*body);
			if (entries < kMinEntries)
			{
				log::Warn("crossmap: downloaded file has only {} entries, ignoring it", entries);
				return;
			}

			// Write next to the target and swap, so a crash never leaves a half-written table.
			auto temp = file;
			temp += L".download";
			{
				std::ofstream out(temp, std::ios::binary | std::ios::trunc);
				out.write(body->data(), static_cast<std::streamsize>(body->size()));
				if (!out)
				{
					log::Warn("crossmap: could not write {}", temp.string());
					return;
				}
			}
			if (!MoveFileExW(temp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING))
			{
				log::Warn("crossmap: could not replace {} ({})", file.string(), GetLastError());
				return;
			}
			log::Info("crossmap: updated from {} ({} entries)", url, entries);
		}
	}

	void StartUpdate(const std::filesystem::path& file)
	{
		const auto& cfg = config::Get();
		if (!cfg.crossmapAutoUpdate)
		{
			log::Info("crossmap: auto update disabled in loader.json");
			return;
		}
		g_update = std::async(std::launch::async, Update, file, cfg.crossmapUrl).share();
	}

	bool WaitForUpdate(std::chrono::milliseconds timeout)
	{
		if (!g_update.valid())
			return true;
		return g_update.wait_for(timeout) == std::future_status::ready;
	}
}
