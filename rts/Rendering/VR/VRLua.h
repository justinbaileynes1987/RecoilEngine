/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#pragma once

// Zero-K VR: Lua callouts for the headset UI. Registered into Spring.* for
// unsynced Lua only (LuaUnsyncedCtrl for LuaUI, CLuaMenu for LuaMenu), and only
// in ENABLE_OPENXR builds, so a widget can test `if Spring.SetVRPanels then ... end`.

struct lua_State;

namespace VRLua {
	// LuaUI (and the other unsynced handles that load LuaUnsyncedCtrl)
	bool PushEntries(lua_State* L);
	// LuaMenu (the lobby): the same calls, with its own panel list, plus
	// Spring.SetVRMenuRmlUiHandler
	bool PushMenuEntries(lua_State* L);

	// whether LuaMenu should hold RmlUi while no game runs (VREnable is set).
	// RmlUi serves one Lua state at a time, so LuaMenu takes it in ActivateMenu
	// and gives it up at the start of every SpringApp::Reload, before a game's
	// LuaUI sets it up (see CLuaMenu::VRAcquireRmlUi).
	bool MenuWantsRmlUi();

	// calls LuaMenu's handler (Spring.SetVRMenuRmlUiHandler), if one is set:
	// ("acquire", RmlUi) after LuaMenu got RmlUi, ("release") just before it
	// is taken away; the handler must drop every RmlUi object it holds then
	void CallMenuRmlUiHandler(lua_State* L, bool acquire);
	// LuaMenu holds RmlUi right now (a handler set later is told at once)
	void SetMenuHoldsRmlUi(bool holds);
	// LuaMenu's Lua state is gone: drop the handler without touching it
	void ForgetMenuHandler();
}
