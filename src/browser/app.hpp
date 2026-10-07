#pragma once
#include <include/cef_app.h>

namespace mlbrowser
{
	// The CefApp for both the browser process and the helper processes.
	CefRefPtr<CefApp> CreateApp();
}
