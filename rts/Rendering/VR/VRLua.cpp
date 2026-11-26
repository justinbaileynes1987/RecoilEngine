/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include "VRLua.h"

#include <cstring>
#include <utility>
#include <vector>

#include "Lua/LuaUtils.h"
#include "Rendering/VR/VRState.h"

namespace {
	constexpr int MAX_UI_PANELS = 8;

	/***
	 * Zero-K VR: shows rectangles of the desktop window as floating panels in
	 * the headset. Replaces the whole set; pass an empty table to clear it.
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
		std::vector<VRState::UIPanelRequest> panels;

		if (lua_istable(L, 1)) {
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
		}

		VRState::uiPanels = std::move(panels);
		VRState::uiPanelsVersion++;

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
	 */
	int GetVRView(lua_State* L)
	{
		lua_pushboolean(L, VRState::eyePass);
		lua_pushnumber(L, VRState::eyeElmosPerMetre);
		lua_pushboolean(L, VRState::tvShown);
		lua_pushstring(L, (VRState::pointerHand == 0) ? "left" : "right");
		return 4;
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
}

bool VRLua::PushEntries(lua_State* L)
{
	REGISTER_LUA_CFUNC(SetVRPanels);
	REGISTER_LUA_CFUNC(GetVRView);
	REGISTER_LUA_CFUNC(SetVRTVShown);
	return true;
}
