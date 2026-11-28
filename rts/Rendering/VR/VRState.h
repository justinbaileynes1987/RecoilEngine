/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#pragma once

#include <algorithm>
#include <string>
#include <vector>

#include "System/float3.h"

class CCamera;

// Zero-K VR: state that ordinary rendering code checks while the headset's
// eye views are drawn. Header-only so it costs nothing in builds without
// ENABLE_OPENXR, where it simply stays false.
namespace VRState {
	// true while CVRManager::DrawEyes updates and draws an eye. The eye can be
	// thousands of elmos from what it looks at (a miniature on a table), so
	// distance-based switches (unit icons, feature draw distance, terrain
	// texture streaming) are bypassed or left to the desktop camera.
	inline bool eyePass = false;

	// a copy of the desktop player camera, valid while eyePass is true (the
	// player camera itself sits at the eye then)
	inline const CCamera* desktopCamera = nullptr;

	// the camera that window pixels belong to: the mouse, Spring.TraceScreenRay
	// and the other screen<->world queries always mean the desktop window, also
	// when called while an eye is drawn
	inline const CCamera* ScreenCamera(const CCamera* current) {
		return (eyePass && desktopCamera != nullptr) ? desktopCamera : current;
	}

	// What the controller pointer points at, picked along its own ray with
	// the volumes the headset shows (TraceRay::GuiTraceRay with pickFromEye).
	// While active, GUI traces along the cursor ray (from the desktop camera
	// through the cursor pixel: selection clicks, orders, tooltips,
	// Spring.TraceScreenRay at the cursor) return this pick instead of
	// re-picking from the desktop camera, where a unit can hide behind another
	// or behind a desktop-sized icon. Unsynced: it only changes which unit a
	// local click means.
	struct CursorPick {
		bool active = false;
		float3 camPos;    // desktop camera position
		float3 camDir;    // cursor ray direction (mouse->dir)
		int unitID = -1;
		int featureID = -1;
		float3 pos;       // where the pointer meets the unit, feature or ground
	};
	inline CursorPick cursorPick;

	// true while the pointer's own pick runs: unit icons are judged as the
	// eye views show them (IconSwitchDistance)
	inline bool pickFromEye = false;

	// what the pointer's last pick was decided by, for the pointer log
	// (VRPointer.cpp): ground and water distances along the ray (-1: missed)
	// and how the hit object was hit
	enum PickHow { PICK_NONE = 0, PICK_HULL, PICK_SHELL, PICK_ICON, PICK_FEATURE_HULL, PICK_FEATURE_SHELL };
	struct PickDebug {
		float groundDist = -1.0f;
		float waterDist = -1.0f;
		int how = PICK_NONE;
	};
	inline PickDebug pickDebug;

	// the nearest icon the last pick hit (unitID -1: none), for the pointer log
	struct PickDebugIcon {
		int unitID = -1;
		float dist = 0.0f;
		float3 center;        // where the icon is drawn
		float radius = 0.0f;  // its half-size from the eye distance, before the cap
		float eyeDist = 0.0f;
	};
	inline PickDebugIcon pickDebugIcon;

	// world scale of the pointer's pick (elmos per metre of table), set by
	// VRPointer; icon picks are capped at this many metres per unit of the
	// icon's own size
	inline float pickElmosPerMetre = 1.0f;
	constexpr float ICON_PICK_MAX_RADIUS_M = 0.015f;

	// both eyes in world space, set by CVRManager::DrawEyes each frame
	inline float3 eyePos[2];

	// world scale of the eye views (elmos per metre of table), set by
	// CVRManager::DrawEyes; LuaUI sizes VR-only drawings with it (Spring.GetVRView)
	inline float eyeElmosPerMetre = 1.0f;

	// the hand that drives the pointer (0 left, 1 right), mirrored by
	// CVRManager for Spring.GetVRView
	inline int pointerHand = 1;

	// each controller's buttons and thumbstick this frame (0 left, 1 right),
	// mirrored by CVRManager for Spring.GetVRControllers (LuaUI reads the
	// off-hand thumbstick to turn a building while placing it)
	struct HandInput {
		bool valid = false;
		float trigger = 0.0f;
		float grip = 0.0f;
		float stickX = 0.0f;
		float stickY = 0.0f;
		bool primary = false;    // A / X
		bool secondary = false;  // B / Y
		bool stickClick = false;
	};
	inline HandInput handInput[2];

	// the big desktop panel (TV) is shown while UI panels exist: the pointer
	// hand's thumbstick click or Spring.SetVRTVShown toggles it
	inline bool tvShown = true;

	// unit icons in the eye views, as tangents of angles seen from the eye:
	// a unit whose radius looks smaller than iconSwitchTan is drawn as an icon,
	// and icons are iconHalfSizeTan * distance in half-size (times the icon's
	// own size from its definition)
	inline float iconSwitchTan = 0.0087f;   // ~0.5 degrees
	inline float iconHalfSizeTan = 0.0079f; // ~0.9 degrees across

	// the distance that decides whether a unit is an icon: the farther eye's,
	// so both eyes agree (an icon in either eye is an icon in both) and a unit
	// near the switch never shows as a model in one eye and an icon in the other
	inline float IconSwitchDistance(const float3& pos) {
		return std::max(eyePos[0].distance(pos), eyePos[1].distance(pos));
	}

	// Floating UI panels: rectangles of the desktop window (top-left origin,
	// window pixels) that LuaUI asks to show as panels of their own in the
	// headset (Spring.SetVRPanels, see VRLua.cpp). The pointer clicks them
	// like the desktop panel. Every change bumps uiPanelsVersion.
	enum UIPanelAnchor { UI_ANCHOR_BAR = 0, UI_ANCHOR_PALETTE = 1, UI_ANCHOR_SIDE = 2, UI_ANCHOR_CENTER = 3, UI_ANCHOR_WALL = 4 };
	struct UIPanelRequest {
		std::string name;
		int anchor = UI_ANCHOR_BAR; // bar: on the table's far edge; palette: over the off hand; side: by the pointer hand; center: in front of you; wall: beside the TV
		float widthM = 0.0f;        // physical width in metres; 0 = the anchor's default metres per pixel
		bool canvas = false;        // source "vr": a rectangle of the VR UI canvas (RmlUi "vr_" contexts, see VRRml.h), not the desktop frame
		bool hidden = false;        // kept (with its swapchain) but not shown, so showing it later is instant
		int x = 0;
		int y = 0;
		int w = 0;
		int h = 0;
	};
	inline std::vector<UIPanelRequest> uiPanels;
	inline int uiPanelsVersion = 0;

	// the same, asked for by LuaMenu (the lobby). LuaMenu lives through every
	// game, so its panels are kept apart: the headset shows LuaMenu's while no
	// game runs (menuView) and LuaUI's in a game.
	inline std::vector<UIPanelRequest> menuPanels;
	inline int menuPanelsVersion = 0;

	// set by CVRManager each frame: an OpenXR session is running, and no game
	// is the active controller (the menu, the loading screen, pregame)
	inline bool sessionRunning = false;
	inline bool menuView = false;

	// the headset's view in world space (head position, axes, and the tangents
	// of the field of view both eyes together cover), set by CVRManager::DrawEyes
	// each frame a game's eyes are drawn
	struct HeadView {
		bool valid = false;
		float3 pos;
		float3 forward;
		float3 up;
		float3 right;
		float tanLeft = -1.0f;  // negative: left of forward
		float tanRight = 1.0f;
		float tanDown = -1.0f;  // negative: below forward
		float tanUp = 1.0f;
	};
	inline HeadView headView;

	// "in view" for selection (double-click on a unit, "select Visible"): the
	// desktop camera is held over the whole map in VR, so while a headset
	// shows the game it means the headset's view. -1: no headset view, use the
	// camera; 0: out of view; 1: in view (a sphere of the radius touching it)
	inline int InHeadView(const float3& p, float radius = 0.0f) {
		if (!headView.valid || !sessionRunning || menuView)
			return -1;
		const HeadView& h = headView;
		const float3 d = p - h.pos;
		const float z = d.dot(h.forward);
		if (z <= -radius)
			return 0;
		const float zz = std::max(z, 1.0f);
		const float x = d.dot(h.right);
		const float y = d.dot(h.up);
		const bool inX = (x + radius >= h.tanLeft * zz) && (x - radius <= h.tanRight * zz);
		const bool inY = (y + radius >= h.tanDown * zz) && (y - radius <= h.tanUp * zz);
		return (inX && inY) ? 1 : 0;
	}
}
