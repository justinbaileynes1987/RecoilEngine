/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#pragma once

// Zero-K VR: state that ordinary rendering code checks while the headset's
// eye views are drawn. Header-only so it costs nothing in builds without
// ENABLE_OPENXR, where it simply stays false.
namespace VRState {
	// true while CVRManager::DrawEyes updates and draws an eye. The eye can be
	// thousands of elmos from what it looks at (a miniature on a table), so
	// distance-based switches (unit icons, feature draw distance, terrain
	// texture streaming) are bypassed or left to the desktop camera.
	inline bool eyePass = false;
}
