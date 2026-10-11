#include "ai.hpp"

#include <WinSock2.h>
#include <Windows.h>
#include <bcrypt.h>
#include <winhttp.h>

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <format>
#include <fstream>
#include <mutex>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "../config.hpp"
#include "../log.hpp"
#include "../paths.hpp"

namespace loader::ai
{
	namespace
	{
		// Pinned downloads (research/phase0.md §40).
		struct File
		{
			const wchar_t* url;
			const char* sha256;
			uint64_t size;
		};
		constexpr File kLlama{L"https://github.com/ggml-org/llama.cpp/releases/download/b11552/llama-b11552-bin-win-cpu-x64.zip",
		    "c00741179811ff903b515eae28bdeef1f47af6e81c49920c4e68b62954e056d6", 19528406};
		constexpr File kModel{L"https://huggingface.co/unsloth/Qwen3.5-4B-GGUF/resolve/e87f176479d0855a907a41277aca2f8ee7a09523/Qwen3.5-4B-Q4_K_M.gguf",
		    "00fe7986ff5f6b463e62455821146049db6f9313603938a70800d1fb69ef11a4", 2740937888};

		std::filesystem::path Dir() { return paths::Get().root / L"ai"; }
		std::filesystem::path ServerExe() { return Dir() / L"llama" / L"llama-server.exe"; }
		std::filesystem::path ModelFile() { return Dir() / L"Qwen3.5-4B-Q4_K_M.gguf"; }

		struct Job
		{
			Request request;
			std::function<void(std::optional<std::string>)> done;
		};

		std::mutex g_mutex;
		std::condition_variable g_wake;
		Status g_status;
		std::deque<Job> g_jobs;
		bool g_installRequested = false, g_settingChanged = true;
		HANDLE g_process = nullptr, g_job = nullptr;
		int g_port = 0;
		std::wstring g_key; // the server's API key, new each start: only the loader can use the server

		void SetState(State s, std::string message = {})
		{
			std::lock_guard lock(g_mutex);
			g_status.state = s;
			g_status.message = std::move(message);
		}

		struct Handle
		{
			HINTERNET h = nullptr;
			~Handle()
			{
				if (h)
					WinHttpCloseHandle(h);
			}
		};

		// HTTPS GET into `path`, appending from `from` bytes (Range). Progress goes to g_status.
		bool Download(const File& file, const std::filesystem::path& path, std::string& error)
		{
			std::error_code ec;
			uint64_t from = std::filesystem::exists(path, ec) ? std::filesystem::file_size(path, ec) : 0;
			if (from > file.size)
			{
				std::filesystem::remove(path, ec);
				from = 0;
			}
			{
				std::lock_guard lock(g_mutex);
				g_status.done = from;
				g_status.total = file.size;
			}
			if (from == file.size)
				return true;
			URL_COMPONENTS parts{sizeof(parts)};
			wchar_t host[256]{}, urlPath[2048]{};
			parts.lpszHostName = host;
			parts.dwHostNameLength = 256;
			parts.lpszUrlPath = urlPath;
			parts.dwUrlPathLength = 2048;
			if (!WinHttpCrackUrl(file.url, 0, 0, &parts))
			{
				error = "bad url";
				return false;
			}
			Handle session{WinHttpOpen(L"GTAVE-ModLoader", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0)};
			Handle connect{session.h ? WinHttpConnect(session.h, host, parts.nPort, 0) : nullptr};
			Handle request{connect.h ? WinHttpOpenRequest(connect.h, L"GET", urlPath, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)
			                         : nullptr};
			const std::wstring range = from ? std::format(L"Range: bytes={}-\r\n", from) : L"";
			if (!request.h ||
			    !WinHttpSendRequest(request.h, range.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : range.c_str(), static_cast<DWORD>(-1L), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
			    !WinHttpReceiveResponse(request.h, nullptr))
			{
				error = std::format("connection failed ({})", GetLastError());
				return false;
			}
			DWORD status = 0, size = sizeof(status);
			WinHttpQueryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
			if (status == 200 && from)
				from = 0; // the server ignored the range: start over
			else if (status != 200 && status != 206)
			{
				error = std::format("HTTP {}", status);
				return false;
			}
			std::ofstream out(path, from ? std::ios::binary | std::ios::app : std::ios::binary | std::ios::trunc);
			std::vector<char> buffer(1 << 20);
			uint64_t done = from;
			for (DWORD read = 0;; )
			{
				if (!WinHttpReadData(request.h, buffer.data(), static_cast<DWORD>(buffer.size()), &read))
				{
					error = std::format("download interrupted ({})", GetLastError());
					return false;
				}
				if (!read)
					break;
				out.write(buffer.data(), read);
				done += read;
				std::lock_guard lock(g_mutex);
				g_status.done = done;
			}
			if (done != file.size)
			{
				error = std::format("incomplete download ({} of {} bytes)", done, file.size);
				return false;
			}
			return true;
		}

		std::string Sha256(const std::filesystem::path& path)
		{
			BCRYPT_ALG_HANDLE alg = nullptr;
			BCRYPT_HASH_HANDLE hash = nullptr;
			if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0)
				return {};
			std::string hex;
			if (BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0)
			{
				std::ifstream in(path, std::ios::binary);
				std::vector<char> buffer(1 << 20);
				while (in)
				{
					in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
					if (in.gcount() > 0)
						BCryptHashData(hash, reinterpret_cast<PUCHAR>(buffer.data()), static_cast<ULONG>(in.gcount()), 0);
				}
				UCHAR digest[32];
				if (BCryptFinishHash(hash, digest, sizeof(digest), 0) == 0)
					for (UCHAR b : digest)
						hex += std::format("{:02x}", b);
				BCryptDestroyHash(hash);
			}
			BCryptCloseAlgorithmProvider(alg, 0);
			return hex;
		}

		bool RunAndWait(std::wstring commandLine, DWORD& exitCode)
		{
			STARTUPINFOW si{sizeof(si)};
			PROCESS_INFORMATION pi{};
			if (!CreateProcessW(nullptr, commandLine.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
				return false;
			WaitForSingleObject(pi.hProcess, INFINITE);
			GetExitCodeProcess(pi.hProcess, &exitCode);
			CloseHandle(pi.hThread);
			CloseHandle(pi.hProcess);
			return true;
		}

		void DoInstall()
		{
			std::error_code ec;
			std::filesystem::create_directories(Dir(), ec);
			std::string error;
			const auto fetch = [&](const File& file, const std::filesystem::path& target, const char* what) {
				const auto part = std::filesystem::path(target).concat(L".part");
				SetState(State::Downloading, what);
				if (!Download(file, part, error))
					return false;
				SetState(State::Downloading, std::format("{}：檢查檔案", what));
				if (Sha256(part) != file.sha256)
				{
					std::filesystem::remove(part, ec);
					error = std::format("{} failed its SHA-256 check", what);
					return false;
				}
				std::filesystem::rename(part, target, ec);
				return !ec;
			};
			if (!std::filesystem::exists(ServerExe(), ec))
			{
				const auto zip = Dir() / L"llama.zip";
				if (!fetch(kLlama, zip, "llama.cpp"))
				{
					log::Error("ai: {}", error);
					SetState(State::Failed, error);
					return;
				}
				std::filesystem::create_directories(Dir() / L"llama", ec);
				wchar_t system[MAX_PATH];
				GetSystemDirectoryW(system, MAX_PATH);
				// tar can end with a warning code although everything was unpacked: judge by the result.
				DWORD code = 1;
				if (!RunAndWait(std::format(L"\"{}\\tar.exe\" -xf \"{}\" -C \"{}\"", system, zip.wstring(), (Dir() / L"llama").wstring()), code) ||
				    !std::filesystem::exists(ServerExe(), ec))
				{
					log::Error("ai: could not unpack llama.cpp");
					SetState(State::Failed, "could not unpack llama.cpp");
					return;
				}
				std::filesystem::remove(zip, ec);
			}
			if (!std::filesystem::exists(ModelFile(), ec) && !fetch(kModel, ModelFile(), "模型"))
			{
				log::Error("ai: {}", error);
				SetState(State::Failed, error);
				return;
			}
			log::Info("ai: installed");
			SetState(State::Off);
		}

		int FreePort()
		{
			WSADATA wsa;
			if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
				return 0;
			int port = 0;
			const SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
			sockaddr_in addr{};
			addr.sin_family = AF_INET;
			addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
			int len = sizeof(addr);
			if (s != INVALID_SOCKET && bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0 &&
			    getsockname(s, reinterpret_cast<sockaddr*>(&addr), &len) == 0)
				port = ntohs(addr.sin_port);
			if (s != INVALID_SOCKET)
				closesocket(s);
			WSACleanup();
			return port;
		}

		void StopServer()
		{
			if (g_job)
			{
				CloseHandle(g_job); // kill-on-close: ends the server
				g_job = nullptr;
			}
			if (g_process)
			{
				TerminateProcess(g_process, 0);
				CloseHandle(g_process);
				g_process = nullptr;
			}
		}

		// Local HTTP request to the server. Empty on failure.
		std::string Local(const wchar_t* verb, const wchar_t* path, const std::string& body, DWORD timeoutMs)
		{
			Handle session{WinHttpOpen(L"GTAVE-ModLoader", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0)};
			if (!session.h)
				return {};
			WinHttpSetTimeouts(session.h, 2000, 2000, static_cast<int>(timeoutMs), static_cast<int>(timeoutMs));
			Handle connect{WinHttpConnect(session.h, L"127.0.0.1", static_cast<INTERNET_PORT>(g_port), 0)};
			Handle request{connect.h ? WinHttpOpenRequest(connect.h, verb, path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0) : nullptr};
			const std::wstring headers = L"Content-Type: application/json\r\nAuthorization: Bearer " + g_key + L"\r\n";
			if (!request.h ||
			    !WinHttpSendRequest(request.h, headers.c_str(), static_cast<DWORD>(-1L), const_cast<char*>(body.data()),
			        static_cast<DWORD>(body.size()), static_cast<DWORD>(body.size()), 0) ||
			    !WinHttpReceiveResponse(request.h, nullptr))
				return {};
			DWORD status = 0, size = sizeof(status);
			WinHttpQueryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
			std::string out;
			char buffer[8192];
			for (DWORD read = 0; WinHttpReadData(request.h, buffer, sizeof(buffer), &read) && read;)
				out.append(buffer, read);
			return status == 200 ? out : std::string();
		}

		bool StartServer()
		{
			g_port = FreePort();
			if (!g_port)
				return false;
			const int threads = std::clamp(static_cast<int>(std::thread::hardware_concurrency()) / 4, 2, 4);
			UCHAR random[16];
			BCryptGenRandom(nullptr, random, sizeof(random), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
			g_key.clear();
			for (UCHAR b : random)
				g_key += std::format(L"{:02x}", b);
			std::wstring cmd = std::format(L"\"{}\" -m \"{}\" -t {} -c 4096 -np 1 -ngl 0 --host 127.0.0.1 --port {} --no-ui --api-key {}",
			    ServerExe().wstring(), ModelFile().wstring(), threads, g_port, g_key);
			SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
			HANDLE logFile = CreateFileW((Dir() / L"server.log").c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, 0, nullptr);
			STARTUPINFOW si{sizeof(si)};
			si.dwFlags = STARTF_USESTDHANDLES;
			si.hStdOutput = si.hStdError = logFile;
			PROCESS_INFORMATION pi{};
			const BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS | CREATE_SUSPENDED, nullptr,
			    (Dir() / L"llama").c_str(), &si, &pi);
			if (logFile != INVALID_HANDLE_VALUE)
				CloseHandle(logFile);
			if (!ok)
				return false;
			// The server dies with the game.
			g_job = CreateJobObjectW(nullptr, nullptr);
			JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
			limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
			SetInformationJobObject(g_job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
			AssignProcessToJobObject(g_job, pi.hProcess);
			ResumeThread(pi.hThread);
			CloseHandle(pi.hThread);
			g_process = pi.hProcess;
			log::Info("ai: model server starting (port {}, {} threads)", g_port, threads);
			return true;
		}

		bool ServerAlive() { return g_process && WaitForSingleObject(g_process, 0) == WAIT_TIMEOUT; }

		std::optional<std::string> Complete(const Request& r)
		{
			const nlohmann::json body{{"messages", {{{"role", "system"}, {"content", r.system}}, {{"role", "user"}, {"content", r.prompt}}}},
			    {"max_tokens", std::clamp(r.maxTokens, 1, 1024)}, {"temperature", r.temperature}, {"top_p", 0.95},
			    {"chat_template_kwargs", {{"enable_thinking", false}}}};
			const std::string reply = Local(L"POST", L"/v1/chat/completions", body.dump(), 180000);
			const auto j = nlohmann::json::parse(reply, nullptr, false);
			if (j.is_discarded() || !j.contains("choices") || j["choices"].empty())
				return std::nullopt;
			return j["choices"][0]["message"].value("content", "");
		}

		void Fail(std::deque<Job>& jobs)
		{
			for (auto& job : jobs)
				job.done(std::nullopt);
			jobs.clear();
		}

		void Worker()
		{
			uint64_t startedAt = 0;
			for (;;)
			{
				bool install = false, settingChanged = false;
				{
					std::unique_lock lock(g_mutex);
					g_wake.wait_for(lock, std::chrono::milliseconds(500));
					install = std::exchange(g_installRequested, false);
					settingChanged = std::exchange(g_settingChanged, false);
					g_status.queued = static_cast<int32_t>(g_jobs.size());
				}
				if (install)
				{
					StopServer();
					DoInstall();
					settingChanged = true;
				}
				const bool enabled = config::Get().aiEnabled;
				if (settingChanged || (!ServerAlive() && GetStatus().state == State::Ready))
				{
					if (!enabled)
					{
						StopServer();
						SetState(State::Off);
					}
					else if (!Installed())
					{
						StopServer();
						SetState(State::NotInstalled);
					}
					else if (!ServerAlive())
					{
						StopServer();
						if (StartServer())
						{
							startedAt = GetTickCount64();
							SetState(State::Starting);
						}
						else
							SetState(State::Failed, "could not start the model server");
					}
				}
				const State state = GetStatus().state;
				if (state == State::Starting)
				{
					if (!Local(L"GET", L"/health", "", 2000).empty())
					{
						log::Info("ai: model ready");
						SetState(State::Ready);
					}
					else if (!ServerAlive() || GetTickCount64() - startedAt > 180000)
					{
						log::Error("ai: the model server did not start (see ModLoader\\ai\\server.log)");
						StopServer();
						SetState(State::Failed, "the model server did not start (ModLoader\\ai\\server.log)");
					}
				}
				std::deque<Job> jobs;
				{
					std::lock_guard lock(g_mutex);
					if (state != State::Ready && state != State::Starting && state != State::Downloading)
						jobs.swap(g_jobs); // nothing will serve them
					else if (state == State::Ready && !g_jobs.empty())
					{
						jobs.push_back(std::move(g_jobs.front()));
						g_jobs.pop_front();
					}
				}
				if (state != State::Ready)
				{
					Fail(jobs);
					continue;
				}
				for (auto& job : jobs)
					job.done(Complete(job.request));
			}
		}
	}

	void Start()
	{
		static std::once_flag once;
		std::call_once(once, [] { std::thread(Worker).detach(); });
	}

	Status GetStatus()
	{
		std::lock_guard lock(g_mutex);
		return g_status;
	}

	bool Installed()
	{
		std::error_code ec;
		return std::filesystem::exists(ServerExe(), ec) && std::filesystem::exists(ModelFile(), ec);
	}

	void Install()
	{
		Start();
		std::lock_guard lock(g_mutex);
		g_installRequested = true;
		g_wake.notify_all();
	}

	void SettingChanged()
	{
		Start();
		std::lock_guard lock(g_mutex);
		g_settingChanged = true;
		g_wake.notify_all();
	}

	void Submit(Request request, std::function<void(std::optional<std::string>)> done)
	{
		Start();
		std::lock_guard lock(g_mutex);
		g_jobs.push_back({std::move(request), std::move(done)});
		g_status.queued = static_cast<int32_t>(g_jobs.size());
		g_wake.notify_all();
	}
}
