#pragma once

// UI content. Everything here runs on the render thread inside an ImGui frame.
namespace loader::ui
{
	void ApplyStyle();
	void LoadFonts();
	void DrawFrame();

	// Whether the UI currently needs keyboard/mouse (landing replacement or open menu).
	bool WantsInput();
	// Virtual-key code that toggles the in-game menu (from loader.json "menuKey").
	unsigned MenuKey();
	void ToggleMenu();
}
