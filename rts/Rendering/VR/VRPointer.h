/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#pragma once

// Zero-K VR: turns a controller pointer into the desktop mouse, so selection,
// orders, drags (box select, line-move) and the UI all go through the normal
// engine and LuaUI mouse path. Unsynced: it only feeds local input events.
// Kept apart from VRManager.cpp because it needs sim headers that do not mix
// with windows.h and OpenXR.

#include "System/float3.h"

namespace VRPointer {
	// true while a game is running (the pointer and camera lock only apply then)
	bool InGame();

	// points the player camera straight down at the map centre, high enough
	// that the map fills the middle of the window; margin > 1 leaves that much
	// room around it (for the UI at the screen edges)
	void LockDesktopCamera(float margin);

	struct Input {
		// pointer ray in world space (elmos)
		bool rayValid = false;
		float3 from;
		float3 dir;
		float length = 0.0f;

		// the ray hits the floating desktop panel first, at this window pixel
		// (x from the left, y from the top); the panel wins over the world
		bool onPanel = false;
		int panelX = 0;
		int panelY = 0;

		bool left = false;  // left mouse button (select)
		bool right = false; // right mouse button (order)
	};

	struct Result {
		bool worldHit = false; // pos is where the pointer meets a unit or the ground
		float3 pos;
	};

	// call once per frame after LockDesktopCamera, before the mouse handler updates
	Result Update(const Input& in);

	// let go of any button the pointer is holding down
	void Release();
}
