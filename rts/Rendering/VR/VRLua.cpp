/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include "VRLua.h"

#include <cstring>
#include <utility>
#include <vector>

#include "Lua/LuaMenu.h"
#include "Lua/LuaUtils.h"
#include "Rendering/VR/VRState.h"
#include "System/Config/ConfigHandler.h"
#include "System/Log/ILog.h"

namespace {
	constexpr int MAX_UI_PANELS = 8;

	// LuaMenu's RmlUi handler (a registry reference in LuaMenu's state)
	lua_State* menuHandlerL = nullptr;
	int menuHandlerRef = LUA_NOREF;
	bool menuHoldsRmlUi = false;

	// reads the panel list at stack index 1 (see Spring.SetVRPanels)
	std::vector<VRState::UIPanelRequest> ReadPanels(lua_State* L)
	{
		std::vector<VRState::UIPanelRequest> panels;

		if (!lua_istable(L, 1))
			return panels;

		for (int i = 1; static_cast<int>(panels.size()) < MAX_UI_PANELS; ++i) {
			lua_rawgeti(L, 1, i);
			if (!lua_istable(L, -1)) {
				lua_pop(L, 1);
				break;
			}

			VRState::UIPanelRequest p;

			lua_getfield(L, -1, "name");
			if (lua_isstring(L, -1))
				p.name = lua_tostring(L, -1);
			lua_pop(L, 1);

			lua_getfield(L, -1, "anchor");
			if (lua_isstring(L, -1) && strcmp(lua_tostring(L, -1), "palette") == 0)
				p.anchor = VRState::UI_ANCHOR_PALETTE;
			else if (lua_isstring(L, -1) && strcmp(lua_tostring(L, -1), "side") == 0)
				p.anchor = VRState::UI_ANCHOR_SIDE;
			else if (lua_isstring(L, -1) && strcmp(lua_tostring(L, -1), "center") == 0)
				p.anchor = VRState::UI_ANCHOR_CENTER;
			lua_pop(L, 1);

			lua_getfield(L, -1, "width");
			if (lua_isnumber(L, -1))
				p.widthM = static_cast<float>(lua_tonumber(L, -1));
			lua_pop(L, 1);

			lua_getfield(L, -1, "hidden");
			p.hidden = lua_toboolean(L, -1);
			lua_pop(L, 1);

			lua_getfield(L, -1, "source");
			if (lua_isstring(L, -1) && strcmp(lua_tostring(L, -1), "vr") == 0)
				p.canvas = true;
			lua_pop(L, 1);

			const auto GetInt = [L](const char* key) {
				lua_getfield(L, -1, key);
				const int v = lua_isnumber(L, -1) ? static_cast<int>(lua_tonumber(L, -1)) : 0;
				lua_pop(L, 1);
				return v;
			};
			p.x = GetInt("x");
			p.y = GetInt("y");
			p.w = GetInt("w");
			p.h = GetInt("h");

			lua_pop(L, 1);

			if (p.w > 0 && p.h > 0)
				panels.push_back(std::move(p));
		}

		return panels;
	}

	/***
	 * Zero-K VR: shows rectangles of the desktop window as floating panels in
	 * the headset. Replaces the whole set; pass an empty table to clear it.
	 * LuaUI and LuaMenu each have their own set: LuaMenu's shows while no game
	 * runs (the menu, loading), LuaUI's in a game.
	 *
	 * @function Spring.SetVRPanels
	 * @param panels table array of {name=string, anchor="bar"|"palette"|"side"|"center", source="desktop"|"vr", x=number, y=number, w=number, h=number}
	 *   hidden (optional): keep the panel (and its headset swapchain) but do not show it;
	 *   showing it later is then instant, and the swapchain is made while nothing else draws.
	 *   x, y: top-left corner in window pixels (y down, like Chili); w, h: size in pixels.
	 *   width (optional): physical width in metres; otherwise VRBarScale / VRPaletteScale metres per pixel.
	 *   source "vr": cut from the VR UI canvas, where RmlUi contexts named "vr_*" are
	 *   drawn (window-sized, never shown on the desktop); the pointer drives those
	 *   contexts directly. Default "desktop": cut from the desktop frame.
	 *   "bar" panels stand on the table's far edge, left to right in list order;
	 *   "palette" panels float over the off hand, facing the eyes, while that hand
	 *   is raised in front of you, bottom to top in list order.
	 *   "side" panels stand off to the pointer hand's side (swapping with it) and
	 *   lazily follow the head, bottom to top in list order.
	 *   "center" panels stand in front of you at eye height, placed where you look
	 *   when they appear and fixed there until hidden (a menu).
	 * @return boolean ok
	 */
	int SetVRPanels(lua_State* L)
	{
		VRState::uiPanels = ReadPanels(L);
		VRState::uiPanelsVersion++;
		lua_pushboolean(L, true);
		return 1;
	}

	// LuaMenu's Spring.SetVRPanels
	int SetVRMenuPanels(lua_State* L)
	{
		VRState::menuPanels = ReadPanels(L);
		VRState::menuPanelsVersion++;
		lua_pushboolean(L, true);
		return 1;
	}

	/***
	 * Zero-K VR: the headset view, for VR-only drawing in the draw call-ins.
	 *
	 * @function Spring.GetVRView
	 * @return boolean eyePass true while a headset eye view is drawn (DrawWorld and the
	 *   other world call-ins run once per eye and once for the desktop)
	 * @return number elmosPerMetre world scale of the table view (elmos per metre)
	 * @return boolean tvShown the big desktop panel (TV) is shown
	 * @return string pointerHand "left" or "right"
	 * @return boolean running an OpenXR session is running (a headset shows the game)
	 * @return boolean menuView no game is the active controller (the menu, loading)
	 */
	int GetVRView(lua_State* L)
	{
		lua_pushboolean(L, VRState::eyePass);
		lua_pushnumber(L, VRState::eyeElmosPerMetre);
		lua_pushboolean(L, VRState::tvShown);
		lua_pushstring(L, (VRState::pointerHand == 0) ? "left" : "right");
		lua_pushboolean(L, VRState::sessionRunning);
		lua_pushboolean(L, VRState::menuView);
		return 6;
	}

	/***
	 * Zero-K VR: shows or hides the big desktop panel (TV), as the pointer
	 * hand's thumbstick click does.
	 *
	 * @function Spring.SetVRTVShown
	 * @param shown boolean
	 * @return nil
	 */
	int SetVRTVShown(lua_State* L)
	{
		VRState::tvShown = lua_toboolean(L, 1);
		return 0;
	}

	/***
	 * Zero-K VR (LuaMenu only): RmlUi serves one Lua state at a time, and a
	 * game's LuaUI needs it. LuaMenu gets it while no game runs. The handler is
	 * called with ("acquire", RmlUi) once LuaMenu has RmlUi (at once, if it
	 * already has it), and with ("release") just before it is taken away for a
	 * game: drop every context, document and element then. The RmlUi global is
	 * only valid in between.
	 *
	 * @function Spring.SetVRMenuRmlUiHandler
	 * @param handler function|nil
	 * @return boolean wanted false when LuaMenu never gets RmlUi (VREnable off)
	 */
	int SetVRMenuRmlUiHandler(lua_State* L)
	{
		if (menuHandlerL != nullptr && menuHandlerRef != LUA_NOREF)
			luaL_unref(menuHandlerL, LUA_REGISTRYINDEX, menuHandlerRef);
		menuHandlerL = nullptr;
		menuHandlerRef = LUA_NOREF;

		if (lua_isfunction(L, 1)) {
			lua_pushvalue(L, 1);
			menuHandlerRef = luaL_ref(L, LUA_REGISTRYINDEX);
			menuHandlerL = L;

			if (menuHoldsRmlUi) {
				VRLua::CallMenuRmlUiHandler(L, true);
			} else if (luaMenu != nullptr) {
				// no game runs: take RmlUi now (calls the handler) rather
				// than at the next ActivateMenu (LuaMenu was just loaded)
				luaMenu->VRAcquireRmlUi();
			}
		}

		lua_pushboolean(L, VRLua::MenuWantsRmlUi());
		return 1;
	}

	void PushNamed(lua_State* L, const char* name, lua_CFunction f)
	{
		lua_pushstring(L, name);
		lua_pushcfunction(L, f);
		lua_rawset(L, -3);
	}
}


bool VRLua::PushEntries(lua_State* L)
{
	REGISTER_LUA_CFUNC(SetVRPanels);
	REGISTER_LUA_CFUNC(GetVRView);
	REGISTER_LUA_CFUNC(SetVRTVShown);
	return true;
}


bool VRLua::PushMenuEntries(lua_State* L)
{
	PushNamed(L, "SetVRPanels", SetVRMenuPanels);
	PushNamed(L, "GetVRView", GetVRView);
	PushNamed(L, "SetVRTVShown", SetVRTVShown);
	PushNamed(L, "SetVRMenuRmlUiHandler", SetVRMenuRmlUiHandler);
	return true;
}


bool VRLua::MenuWantsRmlUi()
{
	return configHandler->GetBool("VREnable");
}


void VRLua::SetMenuHoldsRmlUi(bool holds)
{
	menuHoldsRmlUi = holds;
}


void VRLua::ForgetMenuHandler()
{
	menuHandlerL = nullptr;
	menuHandlerRef = LUA_NOREF;
	menuHoldsRmlUi = false;
}


void VRLua::CallMenuRmlUiHandler(lua_State* L, bool acquire)
{
	if (L == nullptr || L != menuHandlerL || menuHandlerRef == LUA_NOREF)
		return;

	const int top = lua_gettop(L);

	lua_rawgeti(L, LUA_REGISTRYINDEX, menuHandlerRef);
	if (!lua_isfunction(L, -1)) {
		lua_settop(L, top);
		return;
	}

	lua_pushstring(L, acquire ? "acquire" : "release");
	int args = 1;
	if (acquire) {
		lua_getglobal(L, "RmlUi");
		args = 2;
	}

	if (lua_pcall(L, args, 0, 0) != 0) {
		LOG_L(L_ERROR, "[VR] LuaMenu RmlUi handler (%s) failed: %s", acquire ? "acquire" : "release", lua_isstring(L, -1) ? lua_tostring(L, -1) : "?");
	}

	lua_settop(L, top);
}
