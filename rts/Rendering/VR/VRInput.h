/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#pragma once

// Zero-K VR: OpenXR controller input (actions, bindings, hand poses).
// Internal to the VR code; include after the OpenXR headers and before
// Game/Camera.h (which defines a `camera` macro).

#include <openxr/openxr.h>

struct VRHandState {
	bool gripValid = false;      // grip pose tracked this frame
	bool aimValid = false;       // aim (pointer) pose tracked this frame
	XrPosef grip = {{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}};
	XrPosef aim  = {{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}};
	float squeeze = 0.0f;        // grip button, 0..1
	float trigger = 0.0f;        // 0..1
	bool command = false;        // primary face button (A / X), or the trackpad click
	bool stickClick = false;     // thumbstick click (moves the pointer to this hand)
	bool secondary = false;      // second face button (B / Y): Ctrl on the off hand, Alt on the pointer hand
	XrVector2f thumbstick = {0.0f, 0.0f};
};

class CVRInput {
public:
	static constexpr int NUM_HANDS = 2; // 0 = left, 1 = right

	// creates the action set, suggests bindings for common controllers and
	// attaches it to the session; false if input is unavailable
	bool Init(XrInstance instance, XrSession session);
	void Destroy();

	// call once per frame after xrBeginFrame; poses are located in baseSpace
	// at the given (predicted display) time
	void Update(XrSession session, XrSpace baseSpace, XrTime time);

	// logs which controller profile the runtime picked for each hand
	void LogProfiles(XrInstance instance, XrSession session) const;

	const VRHandState& Hand(int i) const { return hands[i]; }

	// a vibration on hand i: amplitude 0..1 for the given seconds
	void Pulse(XrSession session, int hand, float amplitude, float seconds) const;

private:
	XrActionSet actionSet = XR_NULL_HANDLE;
	XrAction gripPoseAction = XR_NULL_HANDLE;
	XrAction aimPoseAction = XR_NULL_HANDLE;
	XrAction squeezeAction = XR_NULL_HANDLE;
	XrAction triggerAction = XR_NULL_HANDLE;
	XrAction thumbstickAction = XR_NULL_HANDLE;
	XrAction commandAction = XR_NULL_HANDLE;
	XrAction stickClickAction = XR_NULL_HANDLE;
	XrAction secondaryAction = XR_NULL_HANDLE;
	XrAction hapticAction = XR_NULL_HANDLE;

	XrPath handPaths[NUM_HANDS] = {XR_NULL_PATH, XR_NULL_PATH};
	XrSpace gripSpaces[NUM_HANDS] = {XR_NULL_HANDLE, XR_NULL_HANDLE};
	XrSpace aimSpaces[NUM_HANDS] = {XR_NULL_HANDLE, XR_NULL_HANDLE};

	VRHandState hands[NUM_HANDS];
	bool loggedSyncError = false;
};
