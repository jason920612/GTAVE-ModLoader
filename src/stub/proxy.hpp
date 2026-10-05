#pragma once

namespace stub::proxy
{
	// Must succeed before any forwarded export is called; safe to call from DllMain.
	bool LoadRealVersionDll();
}
