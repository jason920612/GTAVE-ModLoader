// CefApp shared by the browser process (mlbrowser.dll inside the game) and the helper processes
// (ml_browser_helper.exe): Chromium switches, and the renderer side of the page <-> game bridge.
#include "app.hpp"

#include <include/cef_render_process_handler.h>
#include <include/wrapper/cef_message_router.h>

namespace mlbrowser
{
	namespace
	{
		// What pages see: game.call(name, ...args) -> Promise, game.on(event, fn), game.close().
		constexpr char kBridge[] = R"JS(
(() => {
  const listeners = {};
  window.game = {
    call(name, ...args) {
      return new Promise((resolve, reject) => window.cefQuery({
        request: JSON.stringify({ name, args }),
        onSuccess: r => resolve(r ? JSON.parse(r) : null),
        onFailure: (code, message) => reject(new Error(message)),
      }));
    },
    on(event, fn) { (listeners[event] ||= []).push(fn); },
    off(event, fn) { listeners[event] = (listeners[event] || []).filter(f => f !== fn); },
    close() { return this.call('browser.close'); },
    _emit(event, data) { for (const fn of listeners[event] || []) { try { fn(data); } catch (e) { console.error(e); } } },
  };
})();
)JS";

		class App : public CefApp, public CefRenderProcessHandler
		{
		public:
			void OnBeforeCommandLineProcessing(const CefString& processType, CefRefPtr<CefCommandLine> commandLine) override
			{
				if (!processType.empty())
					return;
				// Offline, quiet, software-rendered (the game owns the GPU; pages are simple).
				for (const char* name : {"disable-gpu", "disable-gpu-compositing", "disable-background-networking", "disable-component-update",
				         "disable-sync", "disable-default-apps", "disable-extensions", "no-first-run", "no-default-browser-check",
				         "disable-spell-checking", "mute-audio"})
					commandLine->AppendSwitch(name);
				commandLine->AppendSwitchWithValue("disable-features", "Translate,MediaRouter,OptimizationHints,AutofillServerCommunication");
			}

			CefRefPtr<CefRenderProcessHandler> GetRenderProcessHandler() override { return this; }

			void OnWebKitInitialized() override
			{
				m_router = CefMessageRouterRendererSide::Create(CefMessageRouterConfig());
			}
			void OnContextCreated(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, CefRefPtr<CefV8Context> context) override
			{
				m_router->OnContextCreated(browser, frame, context);
				CefRefPtr<CefV8Value> result;
				CefRefPtr<CefV8Exception> exception;
				context->Eval(kBridge, CefString(), 0, result, exception);
			}
			void OnContextReleased(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, CefRefPtr<CefV8Context> context) override
			{
				m_router->OnContextReleased(browser, frame, context);
			}
			bool OnProcessMessageReceived(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, CefProcessId source,
			    CefRefPtr<CefProcessMessage> message) override
			{
				return m_router->OnProcessMessageReceived(browser, frame, source, message);
			}

		private:
			CefRefPtr<CefMessageRouterRendererSide> m_router;
			IMPLEMENT_REFCOUNTING(App);
		};
	}

	CefRefPtr<CefApp> CreateApp()
	{
		return new App();
	}
}
