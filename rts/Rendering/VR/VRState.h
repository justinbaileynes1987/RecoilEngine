/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#pragma once

#include <algorithm>

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

	// both eyes in world space, set by CVRManager::DrawEyes each frame
	inline float3 eyePos[2];

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
}
