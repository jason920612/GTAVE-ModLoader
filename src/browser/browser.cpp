// mlbrowser.dll: the in-game web browser's browser process side, running inside the game (see mlbrowser.h).
// One windowless CEF browser renders into a BGRA frame the loader uploads to its overlay; input comes from the
// loader's window procedure. Every http(s) URL is served from the web roots, nothing from the internet.
#include <Windows.h>

#include <atomic>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <include/cef_app.h>
#include <include/cef_browser.h>
#include <include/cef_client.h>
#include <include/cef_parser.h>
#include <include/cef_scheme.h>
#include <include/cef_task.h>
#include <include/cef_version.h>
#include <include/wrapper/cef_message_router.h>
#include <include/wrapper/cef_stream_resource_handler.h>

#include "app.hpp"
#include "mlbrowser.h"

namespace
{
	MLBCallbacks g_callbacks{};
	std::vector<std::filesystem::path> g_roots;

	void Log(int level, const std::string& text)
	{
		if (g_callbacks.log)
			g_callbacks.log(level, text.c_str());
	}

	class FnTask : public CefTask
	{
	public:
		explicit FnTask(std::function<void()> fn) : m_fn(std::move(fn)) {}
		void Execute() override { m_fn(); }

	private:
		std::function<void()> m_fn;
		IMPLEMENT_REFCOUNTING(FnTask);
	};
	void OnUi(std::function<void()> fn)
	{
		CefPostTask(TID_UI, new FnTask(std::move(fn)));
	}

	// ---- the latest painted frame ------------------------------------------------------------

	std::mutex g_frameMutex;
	std::vector<uint8_t> g_frame; // BGRA
	int g_frameWidth = 0, g_frameHeight = 0;
	std::atomic<uint32_t> g_frameSerial = 0;
	std::atomic<int> g_viewWidth = 1280, g_viewHeight = 720;

	// ---- pages: files from the web roots ---------------------------------------------------------

	std::string ErrorPage(const std::string& url)
	{
		return "<!doctype html><meta charset=utf-8><title>404</title><body style=\"font-family:sans-serif;padding:40px\">"
		       "<h1>404</h1><p>" + url + "</p></body>";
	}

	class PageFactory : public CefSchemeHandlerFactory
	{
	public:
		CefRefPtr<CefResourceHandler> Create(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>, const CefString&, CefRefPtr<CefRequest> request) override
		{
			const std::string url = request->GetURL();
			CefURLParts parts;
			if (CefParseURL(url, parts))
			{
				const std::wstring host = CefString(&parts.host).ToWString();
				std::wstring path = CefString(&parts.path).ToWString();
				std::filesystem::path relative = std::filesystem::path(path).relative_path().lexically_normal();
				const bool escapes = !relative.empty() && *relative.begin() == L"..";
				for (const auto& root : g_roots)
				{
					if (escapes || host.empty())
						break;
					std::filesystem::path file = root / host / relative;
					std::error_code ec;
					if (std::filesystem::is_directory(file, ec))
						file /= L"index.html";
					if (!std::filesystem::is_regular_file(file, ec))
						continue;
					std::string ext = file.extension().string();
					if (!ext.empty())
						ext.erase(0, 1);
					std::string mime = CefGetMimeType(ext);
					if (mime.empty())
						mime = "application/octet-stream";
					if (auto stream = CefStreamReader::CreateForFile(file.wstring()))
						return new CefStreamResourceHandler(mime, stream);
				}
			}
			return new CefStreamResourceHandler(404, "Not Found", "text/html", {}, CefStreamReader::CreateForHandler(new OwnedData(ErrorPage(url))));
		}

	private:
		// A read handler that owns its bytes.
		class OwnedData : public CefReadHandler
		{
		public:
			explicit OwnedData(std::string data) : m_data(std::move(data)) {}
			size_t Read(void* ptr, size_t size, size_t n) override
			{
				const size_t bytes = std::min(size * n, m_data.size() - m_at);
				std::memcpy(ptr, m_data.data() + m_at, bytes);
				m_at += bytes;
				return size ? bytes / size : 0;
			}
			int Seek(int64_t offset, int whence) override
			{
				const int64_t base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? static_cast<int64_t>(m_at) : static_cast<int64_t>(m_data.size());
				const int64_t to = base + offset;
				if (to < 0 || to > static_cast<int64_t>(m_data.size()))
					return -1;
				m_at = static_cast<size_t>(to);
				return 0;
			}
			int64_t Tell() override { return static_cast<int64_t>(m_at); }
			int Eof() override { return m_at >= m_data.size(); }
			bool MayBlock() override { return false; }

		private:
			std::string m_data;
			size_t m_at = 0;
			IMPLEMENT_REFCOUNTING(OwnedData);
		};
		IMPLEMENT_REFCOUNTING(PageFactory);
	};

	// ---- page -> game calls ------------------------------------------------------------------------

	std::mutex g_queryMutex;
	std::map<int64_t, CefRefPtr<CefMessageRouterBrowserSide::Callback>> g_queries;
	std::atomic<int64_t> g_nextQuery = 1;

	class QueryHandler : public CefMessageRouterBrowserSide::Handler
	{
	public:
		bool OnQuery(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>, int64_t, const CefString& request, bool,
		    CefRefPtr<Callback> callback) override
		{
			const int64_t id = g_nextQuery++;
			{
				std::lock_guard lock(g_queryMutex);
				g_queries[id] = callback;
			}
			if (g_callbacks.query)
				g_callbacks.query(id, request.ToString().c_str());
			else
				callback->Failure(0, "no game");
			return true;
		}
	};
	QueryHandler g_queryHandler;

	// ---- the browser ------------------------------------------------------------------------

	CefRefPtr<CefBrowser> g_browser; // UI thread
	std::mutex g_urlMutex;
	std::string g_url;
	CefRefPtr<CefMessageRouterBrowserSide> g_router;

	class Client : public CefClient, public CefRenderHandler, public CefLifeSpanHandler, public CefRequestHandler, public CefDisplayHandler
	{
	public:
		CefRefPtr<CefRenderHandler> GetRenderHandler() override { return this; }
		CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }
		CefRefPtr<CefRequestHandler> GetRequestHandler() override { return this; }
		CefRefPtr<CefDisplayHandler> GetDisplayHandler() override { return this; }

		void GetViewRect(CefRefPtr<CefBrowser>, CefRect& rect) override
		{
			rect = CefRect(0, 0, g_viewWidth, g_viewHeight);
		}
		void OnPaint(CefRefPtr<CefBrowser>, PaintElementType type, const RectList&, const void* buffer, int width, int height) override
		{
			if (type != PET_VIEW)
				return;
			std::lock_guard lock(g_frameMutex);
			g_frame.assign(static_cast<const uint8_t*>(buffer), static_cast<const uint8_t*>(buffer) + size_t(width) * height * 4);
			g_frameWidth = width;
			g_frameHeight = height;
			++g_frameSerial;
		}

		void OnAfterCreated(CefRefPtr<CefBrowser> browser) override
		{
			g_browser = browser;
			browser->GetHost()->SetFocus(true);
		}
		void OnBeforeClose(CefRefPtr<CefBrowser> browser) override
		{
			g_router->OnBeforeClose(browser);
			g_browser = nullptr;
		}
		// Links that open a new window stay in this one.
		bool OnBeforePopup(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame>, int, const CefString& url, const CefString&,
		    CefLifeSpanHandler::WindowOpenDisposition, bool, const CefPopupFeatures&, CefWindowInfo&, CefRefPtr<CefClient>&, CefBrowserSettings&,
		    CefRefPtr<CefDictionaryValue>&, bool*) override
		{
			browser->GetMainFrame()->LoadURL(url);
			return true;
		}

		bool OnBeforeBrowse(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, CefRefPtr<CefRequest>, bool, bool) override
		{
			g_router->OnBeforeBrowse(browser, frame);
			return false;
		}
		void OnRenderProcessTerminated(CefRefPtr<CefBrowser> browser, TerminationStatus, int, const CefString&) override
		{
			g_router->OnRenderProcessTerminated(browser);
			Log(2, "browser: page process ended");
		}
		bool OnProcessMessageReceived(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, CefProcessId source,
		    CefRefPtr<CefProcessMessage> message) override
		{
			return g_router->OnProcessMessageReceived(browser, frame, source, message);
		}

		void OnAddressChange(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame> frame, const CefString& url) override
		{
			if (!frame->IsMain())
				return;
			std::lock_guard lock(g_urlMutex);
			g_url = url.ToString();
		}

		bool OnConsoleMessage(CefRefPtr<CefBrowser>, cef_log_severity_t level, const CefString& message, const CefString& source, int line) override
		{
			Log(level >= LOGSEVERITY_ERROR ? 3 : level >= LOGSEVERITY_WARNING ? 2 : 1,
			    "page: " + message.ToString() + " (" + source.ToString() + ":" + std::to_string(line) + ")");
			return true;
		}

	private:
		IMPLEMENT_REFCOUNTING(Client);
	};
	CefRefPtr<Client> g_client;

	// ---- CEF lifetime: CefInitialize and CefShutdown on one thread of our own -----------------------

	std::thread g_cefThread;
	std::mutex g_lifeMutex;
	std::condition_variable g_lifeCv;
	enum class Life
	{
		Off,
		Starting,
		Running,
		Failed,
		Stopping
	};
	Life g_life = Life::Off;

	void CefThread(MLBConfig config, std::wstring browserDir, std::wstring cacheDir, std::string locale)
	{
		CefMainArgs args(GetModuleHandleW(nullptr));
		CefSettings settings;
		settings.no_sandbox = true;
		settings.multi_threaded_message_loop = true;
		settings.windowless_rendering_enabled = true;
		CefString(&settings.browser_subprocess_path) = (std::filesystem::path(browserDir) / L"ml_browser_helper.exe").wstring();
		CefString(&settings.resources_dir_path) = browserDir;
		CefString(&settings.locales_dir_path) = (std::filesystem::path(browserDir) / L"locales").wstring();
		CefString(&settings.root_cache_path) = cacheDir;
		CefString(&settings.cache_path) = cacheDir;
		CefString(&settings.log_file) = (std::filesystem::path(cacheDir) / L"cef.log").wstring();
		CefString(&settings.locale) = locale;
		settings.log_severity = LOGSEVERITY_WARNING;
		settings.background_color = CefColorSetARGB(255, 255, 255, 255);

		const bool ok = CefInitialize(args, settings, mlbrowser::CreateApp(), nullptr);
		if (ok)
		{
			// Every http(s) request is a game page.
			CefRegisterSchemeHandlerFactory("https", "", new PageFactory());
			CefRegisterSchemeHandlerFactory("http", "", new PageFactory());
		}
		{
			std::unique_lock lock(g_lifeMutex);
			g_life = ok ? Life::Running : Life::Failed;
			g_lifeCv.notify_all();
			if (!ok)
				return;
			g_lifeCv.wait(lock, [] { return g_life == Life::Stopping; });
		}
		std::mutex doneMutex;
		std::condition_variable doneCv;
		bool closed = false;
		OnUi([&] {
			if (g_browser)
				g_browser->GetHost()->CloseBrowser(true);
			std::lock_guard lock(doneMutex);
			closed = true;
			doneCv.notify_all();
		});
		{
			std::unique_lock lock(doneMutex);
			doneCv.wait_for(lock, std::chrono::seconds(2), [&] { return closed; });
		}
		CefShutdown();
	}
}

extern "C"
{
	__declspec(dllexport) int MLB_Init(const MLBConfig* config)
	{
		std::unique_lock lock(g_lifeMutex);
		if (g_life == Life::Running)
			return 1;
		if (g_life != Life::Off)
			return 0;
		g_callbacks = config->callbacks;
		// libcef.dll is delay-loaded, and the game's DLL search path does not include ModLoader\browser: load it by path
		// first (its own imports, chrome_elf.dll, then come from its folder).
		if (!LoadLibraryExW((std::filesystem::path(config->browserDir) / L"libcef.dll").c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH))
		{
			Log(3, "browser: cannot load libcef.dll (error " + std::to_string(GetLastError()) + ")");
			return 0;
		}
		for (int i = 0; i < config->webRootCount; ++i)
			g_roots.emplace_back(config->webRoots[i]);
		g_router = CefMessageRouterBrowserSide::Create(CefMessageRouterConfig());
		g_client = new Client();
		g_life = Life::Starting;
		g_cefThread = std::thread(CefThread, *config, std::wstring(config->browserDir), std::wstring(config->cacheDir),
		    std::string(config->locale ? config->locale : "en-US"));
		g_lifeCv.wait(lock, [] { return g_life != Life::Starting; });
		if (g_life == Life::Running)
		{
			// The router's handlers are added on the UI thread.
			OnUi([] { g_router->AddHandler(&g_queryHandler, false); });
			Log(1, "browser: CEF " CEF_VERSION " started");
			return 1;
		}
		Log(3, "browser: CefInitialize failed");
		return 0;
	}

	__declspec(dllexport) void MLB_Open(const char* url, int width, int height)
	{
		g_viewWidth = width;
		g_viewHeight = height;
		const std::string target = url ? url : "";
		OnUi([target] {
			if (g_browser)
			{
				auto host = g_browser->GetHost();
				host->WasResized();
				host->WasHidden(false);
				host->SetFocus(true);
				if (!target.empty())
					g_browser->GetMainFrame()->LoadURL(target);
				return;
			}
			CefWindowInfo window;
			window.SetAsWindowless(nullptr);
			CefBrowserSettings settings;
			settings.windowless_frame_rate = 60;
			settings.background_color = CefColorSetARGB(255, 255, 255, 255);
			CefBrowserHost::CreateBrowser(window, g_client, target, settings, nullptr, nullptr);
		});
	}

	__declspec(dllexport) void MLB_Hide()
	{
		OnUi([] {
			if (g_browser)
			{
				g_browser->GetHost()->SetFocus(false);
				g_browser->GetHost()->WasHidden(true);
			}
		});
	}

	__declspec(dllexport) int MLB_Frame(uint32_t* serial, void* dst, int pitch, int width, int height)
	{
		if (g_frameSerial == *serial)
			return 0;
		std::lock_guard lock(g_frameMutex);
		const int w = std::min(width, g_frameWidth), h = std::min(height, g_frameHeight);
		for (int y = 0; y < h; ++y)
			std::memcpy(static_cast<uint8_t*>(dst) + size_t(y) * pitch, g_frame.data() + size_t(y) * g_frameWidth * 4, size_t(w) * 4);
		*serial = g_frameSerial;
		return 1;
	}

	namespace
	{
		uint32_t Modifiers()
		{
			uint32_t m = 0;
			if (GetKeyState(VK_SHIFT) & 0x8000)
				m |= EVENTFLAG_SHIFT_DOWN;
			if (GetKeyState(VK_CONTROL) & 0x8000)
				m |= EVENTFLAG_CONTROL_DOWN;
			if (GetKeyState(VK_MENU) & 0x8000)
				m |= EVENTFLAG_ALT_DOWN;
			if (GetKeyState(VK_LBUTTON) & 0x8000)
				m |= EVENTFLAG_LEFT_MOUSE_BUTTON;
			if (GetKeyState(VK_RBUTTON) & 0x8000)
				m |= EVENTFLAG_RIGHT_MOUSE_BUTTON;
			if (GetKeyState(VK_MBUTTON) & 0x8000)
				m |= EVENTFLAG_MIDDLE_MOUSE_BUTTON;
			return m;
		}
		void WithHost(std::function<void(CefRefPtr<CefBrowserHost>)> fn)
		{
			OnUi([fn = std::move(fn)] {
				if (g_browser)
					fn(g_browser->GetHost());
			});
		}
	}

	__declspec(dllexport) void MLB_MouseMove(int x, int y)
	{
		CefMouseEvent e;
		e.x = x;
		e.y = y;
		e.modifiers = Modifiers();
		WithHost([e](auto host) { host->SendMouseMoveEvent(e, false); });
	}

	__declspec(dllexport) void MLB_MouseButton(int x, int y, int button, int down)
	{
		CefMouseEvent e;
		e.x = x;
		e.y = y;
		e.modifiers = Modifiers();
		const auto type = button == 1 ? MBT_MIDDLE : button == 2 ? MBT_RIGHT : MBT_LEFT;
		WithHost([e, type, down](auto host) { host->SendMouseClickEvent(e, type, !down, 1); });
	}

	__declspec(dllexport) void MLB_Wheel(int x, int y, int dx, int dy)
	{
		CefMouseEvent e;
		e.x = x;
		e.y = y;
		e.modifiers = Modifiers();
		WithHost([e, dx, dy](auto host) { host->SendMouseWheelEvent(e, dx, dy); });
	}

	__declspec(dllexport) void MLB_Key(unsigned msg, uint64_t wparam, int64_t lparam)
	{
		CefKeyEvent e;
		e.windows_key_code = static_cast<int>(wparam);
		e.native_key_code = static_cast<int>(lparam);
		e.is_system_key = msg == WM_SYSCHAR || msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP;
		e.type = (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) ? KEYEVENT_RAWKEYDOWN : (msg == WM_KEYUP || msg == WM_SYSKEYUP) ? KEYEVENT_KEYUP : KEYEVENT_CHAR;
		e.modifiers = Modifiers();
		WithHost([e](auto host) { host->SendKeyEvent(e); });
	}

	__declspec(dllexport) int MLB_Back()
	{
		// Asked from the window thread; the answer comes from the browser's own (thread-safe) history state.
		CefRefPtr<CefBrowser> browser = g_browser;
		if (!browser || !browser->CanGoBack())
			return 0;
		browser->GoBack();
		return 1;
	}

	__declspec(dllexport) int MLB_Forward()
	{
		CefRefPtr<CefBrowser> browser = g_browser;
		if (!browser || !browser->CanGoForward())
			return 0;
		browser->GoForward();
		return 1;
	}

	__declspec(dllexport) int MLB_Url(char* buf, int size)
	{
		std::lock_guard lock(g_urlMutex);
		const int n = static_cast<int>(std::min<size_t>(g_url.size(), size > 0 ? size - 1 : 0));
		if (size > 0)
		{
			std::memcpy(buf, g_url.data(), n);
			buf[n] = 0;
		}
		return n;
	}

	__declspec(dllexport) void MLB_Respond(int64_t id, int ok, const char* result)
	{
		CefRefPtr<CefMessageRouterBrowserSide::Callback> callback;
		{
			std::lock_guard lock(g_queryMutex);
			auto it = g_queries.find(id);
			if (it == g_queries.end())
				return;
			callback = it->second;
			g_queries.erase(it);
		}
		const std::string text = result ? result : "";
		OnUi([callback, ok, text] {
			if (ok)
				callback->Success(text);
			else
				callback->Failure(1, text);
		});
	}

	__declspec(dllexport) void MLB_Emit(const char* event, const char* json)
	{
		const std::string code = "window.game && window.game._emit(" + std::string("\"") + (event ? event : "") + "\", " + (json && *json ? json : "null") + ");";
		OnUi([code] {
			if (g_browser)
				g_browser->GetMainFrame()->ExecuteJavaScript(code, "", 0);
		});
	}

	__declspec(dllexport) void MLB_Shutdown()
	{
		{
			std::lock_guard lock(g_lifeMutex);
			if (g_life != Life::Running)
				return;
			g_life = Life::Stopping;
			g_lifeCv.notify_all();
		}
		if (g_cefThread.joinable())
			g_cefThread.join();
	}
}
