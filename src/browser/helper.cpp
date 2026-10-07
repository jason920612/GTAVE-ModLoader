// ml_browser_helper.exe: Chromium's renderer, GPU and utility processes for the in-game browser.
#include <Windows.h>

#include "app.hpp"

int APIENTRY wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int)
{
	CefMainArgs args(instance);
	return CefExecuteProcess(args, mlbrowser::CreateApp(), nullptr);
}
