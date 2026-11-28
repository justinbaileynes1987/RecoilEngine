/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include "VRLua.h"

#include <cstring>
#include <string>
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
			else if (lua_isstring(L, -1) && strcmp(lua_tostring(L, -1), "wall") == 0)
				p.anchor = VRState::UI_ANCHOR_WALL;
			else if (lua_isstring(L, -1) && strcmp(lua_tostring(L, -1), "radial") == 0)
				p.anchor = VRState::UI_ANCHOR_RADIAL;
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
	 * @param panels table array of {name=string, anchor="bar"|"palette"|"side"|"center"|"wall"|"radial", source="desktop"|"vr", x=number, y=number, w=number, h=number}
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
	 *   "wall" panels hang on the war-room wall to the left of the TV, facing the
	 *   room, top to bottom in list order, grown by the TV's distance over a
	 *   centre panel's so they look as big as they would in front of you.
	 *   "radial" panels float just in front of the pointer hand, facing the eyes
	 *   (a radial menu, shown while LuaUI keeps it unhidden).
	 *   Any panel but a palette or radial one can be grabbed with a grip while
	 *   that hand's ray points at it, and stays where it is let go (saved).
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
	 * Zero-K VR: a short vibration on a controller.
	 *
	 * @function Spring.VRHaptic
	 * @param hand string? "pointer" (default), "off", "left" or "right"
	 * @param amplitude number? 0..1 (default 0.3)
	 * @param seconds number? (default 0.015)
	 */
	int VRHaptic(lua_State* L)
	{
		VRState::HapticRequest r;
		const std::string hand = luaL_optstring(L, 1, "pointer");
		if (hand == "left")
			r.hand = 0;
		else if (hand == "right")
			r.hand = 1;
		else if (hand == "off")
			r.hand = 1 - VRState::pointerHand;
		else
			r.hand = VRState::pointerHand;
		r.amplitude = static_cast<float>(luaL_optnumber(L, 2, 0.3));
		r.seconds = static_cast<float>(luaL_optnumber(L, 3, 0.015));
		if (VRState::sessionRunning && VRState::haptics.size() < 16)
			VRState::haptics.push_back(r);
		return 0;
	}

	/***
	 * Zero-K VR: put panels the player moved by hand back in their anchors.
	 *
	 * @function Spring.ResetVRPanels
	 * @param name string? a panel name, or "*" (default) for all
	 */
	int ResetVRPanels(lua_State* L)
	{
		VRState::pinResets.emplace_back(luaL_optstring(L, 1, "*"));
		return 0;
	}

	/***
	 * Zero-K VR: the names of the panels the player has moved by hand.
	 *
	 * @function Spring.GetVRPanelPins
	 * @return string[] names
	 */
	int GetVRPanelPins(lua_State* L)
	{
		lua_createtable(L, static_cast<int>(VRState::pinnedNames.size()), 0);
		for (size_t i = 0; i < VRState::pinnedNames.size(); ++i) {
			lua_pushstring(L, VRState::pinnedNames[i].c_str());
			lua_rawseti(L, -2, static_cast<int>(i + 1));
		}
		return 1;
	}

	/***
	 * Zero-K VR: the controllers' buttons and thumbsticks this frame. The
	 * engine already uses the triggers, A/X, grips, thumbstick clicks and B/Y
	 * (Ctrl on the off hand, Alt on the pointer hand); the thumbsticks are
	 * free in a game, for LuaUI to use.
	 *
	 * @function Spring.GetVRControllers
	 * @return nil|table {pointer = "left"|"right", left = hand, right = hand}, where hand is
	 *   {valid, trigger, grip, stickX, stickY, primary (A/X), secondary (B/Y), stickClick,
	 *   holds = "table"|"panel"|""}, and uiX, uiY (canvas px) while the pointer
	 *   is on a VR UI panel; nil while no headset session runs
	 */
	int GetVRControllers(lua_State* L)
	{
		if (!VRState::sessionRunning)
			return 0;

		lua_createtable(L, 0, 3);
		LuaPushNamedString(L, "pointer", std::string((VRState::pointerHand == 0) ? "left" : "right"));
		for (int h = 0; h < 2; ++h) {
			const VRState::HandInput& hi = VRState::handInput[h];
			lua_pushstring(L, (h == 0) ? "left" : "right");
			lua_createtable(L, 0, 8);
			LuaPushNamedBool(L, "valid", hi.valid);
			LuaPushNamedNumber(L, "trigger", hi.trigger);
			LuaPushNamedNumber(L, "grip", hi.grip);
			LuaPushNamedNumber(L, "stickX", hi.stickX);
			LuaPushNamedNumber(L, "stickY", hi.stickY);
			LuaPushNamedBool(L, "primary", hi.primary);
			LuaPushNamedBool(L, "secondary", hi.secondary);
			LuaPushNamedBool(L, "stickClick", hi.stickClick);
			LuaPushNamedString(L, "holds", std::string((VRState::handHolds[h] == 1) ? "table" : (VRState::handHolds[h] == 2) ? "panel" : ""));
			lua_rawset(L, -3);
		}
		if (VRState::uiPointerOn) {
			LuaPushNamedNumber(L, "uiX", VRState::uiPointerX);
			LuaPushNamedNumber(L, "uiY", VRState::uiPointerY);
		}
		return 1;
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
	REGISTER_LUA_CFUNC(GetVRControllers);
	REGISTER_LUA_CFUNC(VRHaptic);
	REGISTER_LUA_CFUNC(ResetVRPanels);
	REGISTER_LUA_CFUNC(GetVRPanelPins);
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
