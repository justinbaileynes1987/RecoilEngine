/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#pragma once

// Zero-K VR: the headset's own RmlUi contexts. LuaUI creates them like any
// other context, with a name starting "vr_" (RmlUi.CreateContext("vr_ui")).
// They are kept off the desktop: never drawn on screen and never given the
// desktop mouse or keyboard. Instead the VR code renders them into an
// offscreen canvas, shows parts of it as headset panels, and feeds them the
// controller pointer. The canvas is the size of the window, so the renderer's
// buffers are shared with the desktop contexts without reallocation.
// Implemented in Rml/Backends/RmlUi_Backend.cpp (ENABLE_OPENXR builds only);
// this header has no RmlUi includes so the VR code can use it next to
// windows.h and OpenXR.

namespace VRRml {
	// whether any "vr_" context exists
	bool HaveContexts();

	// renders all "vr_" contexts into the given framebuffer (window-sized, cleared
	// by the caller), with premultiplied alpha; false if there was nothing to render
	bool Render(unsigned int framebuffer);

	// controller pointer input at a canvas pixel (x from the left, y from the top);
	// button 0 = left, 1 = right. Returns true if an element took the event.
	bool PointerMove(int x, int y);
	bool PointerButton(int button, bool down);
	// the pointer left every VR panel
	void PointerLeave();
	// thumbstick scrolling over a VR panel, in mouse-wheel steps (positive: down)
	bool PointerWheel(float delta);

	// updates every RmlUi context; the game does this itself, so call it only
	// while no game runs (LuaMenu's headset menu)
	void Update();
}
