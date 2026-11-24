/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#pragma once

// Zero-K VR: LuaUI callouts for the headset UI. Registered into Spring.* for
// unsynced Lua only (LuaUnsyncedCtrl), and only in ENABLE_OPENXR builds, so a
// widget can test `if Spring.SetVRPanels then ... end`.

struct lua_State;

namespace VRLua {
	bool PushEntries(lua_State* L);
}
