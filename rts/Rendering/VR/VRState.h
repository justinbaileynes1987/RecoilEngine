/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#pragma once

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

	// unit icons in the eye views, as tangents of angles seen from the eye:
	// a unit whose radius looks smaller than iconSwitchTan is drawn as an icon,
	// and icons are iconHalfSizeTan * distance in half-size (times the icon's
	// own size from its definition)
	inline float iconSwitchTan = 0.0087f;   // ~0.5 degrees
	inline float iconHalfSizeTan = 0.0079f; // ~0.9 degrees across
}
