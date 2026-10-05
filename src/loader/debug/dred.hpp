#pragma once

namespace loader::debug
{
	// Research aid: when ModLoader\debug_dred.txt exists, turns on D3D12 DRED (auto-breadcrumbs and page-fault
	// reporting) before the game creates its device, and logs what DRED recorded if the device is removed
	// (the game then shows ERR_GFX_STATE). If the file's first line contains "layer", the D3D12 debug layer is
	// enabled too and its errors are logged. Must be called before the game creates its D3D12 device.
	void StartDred();
}
