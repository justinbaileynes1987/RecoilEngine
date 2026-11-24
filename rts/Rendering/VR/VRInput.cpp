/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include "VRInput.h"

#include <cstring>
#include <string>
#include <vector>

#include "System/Log/ILog.h"


namespace {
	bool Ok(XrInstance instance, XrResult result, const char* what)
	{
		if (XR_SUCCEEDED(result))
			return true;

		char resultStr[XR_MAX_RESULT_STRING_SIZE] = {0};
		if (instance != XR_NULL_HANDLE)
			xrResultToString(instance, result, resultStr);

		LOG_L(L_WARNING, "[VR] %s failed: %s (%d)", what, resultStr, static_cast<int>(result));
		return false;
	}

	XrPath ToPath(XrInstance instance, const std::string& str)
	{
		XrPath path = XR_NULL_PATH;
		xrStringToPath(instance, str.c_str(), &path);
		return path;
	}
}


bool CVRInput::Init(XrInstance instance, XrSession session)
{
	XrActionSetCreateInfo setInfo = {XR_TYPE_ACTION_SET_CREATE_INFO};
	strncpy(setInfo.actionSetName, "zkvr", XR_MAX_ACTION_SET_NAME_SIZE - 1);
	strncpy(setInfo.localizedActionSetName, "Zero-K VR", XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE - 1);

	if (!Ok(instance, xrCreateActionSet(instance, &setInfo, &actionSet), "xrCreateActionSet"))
		return false;

	handPaths[0] = ToPath(instance, "/user/hand/left");
	handPaths[1] = ToPath(instance, "/user/hand/right");

	const auto MakeAction = [&](XrAction& action, XrActionType type, const char* name, const char* localizedName) {
		XrActionCreateInfo info = {XR_TYPE_ACTION_CREATE_INFO};
		info.actionType = type;
		strncpy(info.actionName, name, XR_MAX_ACTION_NAME_SIZE - 1);
		strncpy(info.localizedActionName, localizedName, XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
		info.countSubactionPaths = NUM_HANDS;
		info.subactionPaths = handPaths;
		return Ok(instance, xrCreateAction(actionSet, &info, &action), "xrCreateAction");
	};

	if (!MakeAction(gripPoseAction, XR_ACTION_TYPE_POSE_INPUT, "grip_pose", "Hand pose") ||
	    !MakeAction(aimPoseAction, XR_ACTION_TYPE_POSE_INPUT, "aim_pose", "Pointer pose") ||
	    !MakeAction(squeezeAction, XR_ACTION_TYPE_FLOAT_INPUT, "grab", "Grab the table") ||
	    !MakeAction(triggerAction, XR_ACTION_TYPE_FLOAT_INPUT, "select", "Select / command") ||
	    !MakeAction(thumbstickAction, XR_ACTION_TYPE_VECTOR2F_INPUT, "thumbstick", "Thumbstick") ||
	    !MakeAction(commandAction, XR_ACTION_TYPE_BOOLEAN_INPUT, "command", "Command (right click)") ||
	    !MakeAction(stickClickAction, XR_ACTION_TYPE_BOOLEAN_INPUT, "pointer_hand", "Point with this hand")) {
		Destroy();
		return false;
	}

	// suggested bindings; the runtime picks the profile that matches the
	// controllers in use (SteamVR can also remap them in its binding UI)
	// hands: bit 0 = left, bit 1 = right
	struct Binding { XrAction action; const char* input; int hands = 3; };
	const auto Suggest = [&](const char* profile, std::initializer_list<Binding> bindings) {
		std::vector<XrActionSuggestedBinding> suggested;
		const char* handNames[NUM_HANDS] = {"/user/hand/left", "/user/hand/right"};
		for (int h = 0; h < NUM_HANDS; ++h) {
			for (const Binding& b: bindings) {
				if ((b.hands >> h) & 1)
					suggested.push_back({b.action, ToPath(instance, std::string(handNames[h]) + b.input)});
			}
		}

		XrInteractionProfileSuggestedBinding info = {XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
		info.interactionProfile = ToPath(instance, profile);
		info.countSuggestedBindings = static_cast<uint32_t>(suggested.size());
		info.suggestedBindings = suggested.data();

		const XrResult result = xrSuggestInteractionProfileBindings(instance, &info);
		if (XR_FAILED(result))
			LOG_L(L_WARNING, "[VR] runtime rejected bindings for %s (%d)", profile, static_cast<int>(result));
	};

	Suggest("/interaction_profiles/khr/simple_controller", {
		{gripPoseAction, "/input/grip/pose"},
		{aimPoseAction, "/input/aim/pose"},
		{triggerAction, "/input/select/click"},
		{commandAction, "/input/menu/click"},
	});
	Suggest("/interaction_profiles/oculus/touch_controller", {
		{gripPoseAction, "/input/grip/pose"},
		{aimPoseAction, "/input/aim/pose"},
		{squeezeAction, "/input/squeeze/value"},
		{triggerAction, "/input/trigger/value"},
		{thumbstickAction, "/input/thumbstick"},
		{commandAction, "/input/x/click", 1},
		{commandAction, "/input/a/click", 2},
		{stickClickAction, "/input/thumbstick/click"},
	});
	Suggest("/interaction_profiles/valve/index_controller", {
		{gripPoseAction, "/input/grip/pose"},
		{aimPoseAction, "/input/aim/pose"},
		{squeezeAction, "/input/squeeze/value"},
		{triggerAction, "/input/trigger/value"},
		{thumbstickAction, "/input/thumbstick"},
		{commandAction, "/input/a/click"},
		{stickClickAction, "/input/thumbstick/click"},
	});
	Suggest("/interaction_profiles/htc/vive_controller", {
		{gripPoseAction, "/input/grip/pose"},
		{aimPoseAction, "/input/aim/pose"},
		{squeezeAction, "/input/squeeze/click"},
		{triggerAction, "/input/trigger/value"},
		{thumbstickAction, "/input/trackpad"},
		{commandAction, "/input/trackpad/click"},
	});
	Suggest("/interaction_profiles/microsoft/motion_controller", {
		{gripPoseAction, "/input/grip/pose"},
		{aimPoseAction, "/input/aim/pose"},
		{squeezeAction, "/input/squeeze/click"},
		{triggerAction, "/input/trigger/value"},
		{thumbstickAction, "/input/thumbstick"},
		{commandAction, "/input/trackpad/click"},
		{stickClickAction, "/input/thumbstick/click"},
	});

	for (int h = 0; h < NUM_HANDS; ++h) {
		XrActionSpaceCreateInfo spaceInfo = {XR_TYPE_ACTION_SPACE_CREATE_INFO};
		spaceInfo.subactionPath = handPaths[h];
		spaceInfo.poseInActionSpace.orientation.w = 1.0f;

		spaceInfo.action = gripPoseAction;
		if (!Ok(instance, xrCreateActionSpace(session, &spaceInfo, &gripSpaces[h]), "xrCreateActionSpace")) {
			Destroy();
			return false;
		}

		spaceInfo.action = aimPoseAction;
		if (!Ok(instance, xrCreateActionSpace(session, &spaceInfo, &aimSpaces[h]), "xrCreateActionSpace")) {
			Destroy();
			return false;
		}
	}

	XrSessionActionSetsAttachInfo attachInfo = {XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
	attachInfo.countActionSets = 1;
	attachInfo.actionSets = &actionSet;

	if (!Ok(instance, xrAttachSessionActionSets(session, &attachInfo), "xrAttachSessionActionSets")) {
		Destroy();
		return false;
	}

	LOG("[VR] controller input ready");
	return true;
}


void CVRInput::Destroy()
{
	for (int h = 0; h < NUM_HANDS; ++h) {
		if (gripSpaces[h] != XR_NULL_HANDLE)
			xrDestroySpace(gripSpaces[h]);
		if (aimSpaces[h] != XR_NULL_HANDLE)
			xrDestroySpace(aimSpaces[h]);

		gripSpaces[h] = XR_NULL_HANDLE;
		aimSpaces[h] = XR_NULL_HANDLE;
		hands[h] = VRHandState();
	}

	// destroys the actions as well
	if (actionSet != XR_NULL_HANDLE)
		xrDestroyActionSet(actionSet);

	actionSet = XR_NULL_HANDLE;
	gripPoseAction = XR_NULL_HANDLE;
	aimPoseAction = XR_NULL_HANDLE;
	squeezeAction = XR_NULL_HANDLE;
	triggerAction = XR_NULL_HANDLE;
	thumbstickAction = XR_NULL_HANDLE;
	commandAction = XR_NULL_HANDLE;
	stickClickAction = XR_NULL_HANDLE;
}


void CVRInput::Update(XrSession session, XrSpace baseSpace, XrTime time)
{
	if (actionSet == XR_NULL_HANDLE)
		return;

	XrActiveActionSet activeSet = {actionSet, XR_NULL_PATH};
	XrActionsSyncInfo syncInfo = {XR_TYPE_ACTIONS_SYNC_INFO};
	syncInfo.countActiveActionSets = 1;
	syncInfo.activeActionSets = &activeSet;

	const XrResult syncResult = xrSyncActions(session, &syncInfo);

	// XR_SESSION_NOT_FOCUSED (e.g. the SteamVR dashboard is open) is a success
	// code but means no input reaches us
	if (XR_FAILED(syncResult) || syncResult == XR_SESSION_NOT_FOCUSED) {
		if (XR_FAILED(syncResult) && !loggedSyncError) {
			LOG_L(L_WARNING, "[VR] xrSyncActions failed (%d)", static_cast<int>(syncResult));
			loggedSyncError = true;
		}
		for (VRHandState& hand: hands) {
			hand = VRHandState();
		}
		return;
	}

	for (int h = 0; h < NUM_HANDS; ++h) {
		VRHandState& hand = hands[h];

		XrActionStateGetInfo getInfo = {XR_TYPE_ACTION_STATE_GET_INFO};
		getInfo.subactionPath = handPaths[h];

		XrActionStateFloat floatState = {XR_TYPE_ACTION_STATE_FLOAT};
		getInfo.action = squeezeAction;
		hand.squeeze = (XR_SUCCEEDED(xrGetActionStateFloat(session, &getInfo, &floatState)) && floatState.isActive) ? floatState.currentState : 0.0f;

		floatState = {XR_TYPE_ACTION_STATE_FLOAT};
		getInfo.action = triggerAction;
		hand.trigger = (XR_SUCCEEDED(xrGetActionStateFloat(session, &getInfo, &floatState)) && floatState.isActive) ? floatState.currentState : 0.0f;

		XrActionStateVector2f vecState = {XR_TYPE_ACTION_STATE_VECTOR2F};
		getInfo.action = thumbstickAction;
		hand.thumbstick = (XR_SUCCEEDED(xrGetActionStateVector2f(session, &getInfo, &vecState)) && vecState.isActive) ? vecState.currentState : XrVector2f{0.0f, 0.0f};

		XrActionStateBoolean boolState = {XR_TYPE_ACTION_STATE_BOOLEAN};
		getInfo.action = commandAction;
		hand.command = XR_SUCCEEDED(xrGetActionStateBoolean(session, &getInfo, &boolState)) && boolState.isActive && boolState.currentState;

		boolState = {XR_TYPE_ACTION_STATE_BOOLEAN};
		getInfo.action = stickClickAction;
		hand.stickClick = XR_SUCCEEDED(xrGetActionStateBoolean(session, &getInfo, &boolState)) && boolState.isActive && boolState.currentState;

		const auto LocatePose = [&](XrAction action, XrSpace space, XrPosef& pose) {
			XrActionStatePose poseState = {XR_TYPE_ACTION_STATE_POSE};
			getInfo.action = action;
			if (XR_FAILED(xrGetActionStatePose(session, &getInfo, &poseState)) || !poseState.isActive)
				return false;

			XrSpaceLocation location = {XR_TYPE_SPACE_LOCATION};
			if (XR_FAILED(xrLocateSpace(space, baseSpace, time, &location)))
				return false;

			constexpr XrSpaceLocationFlags needed = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
			if ((location.locationFlags & needed) != needed)
				return false;

			pose = location.pose;
			return true;
		};

		hand.gripValid = LocatePose(gripPoseAction, gripSpaces[h], hand.grip);
		hand.aimValid = LocatePose(aimPoseAction, aimSpaces[h], hand.aim);
	}
}


void CVRInput::LogProfiles(XrInstance instance, XrSession session) const
{
	if (actionSet == XR_NULL_HANDLE)
		return;

	for (int h = 0; h < NUM_HANDS; ++h) {
		XrInteractionProfileState profileState = {XR_TYPE_INTERACTION_PROFILE_STATE};
		if (XR_FAILED(xrGetCurrentInteractionProfile(session, handPaths[h], &profileState)))
			continue;

		char profile[XR_MAX_PATH_LENGTH] = "none";
		uint32_t length = 0;
		if (profileState.interactionProfile != XR_NULL_PATH)
			xrPathToString(instance, profileState.interactionProfile, sizeof(profile), &length, profile);

		LOG("[VR] %s controller profile: %s", (h == 0) ? "left" : "right", profile);
	}
}
