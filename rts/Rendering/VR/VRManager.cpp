/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include "VRManager.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "Rendering/GL/myGL.h"
#include "Rendering/GlobalRendering.h"
#include "Rendering/UniformConstants.h"
#include "Rendering/Textures/Bitmap.h"
#include "Rendering/VR/VRState.h"
#include "System/Config/ConfigHandler.h"
#include "System/Log/ILog.h"
#include "System/float3.h"
#include "System/float4.h"

#ifndef WIN32_LEAN_AND_MEAN
	#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
	#define NOMINMAX
#endif
#include <windows.h>
#include <unknwn.h> // IUnknown, used by openxr_platform.h on Win32

#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_OPENGL
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include "Rendering/VR/VRInput.h"
#include "Rendering/VR/VRPointer.h"
#include "Rendering/VR/VRRml.h"

// after the OpenXR headers: Camera.h defines a `camera` macro that would
// clash with OpenXR struct members of the same name
#include "Game/Camera.h"
#include "Game/CameraHandler.h"
#include "Game/UI/CommandColors.h"
#include "Map/Ground.h"
#include "Map/ReadMap.h"
#include "Sim/Misc/GlobalConstants.h"
#include "Rendering/Env/ISky.h"
#include "Rendering/GL/RenderBuffers.h"
#include "System/Color.h"

CONFIG(bool, VREnable).defaultValue(false).description("Zero-K VR: start an OpenXR session and show the game in a connected headset.");
CONFIG(bool, VRStereo).defaultValue(true).description("Zero-K VR: draw the battlefield in stereo around the player camera. When off, only the floating desktop panel is shown.");
CONFIG(std::string, VRView).defaultValue("table").description("Zero-K VR: 'table' shows the whole map as a miniature on a virtual table; 'camera' views the world from the desktop camera, offset by head movement.");
CONFIG(float, VRTableSize).defaultValue(1.2f).minimumValue(0.1f).description("Zero-K VR (table view): length in metres of the longest side of the map on the table.");
CONFIG(float, VRTableDistance).defaultValue(0.8f).description("Zero-K VR (table view): metres from the starting head position to the table centre.");
CONFIG(float, VRTableHeight).defaultValue(-0.5f).description("Zero-K VR (table view): height in metres of the table surface relative to the starting eye height.");
CONFIG(float, VRTableX).defaultValue(0.0f).description("Zero-K VR (table view): sideways offset in metres of the table centre (positive = right).");
CONFIG(float, VRTableYaw).defaultValue(0.0f).description("Zero-K VR (table view): rotation of the table in degrees, counter-clockwise seen from above.");
CONFIG(float, VRWorldScale).defaultValue(300.0f).minimumValue(1.0f).description("Zero-K VR (camera view): game distance units (elmos) per real-world metre of head movement.");
CONFIG(float, VRMirrorDistance).defaultValue(2.2f).minimumValue(0.3f).description("Zero-K VR: distance in metres from the starting head position to the desktop panel (UI), like a TV on the war-room wall. It stays put when the table is moved or zoomed.");
CONFIG(int, VRDebugCapture).defaultValue(0).minimumValue(0).description("Zero-K VR: if N > 0, save both eye images (vr-eye-0.png, vr-eye-1.png) to the write dir on the Nth frame that draws the eyes, and log the eye positions.");
CONFIG(float, VRIconSwitchAngle).defaultValue(0.5f).minimumValue(0.0f).description("Zero-K VR: units whose radius looks smaller than this many degrees from the eye are drawn as icons in the stereo view. 0 always draws models.");
CONFIG(float, VRIconSize).defaultValue(0.9f).minimumValue(0.05f).description("Zero-K VR: apparent width in degrees of unit icons in the stereo view (before the icon's own size multiplier).");
CONFIG(bool, VRPanel).defaultValue(true).description("Zero-K VR: show the desktop frame (with the UI) as a floating panel. Turn off to see only the stereo view.");
CONFIG(bool, VRPointer).defaultValue(true).description("Zero-K VR (table view): use the controller pointer as the mouse (pointer hand: trigger = left click, A/X = right click; other hand: trigger holds Shift). The desktop camera is held over the whole map while VR runs, so every point the pointer reaches is on screen.");
CONFIG(float, VRPointerMargin).defaultValue(1.6f).minimumValue(1.0f).description("Zero-K VR (table view, pointer on): how much room the desktop view leaves around the map, as a multiple of the map size, so pointer clicks stay clear of the UI at the screen edges.");
CONFIG(float, VRMirrorWidth).defaultValue(2.21f).minimumValue(0.1f).description("Zero-K VR: width in metres of the desktop panel (UI). The default, 2.21, is a 100-inch 16:9 TV.");
CONFIG(bool, VRUIPanels).defaultValue(true).description("Zero-K VR (table view): show the UI panels LuaUI asks for (Spring.SetVRPanels): a bar on the table edge facing you, panels at your side that follow you, and a palette on the off hand. The big desktop panel stays up as well; the pointer hand's thumbstick click hides or shows it.");
CONFIG(float, VRBarScale).defaultValue(0.0007f).minimumValue(0.0001f).description("Zero-K VR: size of the table-edge UI bar, in metres per window pixel (when LuaUI gives no width).");
CONFIG(float, VRBarMaxDistance).defaultValue(1.6f).minimumValue(0.3f).description("Zero-K VR: the table-edge UI bar always stands on the table's far edge; when that is farther than this many metres (horizontally) from your head, it grows so it reads the same size.");
CONFIG(float, VRPaletteScale).defaultValue(0.0005f).minimumValue(0.0001f).description("Zero-K VR: size of the off-hand UI palette, in metres per window pixel.");
CONFIG(float, VRSideAngle).defaultValue(40.0f).description("Zero-K VR: how far round to the pointer hand's side (it swaps with the pointer) the side UI panel (orders, build menu) sits, in degrees from where you face.");
CONFIG(float, VRSideDistance).defaultValue(0.6f).minimumValue(0.2f).description("Zero-K VR: distance in metres from your head to the side UI panel.");
CONFIG(float, VRSideHeight).defaultValue(-0.35f).description("Zero-K VR: height in metres of the side UI panel's centre relative to your eyes.");
CONFIG(float, VRSideMaxAngle).defaultValue(100.0f).description("Zero-K VR: the side UI panel swings further round to your side, up to this many degrees from where you face, when it would hide the table from your eyes.");
CONFIG(float, VRPaletteRaise).defaultValue(0.5f).description("Zero-K VR: the off-hand palette (quick orders) floats over the off hand, facing your eyes, while that hand is raised higher than this many metres below your eyes and is in front of you.");


namespace {
	constexpr uint32_t NUM_EYES = 2;

	struct ColorSwapchain {
		XrSwapchain handle = XR_NULL_HANDLE;
		std::vector<XrSwapchainImageOpenGLKHR> images;
		int32_t sizeX = 0;
		int32_t sizeY = 0;
		bool released = false; // an image was released, so it can be submitted
	};

	// rotate v by unit quaternion q
	XrVector3f Rotate(const XrQuaternionf& q, const XrVector3f& v)
	{
		// v' = v + 2 * cross(q.xyz, cross(q.xyz, v) + q.w * v)
		const float3 qv = {q.x, q.y, q.z};
		const float3 vv = {v.x, v.y, v.z};
		const float3 t = qv.cross(vv) + vv * q.w;
		const float3 r = vv + qv.cross(t) * 2.0f;
		return {r.x, r.y, r.z};
	}

	float3 ToF3(const XrVector3f& v) { return {v.x, v.y, v.z}; }
	XrVector3f ToXr(const float3& v) { return {v.x, v.y, v.z}; }

	XrQuaternionf QuatMul(const XrQuaternionf& a, const XrQuaternionf& b)
	{
		return {
			a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
			a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
			a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
			a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
		};
	}

	// rotation by angle a (radians, right-handed) about a unit axis
	XrQuaternionf QuatAxisAngle(const float3& axis, float a)
	{
		const float s = std::sin(a * 0.5f);
		return {axis.x * s, axis.y * s, axis.z * s, std::cos(a * 0.5f)};
	}

	// a flat rectangle facing its local +z (an OpenXR quad layer), metres
	struct PanelQuad {
		XrPosef pose = {{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}};
		float width = 0.0f;
		float height = 0.0f;
	};

	// where a ray (LOCAL space) meets the front of a quad: distance t along the
	// ray and (u, v) in 0..1 from the quad's bottom-left; false if it misses
	bool RayQuad(const PanelQuad& q, const float3& from, const float3& dir, float& t, float& u, float& v)
	{
		const float3 n = ToF3(Rotate(q.pose.orientation, {0.0f, 0.0f, 1.0f}));
		const float denom = dir.dot(n);
		if (denom > -1e-4f)
			return false; // parallel, or seen from behind

		const float3 c = ToF3(q.pose.position);
		t = (c - from).dot(n) / denom;
		if (t <= 0.0f)
			return false;

		const float3 d = from + dir * t - c;
		u = d.dot(ToF3(Rotate(q.pose.orientation, {1.0f, 0.0f, 0.0f}))) / q.width + 0.5f;
		v = d.dot(ToF3(Rotate(q.pose.orientation, {0.0f, 1.0f, 0.0f}))) / q.height + 0.5f;
		return (u >= 0.0f && u <= 1.0f && v >= 0.0f && v <= 1.0f);
	}

	// rotate v by angle a (radians) about the vertical axis; positive is
	// counter-clockwise seen from above (OpenXR: +x right, +y up, -z forward)
	float3 RotY(const float3& v, float a)
	{
		const float c = std::cos(a);
		const float s = std::sin(a);
		return {v.x * c + v.z * s, v.y, -v.x * s + v.z * c};
	}

	// heading of a direction on the horizontal plane, in RotY's convention
	// (RotY(YawOf(v), forward) points along v)
	float YawOf(const float3& v) { return std::atan2(-v.x, -v.z); }

	float WrapAngle(float a)
	{
		constexpr float PI = 3.14159265f;
		while (a >  PI) a -= 2.0f * PI;
		while (a < -PI) a += 2.0f * PI;
		return a;
	}

	// where the map sits in the room (OpenXR LOCAL space): its centre at sea
	// level, its rotation about the vertical, and the length of its longest side
	struct TablePose {
		float3 center = {0.0f, -0.5f, -0.8f};
		float yaw = 0.0f;
		float size = 1.2f;
	};

	// OpenXR LOCAL space (metres; +x right, +y up, -z forward) to world space
	// (elmos): world = origin + axes(xr - xrOrigin) * scale
	struct XrToWorld {
		float3 origin;
		float3 xrOrigin;
		float3 axisX; // world direction of XR +x
		float3 axisZ; // world direction of XR +z (towards the player)
		float scale = 1.0f;

		float3 Dir(const XrVector3f& v) const { return (axisX * v.x) + (UpVector * v.y) + (axisZ * v.z); }
		float3 Pos(const XrVector3f& p) const { return origin + Dir({p.x - xrOrigin.x, p.y - xrOrigin.y, p.z - xrOrigin.z}) * scale; }
	};

	// the whole map as a miniature on the table: map north points away from
	// the player (at yaw 0), sea level is the table top;
	// XR = table.center + RotY(table.yaw, map-local)
	XrToWorld TableMapping(const TablePose& tp)
	{
		const float mapSizeX = static_cast<float>(mapDims.mapx * SQUARE_SIZE);
		const float mapSizeZ = static_cast<float>(mapDims.mapy * SQUARE_SIZE);
		const float c = std::cos(tp.yaw);
		const float s = std::sin(tp.yaw);

		XrToWorld m;
		m.scale = std::max(mapSizeX, mapSizeZ) / tp.size;
		m.origin = float3(mapSizeX * 0.5f, 0.0f, mapSizeZ * 0.5f);
		m.xrOrigin = tp.center;
		m.axisX = float3(c, 0.0f, s);  // RotY(-yaw) applied to XR +x
		m.axisZ = float3(-s, 0.0f, c); // RotY(-yaw) applied to XR +z
		return m;
	}

	constexpr float POINTER_LENGTH = 5.0f; // metres

	// UI panel quad layers per frame; with the eye layer and the desktop panel
	// this stays under the 16 layers every OpenXR runtime must support
	constexpr int MAX_UI_LAYERS = 10; // panels per session (LuaMenu's and LuaUI's together)

	constexpr float MIN_TABLE_SIZE = 0.2f;   // metres
	constexpr float MAX_TABLE_SIZE = 200.0f; // metres; large enough to stand inside a base

	// half the side of the empty table in the menu (metres): the game table's
	// size, kept within what reads as a table
	float MenuTableHalf(const TablePose& tp) { return std::clamp(tp.size, 0.6f, 2.0f) * 0.5f; }
}


struct CVRManager::Impl
{
	XrInstance instance = XR_NULL_HANDLE;
	XrSystemId systemId = XR_NULL_SYSTEM_ID;
	XrSession session = XR_NULL_HANDLE;
	XrSpace localSpace = XR_NULL_HANDLE;
	int64_t colorFormat = 0;

	ColorSwapchain mirror;
	ColorSwapchain eyes[NUM_EYES];
	// the pointer rays for each eye on a transparent image, submitted after the
	// UI panels so the rays show over them (layers have no depth: a panel
	// layer covers whatever the eye layer drew where it stands)
	ColorSwapchain overlay[NUM_EYES];
	bool overlayOK = false;
	bool overlayDrawn = false;
	GLuint blitFBO = 0;

	// single-sampled copy of the back buffer: the default framebuffer may be
	// multisampled (MSAALevel), and a multisampled source can only be blitted
	// at the same size, so resolve here first and scale from this texture
	GLuint resolveFBO = 0;
	GLuint resolveTex = 0;
	int resolveSizeX = 0;
	int resolveSizeY = 0;
	bool loggedBlitError = false;

	XrSessionState state = XR_SESSION_STATE_UNKNOWN;
	bool running = false;

	// per-frame state, valid between BeginFrame and EndOfFrame
	XrFrameState frameState = {XR_TYPE_FRAME_STATE};
	XrView views[NUM_EYES] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
	bool frameBegun = false;
	bool viewsValid = false;
	bool eyesDrawn = false;

	bool stereo = true;
	bool tableView = true;
	float tableSize = 1.2f;
	float tableDistance = 0.8f;
	float tableHeight = -0.5f;
	float worldScale = 300.0f;
	float mirrorDistance = 2.2f;
	float mirrorWidth = 2.21f;

	bool showPanel = true;
	int debugCaptureFrame = 0; // VRDebugCapture
	int eyeFrames = 0;         // frames on which the eyes were drawn

	// controllers and the movable table
	CVRInput input;
	bool inputReady = false;
	TablePose table;        // current placement (table view)
	TablePose grabStart;    // placement when the current grab began
	float3 grabAnchor;      // hand position (one hand) or midpoint (two) at grab start
	float grabYaw0 = 0.0f;  // heading between the hands at grab start
	float grabDist0 = 1.0f; // distance between the hands at grab start
	int grabMask = 0;       // bit h set while hand h holds the table
	bool tableMoved = false;

	void UpdateTable();
	void SaveTable() const;

	// controller pointer as the mouse (table view, in-game)
	bool pointerEnabled = true;
	float pointerMargin = 1.6f;
	// the pointer hand clicks (trigger = left, A/X = right); the other hand's
	// trigger holds Shift. A thumbstick click or A/X on the other hand moves
	// the pointer to it.
	enum TriggerRole { TRIGGER_NONE, TRIGGER_CLICK, TRIGGER_SHIFT };
	int pointerHand = 1;           // hand that drives the pointer
	bool pointerButtons[CVRInput::NUM_HANDS][3] = {}; // [hand][0 = trigger, 1 = command, 2 = stick click], debounced
	int triggerRole[CVRInput::NUM_HANDS] = {};        // what each held trigger does, fixed at its press
	bool commandClick[CVRInput::NUM_HANDS] = {};      // A/X held as the right button
	int pointerHit = 0;            // this frame: 0 nothing, 1 world, 2 panel
	float3 pointerHitPos;          // world position of the hit (on the panel: mapped into the world)
	float3 pointerHitXR;           // the same in LOCAL space (on a panel only)
	float scrollSteps = 0.0f;      // menu: thumbstick scrolling, wheel steps not yet sent
	void UpdatePointer();

	// the menu (no game is the active controller): the war room is drawn by
	// DrawMenuRoom straight into the eye swapchains, so the TV hangs on its
	// wall and the panels stand round an empty table, as in a game
	bool menuView = false;
	bool inLobby = false;          // LuaMenu is the active controller (not loading)
	bool roomDrawn = false;        // this frame
	void UpdateMenu();
	void DrawMenuRoom();
	// the TV's last submitted placement, to show it again on a frame that has
	// no new desktop image (the menu draws a few times a second when idle)
	bool tvSubmitted = false;
	XrPosef tvPose = {{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}};
	XrExtent2Df tvSize = {0.0f, 0.0f};

	// where the desktop panel floats (LOCAL space): its centre and size in metres
	void PanelRect(bool behindTable, float3& center, float& width, float& height) const;

	// UI panels asked for by LuaUI (VRState::uiPanels): each shows a rectangle
	// of the desktop window on its own quad layer, from its own swapchain
	struct UIPanelLayer {
		std::string name;
		int anchor = VRState::UI_ANCHOR_BAR;
		bool canvas = false;             // from the VR UI canvas (RmlUi "vr_" contexts), not the desktop frame
		float metresPerPixel = 0.0f;     // 0: the anchor's default scale (VRBarScale / VRPaletteScale)
		int x = 0, y = 0, w = 0, h = 0; // window pixels, top-left origin
		ColorSwapchain sc;
		bool hidden = false;            // LuaUI keeps it but does not show it (its swapchain stays)
		bool visible = false;           // this frame
		bool drawn = false;             // submitted to the compositor last frame; only then can the pointer hit it
		PanelQuad quad;
	};
	std::vector<UIPanelLayer> uiLayers;
	// swapchains of panels LuaUI dropped, kept by name: creating swapchains can
	// fail in SteamVR (XR_ERROR_RUNTIME_FAILURE), so each is made once and reused
	std::vector<std::pair<std::string, ColorSwapchain>> uiSpare;
	int swapchainFailures = 0;    // in a row; retries are spaced out
	int swapchainRetryIn = 0;     // frames until the next try after a failure
	int uiPanelsVersionSeen = -1;
	bool uiPanelsMenuSeen = false; // uiLayers came from LuaMenu's list (VRState::menuPanels)
	// every panel name seen this session, in first-seen order: its place in
	// the headset's layer list, for good (see SubmitFrame)
	std::vector<std::string> slotNames;
	bool uiPanelsEnabled = true;  // VRUIPanels
	std::string lastPanelLogged;  // pointer log: the panel last reported under the pointer
	bool paletteShown = false;    // the off-hand palette is up (hand raised, with hysteresis)
	// VR UI (RmlUi) pointer state: buttons held on the UI, hovering it, or a
	// button held in the world (a drag), so a press keeps its target until release
	// the bar's heading (turns to the player as they walk around the table)
	// and the side panel's lazily followed head yaw and position
	float barYaw = 0.0f;
	bool barYawValid = false;
	float sideYaw = 0.0f;
	float3 sidePos;
	bool sideValid = false;
	float sideScale = 0.0005f;
	float sideAngle = 0.7f;    // radians
	float sideDistance = 0.6f;
	float sideHeight = -0.35f;
	float sideMaxAngle = 1.745f; // radians
	float sideAngleNow = 0.0f;  // the angle in use, eased towards the clearest one
	float sideLiftNow = 0.0f;   // metres raised above VRSideHeight to clear the table, eased
	// the player is using the side panel (looking or pointing at it): it holds still
	bool sideEngaged = false;
	bool sideCenterValid = false;
	float3 sideCenter;          // where it stood last frame
	bool pointerOnSide = false; // the pointer was on a side panel last frame
	bool sideAngleValid = false;
	// centre panels (the menu): placed when they appear, fixed until hidden
	bool centerValid = false;
	float centerYaw = 0.0f;
	float3 centerPos;
	bool uiButtons[2] = {false, false};
	bool uiHover = false;
	bool worldHeld = false;
	// the VR UI canvas: the "vr_" RmlUi contexts drawn offscreen, window-sized
	GLuint canvasFBO = 0;
	GLuint canvasTex = 0;
	int canvasSizeX = 0;
	int canvasSizeY = 0;
	bool RenderCanvas(int sizeX, int sizeY);
	float barScale = 0.0007f;
	float barMaxDistance = 1.6f;
	float paletteScale = 0.0005f;
	float paletteRaise = 0.5f;    // metres below the eyes the off hand must be raised above (VRPaletteRaise)
	void SyncUIPanels();
	void LayoutUIPanels();
	void EnsureUISwapchains();
	static float Scale(const UIPanelLayer& layer, float anchorScale) {
		return (layer.metresPerPixel > 0.0f) ? layer.metresPerPixel : anchorScale;
	}
	void HideUIPanels();
	// whether the big desktop panel shows: always outside the eye views (menus),
	// in-game when VRPanel is on and either no UI panels exist or it is toggled on
	bool TVVisible(bool inGameView) const {
		return !inGameView || (showPanel && (!HaveUIPanels() || VRState::tvShown));
	}
	bool HaveUIPanels() const { return uiPanelsEnabled && tableView && !uiLayers.empty(); }

	bool Check(XrResult result, const char* what) const;

	bool CreateInstance();
	bool CreateSession();
	bool ChooseColorFormat();
	bool CreateColorSwapchain(int32_t sizeX, int32_t sizeY, ColorSwapchain& sc);
	bool CreateSwapchains();
	void Destroy();

	void PollEvents();
	void LocateViews();
	// resolve a rectangle of the back buffer into resolveTex (at 0,0), then
	// scale a rectangle of resolveTex into a swapchain's next image
	bool ResolveBackBuffer(int srcX, int srcY, int srcSizeX, int srcSizeY);
	bool BlitResolvedTo(ColorSwapchain& sc, int srcX, int srcY, int srcSizeX, int srcSizeY, GLuint sourceFBO = 0); // 0: resolveFBO
	bool CopyBackBufferTo(ColorSwapchain& sc, int srcX, int srcY, int srcSizeX, int srcSizeY) {
		return ResolveBackBuffer(srcX, srcY, srcSizeX, srcSizeY) && BlitResolvedTo(sc, 0, 0, srcSizeX, srcSizeY);
	}
	// clear eye i's overlay image to transparent and run draw into it
	bool DrawOverlay(uint32_t eye, const std::function<void()>& draw);
	// clear the next image of a swapchain to a colour and run draw into it
	bool DrawInto(ColorSwapchain& sc, const float clear[4], const std::function<void()>& draw, const char* what);
	bool PrimeSwapchain(ColorSwapchain& sc);
	void SubmitFrame(bool haveFrame);
};


bool CVRManager::Impl::Check(XrResult result, const char* what) const
{
	if (XR_SUCCEEDED(result))
		return true;

	char resultStr[XR_MAX_RESULT_STRING_SIZE] = {0};

	if (instance != XR_NULL_HANDLE)
		xrResultToString(instance, result, resultStr);

	LOG_L(L_ERROR, "[VR] %s failed: %s (%d)", what, resultStr, static_cast<int>(result));
	return false;
}


bool CVRManager::Impl::CreateInstance()
{
	uint32_t extCount = 0;
	if (!Check(xrEnumerateInstanceExtensionProperties(nullptr, 0, &extCount, nullptr), "xrEnumerateInstanceExtensionProperties")) {
		LOG_L(L_ERROR, "[VR] no OpenXR runtime found; is SteamVR (or another runtime) installed and set as the active OpenXR runtime?");
		return false;
	}

	std::vector<XrExtensionProperties> exts(extCount, {XR_TYPE_EXTENSION_PROPERTIES});
	if (!Check(xrEnumerateInstanceExtensionProperties(nullptr, extCount, &extCount, exts.data()), "xrEnumerateInstanceExtensionProperties"))
		return false;

	bool haveOpenGL = false;
	for (const XrExtensionProperties& ext: exts) {
		haveOpenGL |= (strcmp(ext.extensionName, XR_KHR_OPENGL_ENABLE_EXTENSION_NAME) == 0);
	}

	if (!haveOpenGL) {
		LOG_L(L_ERROR, "[VR] the active OpenXR runtime does not support OpenGL (%s)", XR_KHR_OPENGL_ENABLE_EXTENSION_NAME);
		return false;
	}

	const char* enabledExts[] = {XR_KHR_OPENGL_ENABLE_EXTENSION_NAME};

	XrInstanceCreateInfo createInfo = {XR_TYPE_INSTANCE_CREATE_INFO};
	strncpy(createInfo.applicationInfo.applicationName, "Zero-K VR", XR_MAX_APPLICATION_NAME_SIZE - 1);
	strncpy(createInfo.applicationInfo.engineName, "Recoil", XR_MAX_ENGINE_NAME_SIZE - 1);
	createInfo.applicationInfo.applicationVersion = 1;
	createInfo.applicationInfo.engineVersion = 1;
	// request 1.0 for the widest runtime compatibility
	createInfo.applicationInfo.apiVersion = XR_API_VERSION_1_0;
	createInfo.enabledExtensionCount = 1;
	createInfo.enabledExtensionNames = enabledExts;

	if (!Check(xrCreateInstance(&createInfo, &instance), "xrCreateInstance"))
		return false;

	XrInstanceProperties instanceProps = {XR_TYPE_INSTANCE_PROPERTIES};
	if (Check(xrGetInstanceProperties(instance, &instanceProps), "xrGetInstanceProperties")) {
		LOG("[VR] OpenXR runtime: %s %u.%u.%u", instanceProps.runtimeName,
			XR_VERSION_MAJOR(instanceProps.runtimeVersion),
			XR_VERSION_MINOR(instanceProps.runtimeVersion),
			XR_VERSION_PATCH(instanceProps.runtimeVersion));
	}

	XrSystemGetInfo systemInfo = {XR_TYPE_SYSTEM_GET_INFO};
	systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;

	const XrResult systemResult = xrGetSystem(instance, &systemInfo, &systemId);
	if (systemResult == XR_ERROR_FORM_FACTOR_UNAVAILABLE) {
		LOG_L(L_ERROR, "[VR] no headset available; is it connected and awake?");
		return false;
	}
	if (!Check(systemResult, "xrGetSystem"))
		return false;

	XrSystemProperties systemProps = {XR_TYPE_SYSTEM_PROPERTIES};
	if (Check(xrGetSystemProperties(instance, systemId, &systemProps), "xrGetSystemProperties"))
		LOG("[VR] headset: %s", systemProps.systemName);

	return true;
}


bool CVRManager::Impl::CreateSession()
{
	PFN_xrGetOpenGLGraphicsRequirementsKHR getGLRequirements = nullptr;
	if (!Check(xrGetInstanceProcAddr(instance, "xrGetOpenGLGraphicsRequirementsKHR", reinterpret_cast<PFN_xrVoidFunction*>(&getGLRequirements)), "xrGetInstanceProcAddr"))
		return false;

	// required by the spec before xrCreateSession
	XrGraphicsRequirementsOpenGLKHR glRequirements = {XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_KHR};
	if (!Check(getGLRequirements(instance, systemId, &glRequirements), "xrGetOpenGLGraphicsRequirementsKHR"))
		return false;

	GLint glMajor = 0;
	GLint glMinor = 0;
	glGetIntegerv(GL_MAJOR_VERSION, &glMajor);
	glGetIntegerv(GL_MINOR_VERSION, &glMinor);

	if (XR_MAKE_VERSION(glMajor, glMinor, 0) < glRequirements.minApiVersionSupported) {
		LOG_L(L_ERROR, "[VR] OpenGL %d.%d is older than the runtime minimum %u.%u", glMajor, glMinor,
			XR_VERSION_MAJOR(glRequirements.minApiVersionSupported),
			XR_VERSION_MINOR(glRequirements.minApiVersionSupported));
		return false;
	}

	XrGraphicsBindingOpenGLWin32KHR graphicsBinding = {XR_TYPE_GRAPHICS_BINDING_OPENGL_WIN32_KHR};
	graphicsBinding.hDC = wglGetCurrentDC();
	graphicsBinding.hGLRC = wglGetCurrentContext();

	if (graphicsBinding.hDC == nullptr || graphicsBinding.hGLRC == nullptr) {
		LOG_L(L_ERROR, "[VR] no current OpenGL context");
		return false;
	}

	XrSessionCreateInfo sessionInfo = {XR_TYPE_SESSION_CREATE_INFO};
	sessionInfo.next = &graphicsBinding;
	sessionInfo.systemId = systemId;

	if (!Check(xrCreateSession(instance, &sessionInfo, &session), "xrCreateSession"))
		return false;

	XrReferenceSpaceCreateInfo spaceInfo = {XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
	spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
	spaceInfo.poseInReferenceSpace.orientation.w = 1.0f;

	if (!Check(xrCreateReferenceSpace(session, &spaceInfo, &localSpace), "xrCreateReferenceSpace"))
		return false;

	// controllers are optional: without them the view still works
	inputReady = input.Init(instance, session);
	if (!inputReady)
		LOG_L(L_WARNING, "[VR] controller input unavailable; continuing without it");

	return true;
}


bool CVRManager::Impl::ChooseColorFormat()
{
	uint32_t formatCount = 0;
	if (!Check(xrEnumerateSwapchainFormats(session, 0, &formatCount, nullptr), "xrEnumerateSwapchainFormats"))
		return false;

	std::vector<int64_t> formats(formatCount);
	if (!Check(xrEnumerateSwapchainFormats(session, formatCount, &formatCount, formats.data()), "xrEnumerateSwapchainFormats"))
		return false;

	if (formats.empty()) {
		LOG_L(L_ERROR, "[VR] runtime offers no swapchain formats");
		return false;
	}

	// the back buffer already holds sRGB-encoded bytes, so an sRGB swapchain
	// (filled by a raw copy) makes the runtime display them correctly
	colorFormat = formats[0];
	for (const int64_t preferred: {static_cast<int64_t>(GL_SRGB8_ALPHA8), static_cast<int64_t>(GL_RGBA8)}) {
		if (std::find(formats.begin(), formats.end(), preferred) != formats.end()) {
			colorFormat = preferred;
			break;
		}
	}

	return true;
}


bool CVRManager::Impl::CreateColorSwapchain(int32_t sizeX, int32_t sizeY, ColorSwapchain& sc)
{
	XrSwapchainCreateInfo swapchainInfo = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
	swapchainInfo.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
	swapchainInfo.format = colorFormat;
	swapchainInfo.sampleCount = 1;
	swapchainInfo.width = static_cast<uint32_t>(sizeX);
	swapchainInfo.height = static_cast<uint32_t>(sizeY);
	swapchainInfo.faceCount = 1;
	swapchainInfo.arraySize = 1;
	swapchainInfo.mipCount = 1;

	if (!Check(xrCreateSwapchain(session, &swapchainInfo, &sc.handle), "xrCreateSwapchain")) {
		sc.handle = XR_NULL_HANDLE;
		return false;
	}

	uint32_t imageCount = 0;
	if (!Check(xrEnumerateSwapchainImages(sc.handle, 0, &imageCount, nullptr), "xrEnumerateSwapchainImages"))
		return false;

	sc.images.assign(imageCount, {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR});
	if (!Check(xrEnumerateSwapchainImages(sc.handle, imageCount, &imageCount, reinterpret_cast<XrSwapchainImageBaseHeader*>(sc.images.data())), "xrEnumerateSwapchainImages"))
		return false;

	sc.sizeX = sizeX;
	sc.sizeY = sizeY;
	return true;
}


bool CVRManager::Impl::CreateSwapchains()
{
	if (!ChooseColorFormat())
		return false;

	if (!CreateColorSwapchain(globalRendering->winSizeX, globalRendering->winSizeY, mirror))
		return false;

	LOG("[VR] panel swapchain %dx%d, format 0x%x", mirror.sizeX, mirror.sizeY, static_cast<unsigned>(colorFormat));

	if (stereo) {
		uint32_t viewCount = 0;
		if (!Check(xrEnumerateViewConfigurationViews(instance, systemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &viewCount, nullptr), "xrEnumerateViewConfigurationViews"))
			return false;

		std::vector<XrViewConfigurationView> configViews(viewCount, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
		if (!Check(xrEnumerateViewConfigurationViews(instance, systemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, viewCount, &viewCount, configViews.data()), "xrEnumerateViewConfigurationViews"))
			return false;

		if (viewCount != NUM_EYES) {
			LOG_L(L_ERROR, "[VR] expected %u stereo views, runtime reports %u", NUM_EYES, viewCount);
			return false;
		}

		for (uint32_t i = 0; i < NUM_EYES; ++i) {
			const int32_t sx = static_cast<int32_t>(configViews[i].recommendedImageRectWidth);
			const int32_t sy = static_cast<int32_t>(configViews[i].recommendedImageRectHeight);

			if (!CreateColorSwapchain(sx, sy, eyes[i]))
				return false;
		}

		LOG("[VR] eye swapchains %dx%d (world is drawn at the desktop view size %dx%d, then scaled)",
			eyes[0].sizeX, eyes[0].sizeY, globalRendering->viewSizeX, globalRendering->viewSizeY);

		// optional: without it the rays stay under the panels
		overlayOK = CreateColorSwapchain(eyes[0].sizeX, eyes[0].sizeY, overlay[0]) && CreateColorSwapchain(eyes[1].sizeX, eyes[1].sizeY, overlay[1]);
		if (!overlayOK)
			LOG_L(L_WARNING, "[VR] no pointer overlay swapchains; the pointer rays are drawn under the UI panels");
	}

	glGenFramebuffers(1, &blitFBO);
	return true;
}


void CVRManager::Impl::Destroy()
{
	if (blitFBO != 0) {
		glDeleteFramebuffers(1, &blitFBO);
		blitFBO = 0;
	}
	if (resolveFBO != 0) {
		glDeleteFramebuffers(1, &resolveFBO);
		resolveFBO = 0;
	}
	if (resolveTex != 0) {
		glDeleteTextures(1, &resolveTex);
		resolveTex = 0;
	}
	resolveSizeX = 0;
	resolveSizeY = 0;
	if (canvasFBO != 0) {
		glDeleteFramebuffers(1, &canvasFBO);
		canvasFBO = 0;
	}
	if (canvasTex != 0) {
		glDeleteTextures(1, &canvasTex);
		canvasTex = 0;
	}
	canvasSizeX = 0;
	canvasSizeY = 0;

	for (ColorSwapchain* sc: {&mirror, &eyes[0], &eyes[1], &overlay[0], &overlay[1]}) {
		if (sc->handle != XR_NULL_HANDLE)
			xrDestroySwapchain(sc->handle);

		*sc = ColorSwapchain();
	}
	for (UIPanelLayer& layer: uiLayers) {
		if (layer.sc.handle != XR_NULL_HANDLE)
			xrDestroySwapchain(layer.sc.handle);
	}
	uiLayers.clear();
	for (auto& spare: uiSpare) {
		if (spare.second.handle != XR_NULL_HANDLE)
			xrDestroySwapchain(spare.second.handle);
	}
	uiSpare.clear();
	swapchainFailures = 0;
	swapchainRetryIn = 0;
	uiPanelsVersionSeen = -1;
	slotNames.clear();

	input.Destroy();
	inputReady = false;
	grabMask = 0;

	if (localSpace != XR_NULL_HANDLE) {
		xrDestroySpace(localSpace);
		localSpace = XR_NULL_HANDLE;
	}
	if (session != XR_NULL_HANDLE) {
		xrDestroySession(session);
		session = XR_NULL_HANDLE;
	}
	if (instance != XR_NULL_HANDLE) {
		xrDestroyInstance(instance);
		instance = XR_NULL_HANDLE;
	}

	systemId = XR_NULL_SYSTEM_ID;
	state = XR_SESSION_STATE_UNKNOWN;
	running = false;
	frameBegun = false;
	viewsValid = false;
	eyesDrawn = false;
}


void CVRManager::Impl::PollEvents()
{
	bool lost = false;

	XrEventDataBuffer event = {XR_TYPE_EVENT_DATA_BUFFER};
	while (xrPollEvent(instance, &event) == XR_SUCCESS) {
		switch (event.type) {
			case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
				const auto* stateEvent = reinterpret_cast<const XrEventDataSessionStateChanged*>(&event);
				state = stateEvent->state;
				LOG("[VR] session state %d", static_cast<int>(state));

				switch (state) {
					case XR_SESSION_STATE_READY: {
						XrSessionBeginInfo beginInfo = {XR_TYPE_SESSION_BEGIN_INFO};
						beginInfo.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
						running = Check(xrBeginSession(session, &beginInfo), "xrBeginSession");
					} break;
					case XR_SESSION_STATE_STOPPING: {
						Check(xrEndSession(session), "xrEndSession");
						running = false;
					} break;
					case XR_SESSION_STATE_EXITING:
					case XR_SESSION_STATE_LOSS_PENDING: {
						lost = true;
					} break;
					default: {
					} break;
				}
			} break;
			case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING: {
				lost = true;
			} break;
			case XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED: {
				input.LogProfiles(instance, session);
			} break;
			default: {
			} break;
		}

		event = {XR_TYPE_EVENT_DATA_BUFFER};
	}

	if (lost) {
		LOG_L(L_WARNING, "[VR] session ended by the runtime; VR is off until the game restarts");
		Destroy();
	}
}


void CVRManager::Impl::LocateViews()
{
	viewsValid = false;

	if (!stereo || eyes[0].handle == XR_NULL_HANDLE)
		return;

	XrViewLocateInfo locateInfo = {XR_TYPE_VIEW_LOCATE_INFO};
	locateInfo.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
	locateInfo.displayTime = frameState.predictedDisplayTime;
	locateInfo.space = localSpace;

	XrViewState viewState = {XR_TYPE_VIEW_STATE};
	uint32_t viewCount = 0;

	for (XrView& view: views) {
		view = {XR_TYPE_VIEW};
	}

	if (!Check(xrLocateViews(session, &locateInfo, &viewState, NUM_EYES, &viewCount, views), "xrLocateViews"))
		return;

	viewsValid = (viewCount == NUM_EYES) && ((viewState.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) != 0);
}


namespace {
	// saves and restores the GL state a framebuffer blit touches
	struct BlitStateGuard {
		GLint prevReadFBO = 0;
		GLint prevDrawFBO = 0;
		GLboolean prevScissor = GL_FALSE;
		GLboolean prevSRGB = GL_FALSE;

		BlitStateGuard() {
			glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prevReadFBO);
			glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prevDrawFBO);
			prevScissor = glIsEnabled(GL_SCISSOR_TEST);
			prevSRGB = glIsEnabled(GL_FRAMEBUFFER_SRGB);
			glDisable(GL_SCISSOR_TEST);
			glDisable(GL_FRAMEBUFFER_SRGB);
		}
		~BlitStateGuard() {
			glBindFramebuffer(GL_READ_FRAMEBUFFER, prevReadFBO);
			glBindFramebuffer(GL_DRAW_FRAMEBUFFER, prevDrawFBO);
			if (prevScissor)
				glEnable(GL_SCISSOR_TEST);
			if (prevSRGB)
				glEnable(GL_FRAMEBUFFER_SRGB);
		}
	};
}

bool CVRManager::Impl::ResolveBackBuffer(int srcX, int srcY, int srcSizeX, int srcSizeY)
{
	if (srcSizeX <= 0 || srcSizeY <= 0)
		return false;

	BlitStateGuard guard;

	// drop errors raised before this point so the check below is about the copy
	while (glGetError() != GL_NO_ERROR) {}

	// resolve (same size, so legal for a multisampled back buffer)
	if (resolveSizeX != srcSizeX || resolveSizeY != srcSizeY) {
		if (resolveTex == 0)
			glGenTextures(1, &resolveTex);
		if (resolveFBO == 0)
			glGenFramebuffers(1, &resolveFBO);

		GLint prevTex = 0;
		glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex);
		glBindTexture(GL_TEXTURE_2D, resolveTex);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, srcSizeX, srcSizeY, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glBindTexture(GL_TEXTURE_2D, prevTex);

		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, resolveFBO);
		glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, resolveTex, 0);

		resolveSizeX = srcSizeX;
		resolveSizeY = srcSizeY;
	}

	glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, resolveFBO);
	glBlitFramebuffer(
		srcX, srcY, srcX + srcSizeX, srcY + srcSizeY,
		0, 0, srcSizeX, srcSizeY,
		GL_COLOR_BUFFER_BIT, GL_NEAREST
	);

	const GLenum glErr = glGetError();
	if (glErr != GL_NO_ERROR && !loggedBlitError) {
		LOG_L(L_ERROR, "[VR] resolving the back buffer failed: GL error 0x%x (%dx%d)", glErr, srcSizeX, srcSizeY);
		loggedBlitError = true;
	}

	return true;
}


// draws the VR UI (RmlUi "vr_" contexts) into the window-sized canvas, cleared
// to transparent; panels are cut out of it like the desktop ones
bool CVRManager::Impl::RenderCanvas(int sizeX, int sizeY)
{
	if (sizeX <= 0 || sizeY <= 0 || !VRRml::HaveContexts())
		return false;

	BlitStateGuard guard;

	if (canvasSizeX != sizeX || canvasSizeY != sizeY) {
		if (canvasTex == 0)
			glGenTextures(1, &canvasTex);
		if (canvasFBO == 0)
			glGenFramebuffers(1, &canvasFBO);

		GLint prevTex = 0;
		glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex);
		glBindTexture(GL_TEXTURE_2D, canvasTex);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, sizeX, sizeY, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glBindTexture(GL_TEXTURE_2D, prevTex);

		glBindFramebuffer(GL_FRAMEBUFFER, canvasFBO);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, canvasTex, 0);

		canvasSizeX = sizeX;
		canvasSizeY = sizeY;
	}

	GLfloat prevClear[4];
	GLboolean prevMask[4];
	glGetFloatv(GL_COLOR_CLEAR_VALUE, prevClear);
	glGetBooleanv(GL_COLOR_WRITEMASK, prevMask);

	glBindFramebuffer(GL_FRAMEBUFFER, canvasFBO);
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
	glClear(GL_COLOR_BUFFER_BIT);

	glClearColor(prevClear[0], prevClear[1], prevClear[2], prevClear[3]);
	glColorMask(prevMask[0], prevMask[1], prevMask[2], prevMask[3]);

	return VRRml::Render(canvasFBO);
}


bool CVRManager::Impl::BlitResolvedTo(ColorSwapchain& sc, int srcX, int srcY, int srcSizeX, int srcSizeY, GLuint sourceFBO)
{
	if (sourceFBO == 0)
		sourceFBO = resolveFBO;
	if (sc.handle == XR_NULL_HANDLE || sourceFBO == 0)
		return false;

	uint32_t imageIndex = 0;
	XrSwapchainImageAcquireInfo acquireInfo = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
	if (!Check(xrAcquireSwapchainImage(sc.handle, &acquireInfo, &imageIndex), "xrAcquireSwapchainImage"))
		return false;

	XrSwapchainImageWaitInfo waitInfo = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
	waitInfo.timeout = XR_INFINITE_DURATION;
	if (!Check(xrWaitSwapchainImage(sc.handle, &waitInfo), "xrWaitSwapchainImage"))
		return false;

	{
		BlitStateGuard guard;

		// drop errors raised before this point so the check below is about the copy
		while (glGetError() != GL_NO_ERROR) {}

		glBindFramebuffer(GL_READ_FRAMEBUFFER, sourceFBO);
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, blitFBO);
		glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, sc.images[imageIndex].image, 0);
		glBlitFramebuffer(
			srcX, srcY, srcX + srcSizeX, srcY + srcSizeY,
			0, 0, sc.sizeX, sc.sizeY,
			GL_COLOR_BUFFER_BIT, GL_LINEAR
		);

		const GLenum glErr = glGetError();
		if (glErr != GL_NO_ERROR && !loggedBlitError) {
			LOG_L(L_ERROR, "[VR] copy into a swapchain image failed: GL error 0x%x (source %dx%d, target %dx%d)", glErr, srcSizeX, srcSizeY, sc.sizeX, sc.sizeY);
			loggedBlitError = true;
		}

		glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
	}

	XrSwapchainImageReleaseInfo releaseInfo = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
	const bool ok = Check(xrReleaseSwapchainImage(sc.handle, &releaseInfo), "xrReleaseSwapchainImage");
	sc.released |= ok;
	return ok;
}


bool CVRManager::Impl::DrawOverlay(uint32_t eye, const std::function<void()>& draw)
{
	ColorSwapchain& sc = overlay[eye];
	if (!overlayOK || sc.handle == XR_NULL_HANDLE)
		return false;

	const float clear[4] = {0.0f, 0.0f, 0.0f, 0.0f};
	return DrawInto(sc, clear, draw, "overlay");
}


bool CVRManager::Impl::DrawInto(ColorSwapchain& sc, const float clear[4], const std::function<void()>& draw, const char* what)
{
	if (sc.handle == XR_NULL_HANDLE)
		return false;

	uint32_t imageIndex = 0;
	XrSwapchainImageAcquireInfo acquireInfo = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
	if (!Check(xrAcquireSwapchainImage(sc.handle, &acquireInfo, &imageIndex), what))
		return false;

	XrSwapchainImageWaitInfo waitInfo = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
	waitInfo.timeout = XR_INFINITE_DURATION;
	if (!Check(xrWaitSwapchainImage(sc.handle, &waitInfo), what))
		return false;

	{
		BlitStateGuard guard;
		GLint prevViewport[4];
		GLfloat prevClear[4];
		glGetIntegerv(GL_VIEWPORT, prevViewport);
		glGetFloatv(GL_COLOR_CLEAR_VALUE, prevClear);

		glBindFramebuffer(GL_FRAMEBUFFER, blitFBO);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, sc.images[imageIndex].image, 0);
		glViewport(0, 0, sc.sizeX, sc.sizeY);
		glClearColor(clear[0], clear[1], clear[2], clear[3]);
		glClear(GL_COLOR_BUFFER_BIT);

		draw();

		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
		glClearColor(prevClear[0], prevClear[1], prevClear[2], prevClear[3]);
		glViewport(prevViewport[0], prevViewport[1], prevViewport[2], prevViewport[3]);
	}

	XrSwapchainImageReleaseInfo releaseInfo = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
	const bool ok = Check(xrReleaseSwapchainImage(sc.handle, &releaseInfo), what);
	sc.released |= ok;
	return ok;
}


// releases one image of a swapchain that has none yet, so a parked layer can
// name it (its pixels never show)
bool CVRManager::Impl::PrimeSwapchain(ColorSwapchain& sc)
{
	if (sc.handle == XR_NULL_HANDLE)
		return false;
	if (sc.released)
		return true;

	uint32_t imageIndex = 0;
	XrSwapchainImageAcquireInfo acquireInfo = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
	if (!Check(xrAcquireSwapchainImage(sc.handle, &acquireInfo, &imageIndex), "xrAcquireSwapchainImage (prime)"))
		return false;
	XrSwapchainImageWaitInfo waitInfo = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
	waitInfo.timeout = XR_INFINITE_DURATION;
	if (!Check(xrWaitSwapchainImage(sc.handle, &waitInfo), "xrWaitSwapchainImage (prime)"))
		return false;
	XrSwapchainImageReleaseInfo releaseInfo = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
	sc.released = Check(xrReleaseSwapchainImage(sc.handle, &releaseInfo), "xrReleaseSwapchainImage (prime)");
	return sc.released;
}


// grab the table with the grip button: one hand moves it, two hands also
// turn it (around the vertical) and zoom it (by the change in hand distance)
void CVRManager::Impl::UpdateTable()
{
	if (!tableView || !inputReady)
		return;

	int mask = 0;
	for (int h = 0; h < CVRInput::NUM_HANDS; ++h) {
		const VRHandState& hand = input.Hand(h);
		const bool wasHeld = ((grabMask >> h) & 1) != 0;
		// hysteresis so a half-pressed grip does not flicker
		const bool held = hand.gripValid && (hand.squeeze > (wasHeld ? 0.35f : 0.65f));
		mask |= (held ? 1 : 0) << h;
	}

	const float3 leftPos = ToF3(input.Hand(0).grip.position);
	const float3 rightPos = ToF3(input.Hand(1).grip.position);

	// a hand was added or released: restart the gesture from where things are
	if (mask != grabMask) {
		grabMask = mask;
		grabStart = table;

		switch (mask) {
			case 1: { grabAnchor = leftPos;  } break;
			case 2: { grabAnchor = rightPos; } break;
			case 3: {
				const float3 span = rightPos - leftPos;
				grabAnchor = (leftPos + rightPos) * 0.5f;
				grabYaw0 = YawOf(span);
				grabDist0 = std::max(span.Length(), 0.01f);
			} break;
			default: {} break;
		}
		return;
	}

	switch (mask) {
		case 1:
		case 2: {
			const float3 handPos = (mask == 1) ? leftPos : rightPos;
			table.center = grabStart.center + (handPos - grabAnchor);
		} break;
		case 3: {
			const float3 span = rightPos - leftPos;
			const float3 mid = (leftPos + rightPos) * 0.5f;

			// in the menu the table is an empty slab: turning and moving it is
			// kept for the next game, zooming (the map's scale) is not offered
			const float newSize = menuView ? grabStart.size : std::clamp(grabStart.size * (span.Length() / grabDist0), MIN_TABLE_SIZE, MAX_TABLE_SIZE);
			const float zoom = newSize / grabStart.size;
			const float turn = WrapAngle(YawOf(span) - grabYaw0);

			table.size = newSize;
			table.yaw = WrapAngle(grabStart.yaw + turn);
			table.center = mid + RotY((grabStart.center - grabAnchor) * zoom, turn);
		} break;
		default: {
			return;
		} break;
	}

	tableMoved = true;
}


void CVRManager::Impl::SaveTable() const
{
	if (!tableMoved)
		return;

	configHandler->Set("VRTableSize", table.size);
	configHandler->Set("VRTableX", table.center.x);
	configHandler->Set("VRTableHeight", table.center.y);
	configHandler->Set("VRTableDistance", -table.center.z);
	configHandler->Set("VRTableYaw", table.yaw * (180.0f / 3.14159265f));

	LOG("[VR] saved table placement: size %.2f m, centre (%.2f, %.2f, %.2f) m, turned %.0f degrees",
		table.size, table.center.x, table.center.y, table.center.z, table.yaw * (180.0f / 3.14159265f));
}


void CVRManager::Impl::PanelRect(bool behindTable, float3& center, float& width, float& height) const
{
	const float aspect = (mirror.sizeX > 0) ? static_cast<float>(mirror.sizeY) / static_cast<float>(mirror.sizeX) : 0.5625f;

	width = mirrorWidth;
	height = mirrorWidth * aspect;

	if (behindTable) {
		// in-game: a big TV on the war-room wall behind the table, fixed in the
		// room (moving or zooming the table doesn't push it away), its bottom
		// edge just below eye level so the table below stays in clear view
		center = float3(0.0f, height * 0.5f - 0.1f, -mirrorDistance);
	} else {
		center = float3(0.0f, 0.0f, -mirrorDistance);
	}
}


// picks up a changed set of UI panels from LuaUI; a panel keeps its swapchain
// (by name) until its pixel size changes, which SubmitFrame handles
void CVRManager::Impl::SyncUIPanels()
{
	// LuaMenu's panels while no game runs, LuaUI's in a game
	const std::vector<VRState::UIPanelRequest>& requests = menuView ? VRState::menuPanels : VRState::uiPanels;
	const int version = menuView ? VRState::menuPanelsVersion : VRState::uiPanelsVersion;

	if (uiPanelsVersionSeen == version && uiPanelsMenuSeen == menuView)
		return;

	uiPanelsVersionSeen = version;
	uiPanelsMenuSeen = menuView;

	const int winX = globalRendering->winSizeX;
	const int winY = globalRendering->winSizeY;

	std::vector<UIPanelLayer> next;
	std::string summary;

	for (const VRState::UIPanelRequest& req: requests) {
		// clamp to the window
		const int x0 = std::clamp(req.x, 0, winX);
		const int y0 = std::clamp(req.y, 0, winY);
		const int x1 = std::clamp(req.x + req.w, 0, winX);
		const int y1 = std::clamp(req.y + req.h, 0, winY);
		if (x1 - x0 < 4 || y1 - y0 < 4)
			continue;

		UIPanelLayer layer;
		const auto old = std::find_if(uiLayers.begin(), uiLayers.end(), [&](const UIPanelLayer& l) { return l.name == req.name && l.sc.handle != XR_NULL_HANDLE; });
		if (old != uiLayers.end()) {
			layer.sc = old->sc;
			old->sc = ColorSwapchain(); // moved
		} else {
			const auto spare = std::find_if(uiSpare.begin(), uiSpare.end(), [&](const auto& s) { return s.first == req.name; });
			if (spare != uiSpare.end()) {
				layer.sc = spare->second;
				uiSpare.erase(spare);
			}
		}

		if (std::find(slotNames.begin(), slotNames.end(), req.name) == slotNames.end())
			slotNames.push_back(req.name);

		layer.name = req.name;
		layer.anchor = req.anchor;
		layer.canvas = req.canvas;
		layer.hidden = req.hidden;
		layer.x = x0;
		layer.y = y0;
		layer.w = x1 - x0;
		layer.h = y1 - y0;
		layer.metresPerPixel = (req.widthM > 0.0f) ? req.widthM / static_cast<float>(layer.w) : 0.0f;

		summary += " " + layer.name + (layer.hidden ? "[hidden]" : "") + ((layer.anchor == VRState::UI_ANCHOR_PALETTE) ? "(palette " : (layer.anchor == VRState::UI_ANCHOR_SIDE) ? "(side " : (layer.anchor == VRState::UI_ANCHOR_CENTER) ? "(center " : (layer.anchor == VRState::UI_ANCHOR_WALL) ? "(wall " : "(bar ") + std::to_string(layer.w) + "x" + std::to_string(layer.h) + ")";
		next.push_back(std::move(layer));
	}

	// dropped panels keep their swapchains for when they come back; SubmitFrame
	// keeps submitting them, parked, in their own place (see slotNames)
	for (UIPanelLayer& layer: uiLayers) {
		if (layer.sc.handle == XR_NULL_HANDLE)
			continue;
		const auto spare = std::find_if(uiSpare.begin(), uiSpare.end(), [&](const auto& s) { return s.first == layer.name; });
		if (spare != uiSpare.end()) {
			xrDestroySwapchain(spare->second.handle);
			spare->second = layer.sc;
		} else {
			uiSpare.emplace_back(layer.name, layer.sc);
		}
	}

	uiLayers = std::move(next);

	// every change of the set (rare: a screen change, a panel shown or hidden)
	LOG("[VR] UI panels from %s:%s", menuView ? "LuaMenu" : "LuaUI", summary.empty() ? " none" : summary.c_str());
}


void CVRManager::Impl::HideUIPanels()
{
	for (UIPanelLayer& layer: uiLayers) {
		layer.visible = false;
	}
}


// makes the UI panels' swapchains (hidden panels too, so showing one later is
// instant), before anything draws this frame. SteamVR's xrCreateSwapchain can
// fail with XR_ERROR_RUNTIME_FAILURE (seen in the headset 2026-11-24 for the
// side panel, every frame, so it never showed): it is tried with no GL error
// pending and nothing bound, and after a failure only every half second or so.
void CVRManager::Impl::EnsureUISwapchains()
{
	if (swapchainRetryIn > 0) {
		swapchainRetryIn--;
		return;
	}

	for (UIPanelLayer& layer: uiLayers) {
		if (layer.sc.handle != XR_NULL_HANDLE && (layer.sc.sizeX != layer.w || layer.sc.sizeY != layer.h)) {
			xrDestroySwapchain(layer.sc.handle);
			layer.sc = ColorSwapchain();
		}
		if (layer.sc.handle != XR_NULL_HANDLE)
			continue;

		GLenum pending = GL_NO_ERROR;
		for (GLenum e; (e = glGetError()) != GL_NO_ERROR; )
			pending = e;

		bool made = false;
		{
			BlitStateGuard guard;
			GLint prevUnpack = 0, prevPack = 0, prevActive = 0, prevTex = 0;
			glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &prevUnpack);
			glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &prevPack);
			glGetIntegerv(GL_ACTIVE_TEXTURE, &prevActive);
			glActiveTexture(GL_TEXTURE0);
			glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex);

			glBindFramebuffer(GL_FRAMEBUFFER, 0);
			glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
			glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
			glBindTexture(GL_TEXTURE_2D, 0);

			made = CreateColorSwapchain(layer.w, layer.h, layer.sc);

			glBindTexture(GL_TEXTURE_2D, prevTex);
			glActiveTexture(prevActive);
			glBindBuffer(GL_PIXEL_PACK_BUFFER, prevPack);
			glBindBuffer(GL_PIXEL_UNPACK_BUFFER, prevUnpack);
		}

		if (made) {
			if (swapchainFailures > 0)
				LOG("[VR] made the swapchain for UI panel %s (%dx%d) after %d failed tries", layer.name.c_str(), layer.w, layer.h, swapchainFailures);
			swapchainFailures = 0;
			continue;
		}

		if (layer.sc.handle != XR_NULL_HANDLE)
			xrDestroySwapchain(layer.sc.handle);
		layer.sc = ColorSwapchain();

		if (swapchainFailures == 0 || (swapchainFailures % 20) == 0)
			LOG_L(L_WARNING, "[VR] could not make the swapchain for UI panel %s (%dx%d), try %d (GL error before it: 0x%x); retrying", layer.name.c_str(), layer.w, layer.h, swapchainFailures + 1, pending);
		swapchainFailures++;
		swapchainRetryIn = 45;
		return;
	}
}


// places the UI panels for this frame (LOCAL space):
//  - bar: standing on the table's far edge, facing the player, leaning back a
//    little, left to right in list order; never farther than VRBarMaxDistance
//  - side: off to the pointer hand's side, following the head lazily
//  - palette: over the off hand's controller, facing the eyes, bottom to top
//    in list order, shown while that hand is raised in front of you
void CVRManager::Impl::LayoutUIPanels()
{
	HideUIPanels();

	if (!HaveUIPanels())
		return;

	float3 head;
	if (viewsValid)
		head = (ToF3(views[0].pose.position) + ToF3(views[1].pose.position)) * 0.5f;

	// the table top's half extents, in metres, in its own frame
	const float mapX = static_cast<float>(mapDims.mapx * SQUARE_SIZE);
	const float mapZ = static_cast<float>(mapDims.mapy * SQUARE_SIZE);
	const float mapMax = std::max(std::max(mapX, mapZ), 1.0f);
	// in the menu: the empty square slab DrawMenuRoom draws
	const float halfX = menuView ? MenuTableHalf(table) : table.size * mapX / mapMax * 0.5f;
	const float halfZ = menuView ? MenuTableHalf(table) : table.size * mapZ / mapMax * 0.5f;

	// the bar
	{
		float totalW = 0.0f;
		int count = 0;
		for (const UIPanelLayer& layer: uiLayers) {
			if (layer.anchor != VRState::UI_ANCHOR_BAR || layer.hidden)
				continue;
			totalW += layer.w * Scale(layer, barScale);
			count++;
		}

		if (count > 0) {
			constexpr float GAP = 0.02f;
			constexpr float LEAN = 15.0f * 0.0174533f;
			totalW += GAP * (count - 1);

			// the bar stands on the table edge opposite the player, wherever they
			// walk: its heading turns smoothly towards the head
			float3 toHead = head - table.center;
			toHead.y = 0.0f;
			const float targetYaw = (toHead.SqLength() > 1e-4f) ? std::atan2(toHead.x, toHead.z) : table.yaw;
			if (!barYawValid) {
				barYaw = targetYaw;
				barYawValid = true;
			}
			barYaw = WrapAngle(barYaw + WrapAngle(targetYaw - barYaw) * 0.08f);

			// "away" from the head, in the table's own frame, to its edge
			const float3 away = RotY(float3(0.0f, 0.0f, -1.0f), barYaw);
			const float3 awayLocal = RotY(away, -table.yaw);
			const float edgeT = std::min(
				(std::abs(awayLocal.x) > 1e-4f) ? halfX / std::abs(awayLocal.x) : 1e9f,
				(std::abs(awayLocal.z) > 1e-4f) ? halfZ / std::abs(awayLocal.z) : 1e9f
			);
			float3 base = table.center + away * (edgeT + 0.05f);
			base.y = table.center.y + 0.02f;

			// it always stays on the edge, never over the battlefield (a panel is drawn
			// over everything behind it); farther than VRBarMaxDistance it grows
			// instead, so it reads the same size
			float3 toBase = base - head;
			toBase.y = 0.0f;
			const float grow = std::max(1.0f, toBase.Length() / barMaxDistance);

			// face the player, top leaning away
			const XrQuaternionf q = QuatMul(QuatAxisAngle(UpVector, barYaw), QuatAxisAngle(float3(1.0f, 0.0f, 0.0f), -LEAN));
			const float3 right = ToF3(Rotate(q, {1.0f, 0.0f, 0.0f}));
			const float3 up = ToF3(Rotate(q, {0.0f, 1.0f, 0.0f}));

			float x = -totalW * grow * 0.5f;
			for (UIPanelLayer& layer: uiLayers) {
				if (layer.anchor != VRState::UI_ANCHOR_BAR || layer.hidden)
					continue;

				const float w = layer.w * Scale(layer, barScale) * grow;
				const float h = layer.h * Scale(layer, barScale) * grow;
				const float3 c = base + right * (x + w * 0.5f) + up * (h * 0.5f);

				layer.quad.pose.orientation = q;
				layer.quad.pose.position = ToXr(c);
				layer.quad.width = w;
				layer.quad.height = h;
				layer.visible = true;
				x += w + GAP * grow;
			}
		}
	}

	// the side panel: off to the pointer hand's side at about waist height,
	// facing the head. It follows lazily: it stays where it is while the head
	// turns less than SIDE_DEADZONE away from it, then glides along, so it is
	// always a short glance away but never swims with every head movement.
	{
		float totalH = 0.0f;
		for (const UIPanelLayer& layer: uiLayers) {
			if (layer.anchor == VRState::UI_ANCHOR_SIDE && !layer.hidden)
				totalH += layer.h * Scale(layer, sideScale);
		}

		if (totalH > 0.0f && viewsValid) {
			constexpr float SIDE_DEADZONE = 20.0f * 0.0174533f;
			constexpr float GAP = 0.015f;

			float3 fwd = ToF3(Rotate(views[0].pose.orientation, {0.0f, 0.0f, -1.0f}));
			fwd.y = 0.0f;
			const float headYaw = (fwd.SqLength() > 1e-4f) ? YawOf(fwd) : sideYaw;

			if (!sideValid) {
				sideYaw = headYaw;
				sidePos = head;
				sideValid = true;
			}

			// in use: while you look at the panel (its middle within its own half
			// width plus a margin of where you face) or point at it, it holds still,
			// so turning to it never makes it run away (2026-11-27). It lets go
			// once you look clearly away (a wider margin, so it doesn't flicker).
			{
				float panelW = 0.0f;
				for (const UIPanelLayer& layer: uiLayers) {
					if (layer.anchor == VRState::UI_ANCHOR_SIDE && !layer.hidden)
						panelW = std::max(panelW, layer.w * Scale(layer, sideScale));
				}
				bool looking = false;
				float3 toPanel = sideCenter - head;
				toPanel.y = 0.0f;
				if (sideCenterValid && toPanel.SqLength() > 1e-4f) {
					const float off = std::abs(WrapAngle(YawOf(toPanel) - headYaw));
					const float halfAngle = std::atan2(panelW * 0.5f, toPanel.Length());
					const float margin = (sideEngaged ? 20.0f : 8.0f) * 0.0174533f;
					looking = off < halfAngle + margin;
				}
				const bool engaged = looking || pointerOnSide;
				if (engaged != sideEngaged)
					LOG("[VR] side panel %s", engaged ? "held (in use)" : "following again");
				sideEngaged = engaged;
			}

			// the yaw the panel is anchored to: pulled along only by the part of
			// the head turn beyond the dead zone, eased; not while in use
			if (!sideEngaged) {
				const float diff = WrapAngle(headYaw - sideYaw);
				if (std::abs(diff) > SIDE_DEADZONE) {
					const float excess = diff - std::copysign(SIDE_DEADZONE, diff);
					sideYaw = WrapAngle(sideYaw + excess * 0.25f);
				}
				sidePos = sidePos + (head - sidePos) * 0.1f;
			}

			// on the pointer hand's side, within easy reach of the pointer: pointer
			// hand right (1), the panel goes to the right (clockwise, negative yaw).
			// It swaps sides with the pointer (left-handed players).
			const float sideSign = (pointerHand == 1) ? -1.0f : 1.0f;

			float totalW = 0.0f;
			for (const UIPanelLayer& layer: uiLayers) {
				if (layer.anchor == VRState::UI_ANCHOR_SIDE && !layer.hidden)
					totalW = std::max(totalW, layer.w * Scale(layer, sideScale));
			}

			// where the panel stands at a given angle round from the anchored yaw
			struct SidePlace { float yaw; float3 base; XrQuaternionf q; };
			const auto Place = [&](float angle, float lift) {
				SidePlace p;
				p.yaw = sideYaw + sideSign * angle;
				p.base = sidePos + RotY(float3(0.0f, 0.0f, -1.0f), p.yaw) * sideDistance;
				p.base.y = sidePos.y + sideHeight + lift;
				// turned to face the eyes where they are now, as the palette is: its
				// +z points at the head, leaning back as far as the head is above it.
				// The anchor follows the head lazily and holds still while in use, so
				// a fixed angle went askew as you walked through the battlefield and
				// read badly (2026-11-28); facing the eyes keeps it square to you.
				float3 toHeadFlat = head - p.base;
				toHeadFlat.y = 0.0f;
				const float faceYaw = (toHeadFlat.SqLength() > 1e-6f) ? YawOf(-toHeadFlat) : p.yaw;
				const float3 n = (head - p.base).SafeNormalize();
				const float lean = std::asin(std::clamp(n.y, -0.95f, 0.95f));
				p.q = QuatMul(QuatAxisAngle(UpVector, faceYaw), QuatAxisAngle(float3(1.0f, 0.0f, 0.0f), -lean));
				return p;
			};

			// how much of the table the panel hides from the eyes: rays from the
			// head through a grid of points on the panel, counted where they land
			// on the table top (with a margin for what stands on it). A panel is a
			// compositor layer drawn over the whole eye image, so it hides the
			// table whether it stands in front of it, inside it or beyond it.
			const auto Blocked = [&](const SidePlace& p) {
				constexpr float MARGIN = 0.05f;
				const float3 right = ToF3(Rotate(p.q, {1.0f, 0.0f, 0.0f}));
				const float3 up = ToF3(Rotate(p.q, {0.0f, 1.0f, 0.0f}));
				int hits = 0;
				for (int i = -2; i <= 2; ++i) {
					for (int j = -2; j <= 2; ++j) {
						const float3 pt = p.base + right * (i * totalW * 0.25f) + up * (j * totalH * 0.25f);
						const float3 ray = pt - head;
						if (ray.y >= -1e-4f)
							continue; // looking level or up: never reaches the table top
						const float t = (table.center.y - head.y) / ray.y;
						const float3 local = RotY(head + ray * t - table.center, -table.yaw);
						hits += (std::abs(local.x) < halfX + MARGIN && std::abs(local.z) < halfZ + MARGIN);
					}
				}
				return hits;
			};

			// it stays at VRSideAngle beside you (the user wades through the
			// battlefield, so going round the table made them turn far too much,
			// 2026-11-27) and rises in steps, up to eye level, until it hides none
			// of the table (the rays then pass over the table). If no height is
			// clear, the one that hides the least. Eased, so the panel glides.
			constexpr float LIFT_STEP = 0.05f;
			const float maxLift = std::max(0.0f, -sideHeight + totalH * 0.5f);
			const float bestAngle = sideAngle;
			float bestLift = 0.0f;
			int bestHits = 1 << 30;
			for (float lift = 0.0f; lift <= maxLift + LIFT_STEP - 1e-3f && bestHits > 0; lift += LIFT_STEP) {
				const float l = std::min(lift, maxLift);
				const int hits = Blocked(Place(bestAngle, l));
				if (hits < bestHits) {
					bestHits = hits;
					bestLift = l;
				}
			}
			if (!sideAngleValid) {
				sideAngleNow = bestAngle;
				sideLiftNow = bestLift;
				sideAngleValid = true;
			}
			if (!sideEngaged) {
				sideAngleNow += (bestAngle - sideAngleNow) * 0.06f;
				sideLiftNow += (bestLift - sideLiftNow) * 0.06f;
			}

			const SidePlace place = Place(sideAngleNow, sideLiftNow);
			sideCenter = place.base;
			sideCenterValid = true;
			const float3 base = place.base;
			const XrQuaternionf q = place.q;
			const float3 up = ToF3(Rotate(q, {0.0f, 1.0f, 0.0f}));

			// stacked bottom to top, centred on the anchor height
			float y = -totalH * 0.5f;
			for (UIPanelLayer& layer: uiLayers) {
				if (layer.anchor != VRState::UI_ANCHOR_SIDE || layer.hidden)
					continue;

				const float w = layer.w * Scale(layer, sideScale);
				const float h = layer.h * Scale(layer, sideScale);
				const float3 c = base + up * (y + h * 0.5f);

				layer.quad.pose.orientation = q;
				layer.quad.pose.position = ToXr(c);
				layer.quad.width = w;
				layer.quad.height = h;
				layer.visible = true;
				y += h + GAP;
			}
		}
	}

	// centre panels (a menu): in front of you at about eye height, placed
	// where you look when they appear and left there until they are hidden,
	// so looking around or pointing at them never moves them
	{
		float totalH = 0.0f;
		for (const UIPanelLayer& layer: uiLayers) {
			if (layer.anchor == VRState::UI_ANCHOR_CENTER && !layer.hidden)
				totalH += layer.h * Scale(layer, sideScale);
		}

		if (totalH <= 0.0f || !viewsValid) {
			centerValid = false;
		} else {
			constexpr float DISTANCE = 0.75f;
			constexpr float DROP = 0.08f; // centre below the eyes
			constexpr float LEAN = 10.0f * 0.0174533f;
			constexpr float GAP = 0.015f;

			if (!centerValid) {
				float3 fwd = ToF3(Rotate(views[0].pose.orientation, {0.0f, 0.0f, -1.0f}));
				fwd.y = 0.0f;
				centerYaw = (fwd.SqLength() > 1e-4f) ? YawOf(fwd) : 0.0f;
				centerPos = head + RotY(float3(0.0f, 0.0f, -1.0f), centerYaw) * DISTANCE;
				centerPos.y = head.y - DROP;
				centerValid = true;
			}

			const XrQuaternionf q = QuatMul(QuatAxisAngle(UpVector, centerYaw), QuatAxisAngle(float3(1.0f, 0.0f, 0.0f), -LEAN));
			const float3 up = ToF3(Rotate(q, {0.0f, 1.0f, 0.0f}));

			// stacked top to bottom in list order, centred on the anchor
			float y = totalH * 0.5f;
			for (UIPanelLayer& layer: uiLayers) {
				if (layer.anchor != VRState::UI_ANCHOR_CENTER || layer.hidden)
					continue;

				const float w = layer.w * Scale(layer, sideScale);
				const float h = layer.h * Scale(layer, sideScale);
				const float3 c = centerPos + up * (y - h * 0.5f);

				layer.quad.pose.orientation = q;
				layer.quad.pose.position = ToXr(c);
				layer.quad.width = w;
				layer.quad.height = h;
				layer.visible = true;
				y -= h + GAP;
			}
		}
	}

	// wall panels (the window, when the player hangs it on the wall): on the
	// war-room wall to the left of the TV, facing the room like the TV, fixed
	// in the room. Grown by the TV's distance over a centre panel's, so they
	// look as big from where you start as the window in front of you does.
	{
		constexpr float CENTER_DISTANCE = 0.75f; // as the centre panels
		constexpr float GAP = 0.12f;
		const float grow = std::max(1.0f, mirrorDistance / CENTER_DISTANCE);

		float totalH = 0.0f;
		float maxW = 0.0f;
		for (const UIPanelLayer& layer: uiLayers) {
			if (layer.anchor != VRState::UI_ANCHOR_WALL || layer.hidden)
				continue;
			totalH += layer.h * Scale(layer, sideScale) * grow;
			maxW = std::max(maxW, layer.w * Scale(layer, sideScale) * grow);
		}

		if (totalH > 0.0f) {
			float3 tvCenter;
			float tvW = 0.0f;
			float tvH = 0.0f;
			PanelRect(true, tvCenter, tvW, tvH);

			// level with the TV's middle, but never with its bottom below the floor
			// grid's reach: at most 1.2 m below the eyes
			const float midY = std::max(tvCenter.y, -1.2f + totalH * 0.5f);
			const float x = tvCenter.x - tvW * 0.5f - GAP - maxW * 0.5f;
			const XrQuaternionf q = {0.0f, 0.0f, 0.0f, 1.0f};

			float y = midY + totalH * 0.5f;
			for (UIPanelLayer& layer: uiLayers) {
				if (layer.anchor != VRState::UI_ANCHOR_WALL || layer.hidden)
					continue;

				const float w = layer.w * Scale(layer, sideScale) * grow;
				const float h = layer.h * Scale(layer, sideScale) * grow;

				layer.quad.pose.orientation = q;
				layer.quad.pose.position = ToXr(float3(x, y - h * 0.5f, tvCenter.z));
				layer.quad.width = w;
				layer.quad.height = h;
				layer.visible = true;
				y -= h + 0.015f;
			}
		}
	}

	// the palette, on the hand that is not pointing, unless it holds the table
	{
		const int paletteHand = 1 - pointerHand;
		const VRHandState& hand = input.Hand(paletteHand);
		const bool handFree = hand.gripValid && ((grabMask >> paletteHand) & 1) == 0;

		float totalH = 0.0f;
		for (const UIPanelLayer& layer: uiLayers) {
			if (layer.anchor == VRState::UI_ANCHOR_PALETTE && !layer.hidden)
				totalH += layer.h * Scale(layer, paletteScale);
		}

		if (!handFree || totalH <= 0.0f || !viewsValid) {
			paletteShown = false;
			return;
		}

		constexpr float GAP = 0.01f;
		constexpr float LIFT = 0.04f; // metres from the grip up to the palette's bottom edge

		// docked over the off hand like a PDA held up: it moves with the hand
		// but always faces the eyes, so no wrist tilt is needed to read it.
		// Shown while the hand is raised (above VRPaletteRaise below the eyes)
		// and in front of you, so a hand resting at your side or low over the
		// table never puts it in the way. Hysteresis so it doesn't flicker.
		const float3 grip = ToF3(hand.grip.position);
		const float3 centre = grip + UpVector * (LIFT + totalH * 0.5f);
		const float3 fwd = ToF3(Rotate(views[0].pose.orientation, {0.0f, 0.0f, -1.0f})).SafeNormalize();
		const float3 toHand = (centre - head).SafeNormalize();
		const float hys = paletteShown ? 0.05f : 0.0f;
		const bool raised = (grip.y > head.y - paletteRaise - hys);
		const bool inFront = (toHand.dot(fwd) > (paletteShown ? 0.35f : 0.5f));
		paletteShown = raised && inFront;
		if (!paletteShown)
			return;

		// face the eyes: +z towards the head, leaning back as far as the head is above
		float3 toHeadFlat = head - centre;
		toHeadFlat.y = 0.0f;
		const float yaw = (toHeadFlat.SqLength() > 1e-6f) ? YawOf(-toHeadFlat) : 0.0f;
		const float3 n = (head - centre).SafeNormalize();
		const float lean = std::asin(std::clamp(n.y, -0.95f, 0.95f));
		const XrQuaternionf q = QuatMul(QuatAxisAngle(UpVector, yaw), QuatAxisAngle(float3(1.0f, 0.0f, 0.0f), -lean));
		const float3 up = ToF3(Rotate(q, {0.0f, 1.0f, 0.0f}));

		// bottom to top in list order, from LIFT above the grip
		float y = LIFT;
		for (UIPanelLayer& layer: uiLayers) {
			if (layer.anchor != VRState::UI_ANCHOR_PALETTE || layer.hidden)
				continue;

			const float w = layer.w * Scale(layer, paletteScale);
			const float h = layer.h * Scale(layer, paletteScale);
			const float3 c = grip + up * (y + h * 0.5f);

			layer.quad.pose.orientation = q;
			layer.quad.pose.position = ToXr(c);
			layer.quad.width = w;
			layer.quad.height = h;
			layer.visible = true;
			y += h + GAP;
		}
	}
}


void CVRManager::Impl::UpdatePointer()
{
	pointerOnSide = false; // set again below when the pointer is on a side panel

	pointerHit = 0;

	// debounce the buttons and find this frame's presses
	bool pressed[CVRInput::NUM_HANDS][3] = {};
	for (int h = 0; h < CVRInput::NUM_HANDS; ++h) {
		const VRHandState& hand = input.Hand(h);
		bool* b = pointerButtons[h];
		const bool now[3] = {hand.trigger > (b[0] ? 0.4f : 0.6f), hand.command, hand.stickClick};

		for (int i = 0; i < 3; ++i) {
			pressed[h][i] = now[i] && !b[i];
			b[i] = now[i];
		}
	}

	// a thumbstick click or A/X on the other hand moves the pointer there, once
	// the pointer hand holds no mouse button (A/X then also right-clicks, as before)
	// a thumbstick click on the pointer hand shows or hides the big desktop
	// panel (menus, chat, settings; shown by default) while the UI panels are up
	if (pressed[pointerHand][2] && HaveUIPanels()) {
		VRState::tvShown = !VRState::tvShown;
		LOG("[VR] desktop panel %s", VRState::tvShown ? "shown" : "hidden");
	}

	const int otherHand = 1 - pointerHand;
	const bool pointerBusy = (triggerRole[pointerHand] == TRIGGER_CLICK) || commandClick[pointerHand];
	if ((pressed[otherHand][1] || pressed[otherHand][2]) && !pointerBusy)
		pointerHand = otherHand;
	VRState::pointerHand = pointerHand;

	// what a trigger does is decided when it is pressed and kept until release,
	// so a hand switch never turns a held Shift into a click or the reverse
	bool shift = false;
	for (int h = 0; h < CVRInput::NUM_HANDS; ++h) {
		if (!pointerButtons[h][0]) {
			triggerRole[h] = TRIGGER_NONE;
		} else if (pressed[h][0]) {
			triggerRole[h] = (h == pointerHand) ? TRIGGER_CLICK : TRIGGER_SHIFT;
		}

		if (!pointerButtons[h][1]) {
			commandClick[h] = false;
		} else if (pressed[h][1] && h == pointerHand) {
			commandClick[h] = true;
		}

		shift |= (triggerRole[h] == TRIGGER_SHIFT);
	}

	// the off hand's B/Y holds Ctrl and the pointer hand's holds Alt, as the
	// off-hand trigger holds Shift (the lobby has no orders to queue)
	const bool ctrl = input.Hand(1 - pointerHand).secondary;
	const bool alt = input.Hand(pointerHand).secondary;
	VRPointer::SetModifiers(shift && !menuView, ctrl && !menuView, alt && !menuView);

	for (int h = 0; h < CVRInput::NUM_HANDS; ++h) {
		const VRHandState& hs = input.Hand(h);
		VRState::HandInput& hi = VRState::handInput[h];
		hi.valid = hs.aimValid || hs.gripValid;
		hi.trigger = hs.trigger;
		hi.grip = hs.squeeze;
		hi.stickX = hs.thumbstick.x;
		hi.stickY = hs.thumbstick.y;
		hi.primary = hs.command;
		hi.secondary = hs.secondary;
		hi.stickClick = hs.stickClick;
	}

	const VRHandState& hand = input.Hand(pointerHand);

	// menu only: the pointer hand's thumbstick scrolls what the pointer is on
	// (lists in the lobby), in mouse-wheel steps: up to ~12 a second, stick
	// pushed away from you scrolls up
	int wheel = 0;
	if (menuView) {
		constexpr float DEADZONE = 0.3f;
		constexpr float STEPS_PER_SECOND = 12.0f;
		const float y = hand.thumbstick.y;
		const float dt = std::clamp(static_cast<float>(frameState.predictedDisplayPeriod) * 1e-9f, 0.001f, 0.05f);
		if (std::abs(y) > DEADZONE) {
			const float push = (std::abs(y) - DEADZONE) / (1.0f - DEADZONE);
			scrollSteps += std::copysign(push * STEPS_PER_SECOND * dt, y);
		} else {
			scrollSteps = 0.0f;
		}
		wheel = static_cast<int>(scrollSteps);
		scrollSteps -= static_cast<float>(wheel);
	}
	VRPointer::Input in;
	in.left = (triggerRole[pointerHand] == TRIGGER_CLICK);
	in.right = commandClick[pointerHand];

	const XrToWorld m = TableMapping(table);
	float panelT = -1.0f;
	XrVector3f panelHitXR = {};
	bool onCanvas = false; // the nearest panel hit is a VR UI (canvas) panel

	if (hand.aimValid) {
		const XrVector3f dirXR = Rotate(hand.aim.orientation, {0.0f, 0.0f, -1.0f});

		in.rayValid = true;
		in.from = m.Pos(hand.aim.position);
		in.dir = m.Dir(dirXR).SafeNormalize();
		in.length = POINTER_LENGTH * m.scale;

		// the eyes, for judging which units they see as icons (the pick runs
		// before this frame's eyes are drawn, which also set these)
		if (viewsValid) {
			VRState::eyePos[0] = m.Pos(views[0].pose.position);
			VRState::eyePos[1] = m.Pos(views[1].pose.position);
		}

		// panels (the desktop panel and the UI panels) win over the world; the
		// nearest one hit takes the pointer, mapped to its window pixels
		const int winX = globalRendering->winSizeX;
		const int winY = globalRendering->winSizeY;

		if (winX > 0 && winY > 0) {
			const float3 from = ToF3(hand.aim.position);
			const float3 dir = ToF3(dirXR);

			const UIPanelLayer* hitPanel = nullptr;
			const auto TryPanel = [&](const PanelQuad& quad, int px, int py, int pw, int ph, bool canvas) {
				float t = 0.0f, u = 0.0f, v = 0.0f;
				if (!RayQuad(quad, from, dir, t, u, v) || (panelT > 0.0f && t >= panelT))
					return false;

				in.onPanel = !canvas;
				onCanvas = canvas;
				in.panelX = std::clamp(px + static_cast<int>(u * pw), 0, winX - 1);
				in.panelY = std::clamp(py + static_cast<int>((1.0f - v) * ph), 0, winY - 1);
				panelT = t;
				panelHitXR = ToXr(from + dir * t);
				return true;
			};

			if (TVVisible(true)) {
				PanelQuad tv;
				float3 center;
				PanelRect(true, center, tv.width, tv.height);
				tv.pose.position = ToXr(center);
				TryPanel(tv, 0, 0, winX, winY, false);
			}

			for (const UIPanelLayer& layer: uiLayers) {
				if (layer.visible && layer.drawn && TryPanel(layer.quad, layer.x, layer.y, layer.w, layer.h, layer.canvas))
					hitPanel = &layer;
			}

			pointerOnSide = (hitPanel != nullptr) && (hitPanel->anchor == VRState::UI_ANCHOR_SIDE);

			// pointer log (see VRPointer.cpp): which panel takes the pointer, on change
			const std::string hitName = (hitPanel != nullptr) ? hitPanel->name : (panelT > 0.0f ? std::string("tv") : std::string());
			if (hitName != lastPanelLogged) {
				if (!hitName.empty())
					LOG("[VR] pointer: on panel %s", hitName.c_str());
				lastPanelLogged = hitName;
			}
		}
	}

	// VR UI panels (RmlUi "vr_" contexts) take the pointer directly, never the
	// desktop cursor. A press keeps its target (UI or world) until release, so a
	// drag that starts in the world never clicks the UI, and the reverse.
	const bool wants[2] = {in.left, in.right};
	const bool uiHeld = uiButtons[0] || uiButtons[1];
	const bool toUI = uiHeld || (!worldHeld && onCanvas);

	if (toUI) {
		if (onCanvas) {
			VRRml::PointerMove(in.panelX, in.panelY);
			uiHover = true;
		}
		for (int b = 0; b < 2; ++b) {
			if (wants[b] && !uiButtons[b] && onCanvas) {
				VRRml::PointerButton(b, true);
				uiButtons[b] = true;
			} else if (!wants[b] && uiButtons[b]) {
				VRRml::PointerButton(b, false);
				uiButtons[b] = false;
			}
		}

		// the desktop side sees no pointer this frame (and lets go of nothing it holds)
		VRPointer::Input none;
		VRPointer::Update(none);
		worldHeld = false;

		if (onCanvas) {
			pointerHit = 2;
			pointerHitPos = m.Pos(panelHitXR);
			pointerHitXR = ToF3(panelHitXR);
			// RmlUi wheel steps: positive scrolls down
			if (wheel != 0)
				VRRml::PointerWheel(static_cast<float>(-wheel));
		}
		return;
	}

	if (uiHover) {
		VRRml::PointerLeave();
		uiHover = false;
	}
	if (onCanvas) {
		// a world drag passing over the UI: the world keeps the pointer
		in.onPanel = false;
		panelT = -1.0f;
	}

	const VRPointer::Result result = VRPointer::Update(in);
	worldHeld = in.left || in.right;

	if (in.onPanel && panelT > 0.0f) {
		pointerHit = 2;
		pointerHitPos = m.Pos(panelHitXR);
		pointerHitXR = ToF3(panelHitXR);
		if (wheel != 0)
			VRPointer::Scroll(static_cast<float>(wheel));
	} else if (result.worldHit) {
		pointerHit = 1;
		pointerHitPos = result.pos;
	}
}


void CVRManager::Impl::SubmitFrame(bool haveFrame)
{
	XrCompositionLayerProjectionView projViews[NUM_EYES];
	XrCompositionLayerProjection projection = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
	XrCompositionLayerQuad panel = {XR_TYPE_COMPOSITION_LAYER_QUAD};
	XrCompositionLayerQuad uiQuads[MAX_UI_LAYERS];

	const XrCompositionLayerBaseHeader* layers[3 + MAX_UI_LAYERS];
	XrCompositionLayerProjectionView overlayViews[NUM_EYES];
	XrCompositionLayerProjection overlayLayer = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
	uint32_t layerCount = 0;

	// the eye images: the game's world (DrawEyes) or the menu's war room (DrawMenuRoom)
	const bool eyeImages = eyesDrawn || roomDrawn;

	if (frameState.shouldRender && eyeImages) {
		for (uint32_t i = 0; i < NUM_EYES; ++i) {
			projViews[i] = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
			projViews[i].pose = views[i].pose;
			projViews[i].fov = views[i].fov;
			projViews[i].subImage.swapchain = eyes[i].handle;
			projViews[i].subImage.imageRect.offset = {0, 0};
			projViews[i].subImage.imageRect.extent = {eyes[i].sizeX, eyes[i].sizeY};
			projViews[i].subImage.imageArrayIndex = 0;
		}

		projection.space = localSpace;
		projection.viewCount = NUM_EYES;
		projection.views = projViews;
		layers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection);
	}

	const int winX = globalRendering->winSizeX;
	const int winY = globalRendering->winSizeY;
	// the table view with a room round it (a game, or the menu's war room):
	// the TV hangs on the wall and the UI panels show
	const bool inGameView = eyeImages && tableView;
	const bool anyUI = inGameView && std::any_of(uiLayers.begin(), uiLayers.end(), [](const UIPanelLayer& l) { return l.visible && !l.canvas; });
	const bool anyCanvas = inGameView && std::any_of(uiLayers.begin(), uiLayers.end(), [](const UIPanelLayer& l) { return l.visible && l.canvas; });
	const bool resolved = frameState.shouldRender && haveFrame && (TVVisible(inGameView) || anyUI) && ResolveBackBuffer(0, 0, winX, winY);
	// the canvas is drawn here, not by the desktop frame, so it is fresh every
	// frame (the menu leaves out desktop frames while idle)
	const bool canvasReady = frameState.shouldRender && anyCanvas && RenderCanvas(winX, winY);

	// SteamVR shows a quad layer that changes swapchain between frames (in the
	// same place of the layer list) cropped and stretched to the previous
	// swapchain's size: seen in the headset 2026-11-25 as the side panel and
	// the menu zoomed in on their bottom left, coming and going as the TV or a
	// panel was shown or hidden (which moved the later layers). So in the game
	// view every quad keeps its place: one that is not shown is still
	// submitted, "parked" (a millimetre across, far below the floor).
	const auto Park = [](XrCompositionLayerQuad& quad) {
		quad.pose = {};
		quad.pose.orientation.w = 1.0f;
		quad.pose.position = {0.0f, -1000.0f, 0.0f};
		quad.size = {0.001f, 0.001f};
	};

	const bool tvOn = resolved && TVVisible(inGameView) && BlitResolvedTo(mirror, 0, 0, winX, winY);
	// no new desktop frame (Chobby's FPS limit skips draws when idle): show
	// the last image again where it was, rather than an empty frame, which
	// SteamVR answers by taking the headset's focus away (seen 2026-11-26)
	const bool tvHeld = !tvOn && !haveFrame && frameState.shouldRender && tvSubmitted && mirror.released && TVVisible(inGameView);
	if (tvOn || tvHeld || (frameState.shouldRender && inGameView && PrimeSwapchain(mirror))) {
		panel.space = localSpace;
		panel.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
		panel.subImage.swapchain = mirror.handle;
		panel.subImage.imageRect.offset = {0, 0};
		panel.subImage.imageRect.extent = {mirror.sizeX, mirror.sizeY};
		panel.subImage.imageArrayIndex = 0;

		if (tvOn) {
			float3 center;
			PanelRect(inGameView, center, panel.size.width, panel.size.height);
			panel.pose.orientation.w = 1.0f;
			panel.pose.position = {center.x, center.y, center.z};
			tvPose = panel.pose;
			tvSize = panel.size;
		} else if (tvHeld) {
			panel.pose = tvPose;
			panel.size = tvSize;
		} else {
			Park(panel);
		}

		layers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&panel);
	}
	if (frameState.shouldRender)
		tvSubmitted = tvOn || tvHeld;

	// the UI panels, each from its own swapchain sized like its window
	// rectangle (recreated when that size changes), so the whole image is
	// used and no sub-image origin convention is involved
	// a panel the pointer can hit must be one the headset shows (a panel left
	// out below, say for want of a swapchain, would stop the pointer in mid-air)
	// without a new desktop frame, a desktop panel shown last frame shows its
	// last image again (as the TV does); canvas panels are always fresh
	bool heldUI[MAX_UI_LAYERS] = {};
	if (frameState.shouldRender) {
		int i = 0;
		for (UIPanelLayer& layer: uiLayers) {
			if (i < MAX_UI_LAYERS)
				heldUI[i++] = !haveFrame && !layer.canvas && layer.drawn && layer.sc.released;
			layer.drawn = false;
		}
	}

	// every panel of the session keeps one place in the layer list, whichever
	// Lua state's set is up: SteamVR shows a place whose swapchain changes
	// stretched to the old swapchain's size, with old content (seen 2026-11-27:
	// the menu's panel came back in games, in the bar's place, and the command
	// panel was cut to the menu pop-up's size). Panels of the other set are
	// parked in their places with their own swapchains.
	int uiCount = 0;
	for (const std::string& slotName: slotNames) {
		if (!frameState.shouldRender || !inGameView || uiCount >= MAX_UI_LAYERS)
			break;

		const auto active = std::find_if(uiLayers.begin(), uiLayers.end(), [&](const UIPanelLayer& l) { return l.name == slotName; });
		if (active == uiLayers.end()) {
			const auto spare = std::find_if(uiSpare.begin(), uiSpare.end(), [&](const auto& sp) { return sp.first == slotName; });
			if (spare == uiSpare.end() || !PrimeSwapchain(spare->second))
				continue;
			XrCompositionLayerQuad& quad = uiQuads[uiCount++];
			quad = {XR_TYPE_COMPOSITION_LAYER_QUAD};
			quad.space = localSpace;
			quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
			quad.subImage.swapchain = spare->second.handle;
			quad.subImage.imageRect.offset = {0, 0};
			quad.subImage.imageRect.extent = {spare->second.sizeX, spare->second.sizeY};
			quad.subImage.imageArrayIndex = 0;
			quad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
			Park(quad);
			layers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad);
			continue;
		}
		UIPanelLayer& layer = *active;

		// made by EnsureUISwapchains, outside the frame's drawing
		if (layer.sc.handle == XR_NULL_HANDLE || layer.sc.sizeX != layer.w || layer.sc.sizeY != layer.h)
			continue;

		// window rows count down from the top; GL rows count up from the bottom
		const size_t index = static_cast<size_t>(&layer - uiLayers.data());
		const bool held = layer.visible && index < MAX_UI_LAYERS && heldUI[index];
		const bool show = held || (layer.visible && (layer.canvas ? canvasReady : resolved) &&
			BlitResolvedTo(layer.sc, layer.x, winY - (layer.y + layer.h), layer.w, layer.h, layer.canvas ? canvasFBO : 0));

		if (!show) {
			if (!PrimeSwapchain(layer.sc))
				continue;
			XrCompositionLayerQuad& quad = uiQuads[uiCount++];
			quad = {XR_TYPE_COMPOSITION_LAYER_QUAD};
			quad.space = localSpace;
			quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
			quad.subImage.swapchain = layer.sc.handle;
			quad.subImage.imageRect.offset = {0, 0};
			quad.subImage.imageRect.extent = {layer.sc.sizeX, layer.sc.sizeY};
			quad.subImage.imageArrayIndex = 0;
			if (layer.canvas)
				quad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
			Park(quad);
			layers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad);
			continue;
		}

		XrCompositionLayerQuad& quad = uiQuads[uiCount++];
		quad = {XR_TYPE_COMPOSITION_LAYER_QUAD};
		quad.space = localSpace;
		quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
		quad.subImage.swapchain = layer.sc.handle;
		quad.subImage.imageRect.offset = {0, 0};
		quad.subImage.imageRect.extent = {layer.sc.sizeX, layer.sc.sizeY};
		quad.subImage.imageArrayIndex = 0;
		// VR UI panels have transparent parts (rounded corners, gaps); RmlUi
		// output is premultiplied, which is what the compositor expects
		if (layer.canvas)
			quad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
		quad.pose = layer.quad.pose;
		quad.size = {layer.quad.width, layer.quad.height};

		layers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad);
		layer.drawn = true;
	}

	// the pointer rays, over every panel
	if (frameState.shouldRender && eyeImages && overlayDrawn) {
		for (uint32_t i = 0; i < NUM_EYES; ++i) {
			overlayViews[i] = projViews[i];
			overlayViews[i].subImage.swapchain = overlay[i].handle;
			overlayViews[i].subImage.imageRect.extent = {overlay[i].sizeX, overlay[i].sizeY};
		}
		overlayLayer.space = localSpace;
		overlayLayer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
		overlayLayer.viewCount = NUM_EYES;
		overlayLayer.views = overlayViews;
		layers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&overlayLayer);
	}

	XrFrameEndInfo frameEndInfo = {XR_TYPE_FRAME_END_INFO};
	frameEndInfo.displayTime = frameState.predictedDisplayTime;
	frameEndInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
	frameEndInfo.layerCount = layerCount;
	frameEndInfo.layers = layers;

	Check(xrEndFrame(session, &frameEndInfo), "xrEndFrame");
}



CVRManager& CVRManager::GetInstance()
{
	static CVRManager instance;
	return instance;
}

CVRManager::CVRManager(): impl(std::make_unique<Impl>()) {}
CVRManager::~CVRManager() = default;


void CVRManager::Init()
{
	if (!configHandler->GetBool("VREnable")) {
		LOG("[VR] disabled (set VREnable=1 to use a headset)");
		return;
	}

	// a hardware cursor is drawn by the OS on top of the window, so it never
	// reaches the back buffer that is mirrored into the headset; force the
	// software cursor for this run only (overlay value, not saved to file)
	if (configHandler->GetBool("HardwareCursor")) {
		configHandler->Set("HardwareCursor", false, true);
		LOG("[VR] using the software cursor while VR is enabled");
	}

	impl->stereo = configHandler->GetBool("VRStereo");
	impl->tableView = (configHandler->GetString("VRView") != "camera");
	impl->debugCaptureFrame = configHandler->GetInt("VRDebugCapture");
	impl->showPanel = configHandler->GetBool("VRPanel");
	impl->pointerEnabled = configHandler->GetBool("VRPointer");
	impl->pointerMargin = configHandler->GetFloat("VRPointerMargin");
	VRState::iconSwitchTan = std::tan(configHandler->GetFloat("VRIconSwitchAngle") * 0.0174533f);
	VRState::iconHalfSizeTan = std::tan(configHandler->GetFloat("VRIconSize") * 0.5f * 0.0174533f);
	impl->tableSize = configHandler->GetFloat("VRTableSize");
	impl->tableDistance = configHandler->GetFloat("VRTableDistance");
	impl->tableHeight = configHandler->GetFloat("VRTableHeight");
	impl->table.size = impl->tableSize;
	impl->table.center = float3(configHandler->GetFloat("VRTableX"), impl->tableHeight, -impl->tableDistance);
	impl->table.yaw = configHandler->GetFloat("VRTableYaw") * (3.14159265f / 180.0f);
	impl->worldScale = configHandler->GetFloat("VRWorldScale");
	impl->mirrorDistance = configHandler->GetFloat("VRMirrorDistance");
	impl->mirrorWidth = configHandler->GetFloat("VRMirrorWidth");
	impl->uiPanelsEnabled = configHandler->GetBool("VRUIPanels");
	impl->barScale = configHandler->GetFloat("VRBarScale");
	impl->barMaxDistance = configHandler->GetFloat("VRBarMaxDistance");
	impl->paletteScale = configHandler->GetFloat("VRPaletteScale");
	impl->paletteRaise = configHandler->GetFloat("VRPaletteRaise");
	impl->sideAngle = configHandler->GetFloat("VRSideAngle") * 0.0174533f;
	impl->sideDistance = configHandler->GetFloat("VRSideDistance");
	impl->sideHeight = configHandler->GetFloat("VRSideHeight");
	impl->sideMaxAngle = std::max(configHandler->GetFloat("VRSideMaxAngle") * 0.0174533f, impl->sideAngle);

	if (impl->CreateInstance() && impl->CreateSession() && impl->CreateSwapchains()) {
		LOG("[VR] OpenXR session created (stereo=%d, view=%s); waiting for the runtime to start it", impl->stereo, impl->tableView ? "table" : "camera");
		return;
	}

	LOG_L(L_WARNING, "[VR] could not start VR; continuing on the desktop only");
	impl->Destroy();
}


void CVRManager::Kill()
{
	VRPointer::Release();
	impl->SaveTable();
	impl->Destroy();
}


void CVRManager::BeginFrame()
{
	if (impl->instance == XR_NULL_HANDLE)
		return;

	impl->PollEvents();

	VRState::sessionRunning = impl->running;
	impl->menuView = VRPointer::InMenuView();
	impl->inLobby = VRPointer::InMenu();
	VRState::menuView = impl->menuView;

	if (!impl->running || impl->frameBegun)
		return;

	impl->eyesDrawn = false;
	impl->overlayDrawn = false;
	impl->viewsValid = false;
	impl->frameState = {XR_TYPE_FRAME_STATE};

	XrFrameWaitInfo frameWaitInfo = {XR_TYPE_FRAME_WAIT_INFO};
	if (!impl->Check(xrWaitFrame(impl->session, &frameWaitInfo, &impl->frameState), "xrWaitFrame"))
		return;

	XrFrameBeginInfo frameBeginInfo = {XR_TYPE_FRAME_BEGIN_INFO};
	if (!impl->Check(xrBeginFrame(impl->session, &frameBeginInfo), "xrBeginFrame"))
		return;

	impl->frameBegun = true;

	if (impl->frameState.shouldRender)
		impl->LocateViews();

	if (impl->inputReady) {
		impl->input.Update(impl->session, impl->localSpace, impl->frameState.predictedDisplayTime);
		impl->UpdateTable();
	}

	impl->UpdateMenu();
}


// the lobby (LuaMenu active): its headset panels, the pointer on them and on
// the TV, and RmlUi (which only the game updates otherwise). The game does
// the same in UpdateDesktop.
void CVRManager::Impl::UpdateMenu()
{
	static bool lobbyPointer = false;

	if (inLobby)
		VRRml::Update();

	if (!inLobby || !tableView || !pointerEnabled || !inputReady) {
		if (menuView) {
			HideUIPanels();
			pointerHit = 0;
		}
		if (lobbyPointer) {
			// leaving the lobby (a game loads): let go of everything held
			VRPointer::Release();
			if (uiHover)
				VRRml::PointerLeave();
			uiButtons[0] = uiButtons[1] = false;
			uiHover = false;
			worldHeld = false;
			lobbyPointer = false;
		}
		return;
	}

	if (!lobbyPointer) {
		// coming from a game: nothing it held carries over
		uiButtons[0] = uiButtons[1] = false;
		uiHover = false;
		worldHeld = false;
		lobbyPointer = true;
	}

	SyncUIPanels();
	EnsureUISwapchains();
	LayoutUIPanels();
	UpdatePointer();
}


void CVRManager::UpdateDesktop()
{
	Impl& m = *impl;

	if (!m.running || !m.tableView || !m.pointerEnabled || !VRPointer::InGame() || m.menuView) {
		m.pointerHit = 0;
		m.HideUIPanels();
		VRPointer::Release();
		return;
	}

	VRPointer::LockDesktopCamera(m.pointerMargin);

	m.SyncUIPanels();
	m.EnsureUISwapchains();

	if (m.inputReady) {
		m.LayoutUIPanels();
		m.UpdatePointer();
	} else {
		m.HideUIPanels();
	}
}


// greybox war room for the table view: a table slab under the map and a floor
// grid, in world coordinates (elmos); toWorld maps OpenXR LOCAL metres
template<typename ToWorld>
static void DrawGreyboxRoom(const CCamera* cam, const ToWorld& toWorld, float scale, float tableHeight, float tableDistance)
{
	const float mapX = static_cast<float>(mapDims.mapx * SQUARE_SIZE);
	const float mapZ = static_cast<float>(mapDims.mapy * SQUARE_SIZE);
	const float margin = 0.03f * scale;
	const float topY = std::min(readMap->GetInitMinHeight(), 0.0f) - 0.002f * scale;
	const float botY = topY - 0.04f * scale;

	const float x0 = -margin, x1 = mapX + margin;
	const float z0 = -margin, z1 = mapZ + margin;

	const SColor topCol(58, 56, 52, 255);
	const SColor sideCol(40, 38, 35, 255);
	const SColor floorCol(28, 29, 31, 255);
	const SColor gridCol(70, 74, 80, 255);

	auto& rb = RenderBuffer::GetTypedRenderBuffer<VA_TYPE_C>();

	// table top and sides
	rb.AddQuadTriangles({{x0, topY, z0}, topCol}, {{x1, topY, z0}, topCol}, {{x1, topY, z1}, topCol}, {{x0, topY, z1}, topCol});
	rb.AddQuadTriangles({{x0, topY, z1}, sideCol}, {{x1, topY, z1}, sideCol}, {{x1, botY, z1}, sideCol}, {{x0, botY, z1}, sideCol});
	rb.AddQuadTriangles({{x0, topY, z0}, sideCol}, {{x1, topY, z0}, sideCol}, {{x1, botY, z0}, sideCol}, {{x0, botY, z0}, sideCol});
	rb.AddQuadTriangles({{x0, topY, z0}, sideCol}, {{x0, topY, z1}, sideCol}, {{x0, botY, z1}, sideCol}, {{x0, botY, z0}, sideCol});
	rb.AddQuadTriangles({{x1, topY, z0}, sideCol}, {{x1, topY, z1}, sideCol}, {{x1, botY, z1}, sideCol}, {{x1, botY, z0}, sideCol});

	// floor 0.75 m below the table top, 8 x 8 m around the starting position
	const float floorXR = tableHeight - 0.75f;
	const float3 f00 = toWorld(-4.0f, floorXR, -4.0f);
	const float3 f11 = toWorld( 4.0f, floorXR,  4.0f);
	rb.AddQuadTriangles({{f00.x, f00.y, f00.z}, floorCol}, {{f11.x, f00.y, f00.z}, floorCol}, {{f11.x, f00.y, f11.z}, floorCol}, {{f00.x, f00.y, f11.z}, floorCol});

	auto& shader = rb.GetShader();

	glMatrixMode(GL_PROJECTION); glPushMatrix(); glLoadMatrixf(cam->GetProjectionMatrix());
	glMatrixMode(GL_MODELVIEW);  glPushMatrix(); glLoadMatrixf(cam->GetViewMatrix());

	glEnable(GL_DEPTH_TEST);
	glDepthMask(GL_TRUE);
	glDisable(GL_BLEND);
	glDisable(GL_CULL_FACE);

	shader.Enable();
	rb.DrawElements(GL_TRIANGLES);

	// floor grid lines every 0.5 m, slightly above the floor
	const float gridY = f00.y + 0.002f * scale;
	for (int i = -8; i <= 8; ++i) {
		const float3 a = toWorld(i * 0.5f, floorXR, -4.0f);
		const float3 b = toWorld(i * 0.5f, floorXR,  4.0f);
		const float3 c = toWorld(-4.0f, floorXR, i * 0.5f);
		const float3 d = toWorld( 4.0f, floorXR, i * 0.5f);
		rb.AddVertex({{a.x, gridY, a.z}, gridCol}); rb.AddVertex({{b.x, gridY, b.z}, gridCol});
		rb.AddVertex({{c.x, gridY, c.z}, gridCol}); rb.AddVertex({{d.x, gridY, d.z}, gridCol});
	}
	rb.DrawArrays(GL_LINES);
	shader.Disable();

	glMatrixMode(GL_PROJECTION); glPopMatrix();
	glMatrixMode(GL_MODELVIEW);  glPopMatrix();
}


// controller visuals for the eye views, in world coordinates (elmos)
struct HandVisual {
	bool gripValid = false;
	float3 gripPos;
	float3 gripAxes[3]; // short x (red), y (green), z (blue) axis lines
	bool held = false;  // gripping the table

	bool aimValid = false;
	float3 rayFrom;
	float3 rayTo;
	bool rayHit = false; // rayTo is on the ground
	float markerSize = 0.0f;
	SColor rayCol = SColor(220, 225, 235, 255);
};

// raysOnly: just the pointer rays and their end markers (the overlay over the UI panels)
static void DrawHands(const float* projMat, const float* viewMat, const HandVisual* hands, int numHands, const std::vector<float3>& selectionOutline, float lift, bool raysOnly = false)
{
	auto& rb = RenderBuffer::GetTypedRenderBuffer<VA_TYPE_C>();

	// the drag-selection box, lying on the ground (closed loop)
	const SColor boxCol(cmdColors.mouseBox);
	for (size_t i = 0, n = selectionOutline.size(); i < n; ++i) {
		rb.AddVertex({selectionOutline[i] + float3(0.0f, lift, 0.0f), boxCol});
		rb.AddVertex({selectionOutline[(i + 1) % n] + float3(0.0f, lift, 0.0f), boxCol});
	}

	for (int h = 0; h < numHands; ++h) {
		const HandVisual& hv = hands[h];

		if (hv.gripValid && !raysOnly) {
			const SColor axisCols[3] = {
				hv.held ? SColor(255, 220, 80, 255) : SColor(230, 70, 60, 255),
				hv.held ? SColor(255, 220, 80, 255) : SColor(80, 220, 90, 255),
				hv.held ? SColor(255, 220, 80, 255) : SColor(80, 120, 240, 255),
			};
			for (int a = 0; a < 3; ++a) {
				rb.AddVertex({hv.gripPos, axisCols[a]});
				rb.AddVertex({hv.gripPos + hv.gripAxes[a], axisCols[a]});
			}
		}

		if (hv.aimValid) {
			const SColor rayCol = hv.rayCol;
			rb.AddVertex({hv.rayFrom, rayCol});
			rb.AddVertex({hv.rayTo, rayCol});

			if (hv.rayHit) {
				const float m = hv.markerSize;
				const float3 p = hv.rayTo;
				rb.AddVertex({p - float3(m, 0.0f, 0.0f), rayCol}); rb.AddVertex({p + float3(m, 0.0f, 0.0f), rayCol});
				rb.AddVertex({p - float3(0.0f, 0.0f, m), rayCol}); rb.AddVertex({p + float3(0.0f, 0.0f, m), rayCol});
				rb.AddVertex({p, rayCol});                         rb.AddVertex({p + float3(0.0f, m * 2.0f, 0.0f), rayCol});
			}
		}
	}

	if (!rb.ShouldSubmit())
		return;

	auto& shader = rb.GetShader();

	glMatrixMode(GL_PROJECTION); glPushMatrix(); glLoadMatrixf(projMat);
	glMatrixMode(GL_MODELVIEW);  glPushMatrix(); glLoadMatrixf(viewMat);

	// always visible: the pointer must show even where terrain is in the way
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_BLEND);

	shader.Enable();
	rb.DrawArrays(GL_LINES);
	shader.Disable();

	glEnable(GL_DEPTH_TEST);

	glMatrixMode(GL_PROJECTION); glPopMatrix();
	glMatrixMode(GL_MODELVIEW);  glPopMatrix();
}


// debug aid: save a rectangle of the default framebuffer's back buffer as a PNG
static void CaptureBackBuffer(const std::string& filename, int x, int y, int sizeX, int sizeY)
{
	sizeX -= (sizeX % 4);
	if (sizeX <= 0 || sizeY <= 0)
		return;

	std::vector<uint8_t> pixels(static_cast<size_t>(sizeX) * sizeY * 4);

	GLint prevReadFBO = 0;
	glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prevReadFBO);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
	glReadPixels(x, y, sizeX, sizeY, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
	glBindFramebuffer(GL_READ_FRAMEBUFFER, prevReadFBO);

	CBitmap bmp(pixels.data(), sizeX, sizeY);
	bmp.ReverseYAxis();
	bmp.Save(filename, true, true);
}


void CVRManager::DrawEyes(const std::function<void()>& updateForCamera, const std::function<void()>& drawWorld)
{
	if (!impl->frameBegun || !impl->frameState.shouldRender || !impl->viewsValid)
		return;

	CCamera* cam = CCameraHandler::GetCamera(CCamera::CAMTYPE_PLAYER);

	const float3 savedPos = cam->GetPos();
	const float savedFov = cam->GetVFOV();

	// Map OpenXR LOCAL space (metres; +x right, +y up, -z forward) to world
	// space (elmos): world = origin + axes(xr - xrOrigin) * scale
	float3 origin;
	float3 xrOrigin;
	float3 axisX; // world direction of XR +x
	float3 axisZ; // world direction of XR +z (towards the player)
	float scale = 1.0f;

	if (impl->tableView) {
		// placement comes from impl->table (moved, turned and zoomed by the controllers)
		const XrToWorld m = TableMapping(impl->table);
		scale = m.scale;
		VRState::eyeElmosPerMetre = scale;
		origin = m.origin;
		xrOrigin = m.xrOrigin;
		axisX = m.axisX;
		axisZ = m.axisZ;
	} else {
		// anchored at the player camera, keeping only its heading so the
		// horizon stays level
		float3 heading = cam->GetForward();
		heading.y = 0.0f;
		if (heading.SqLength() < 1e-4f) {
			// looking straight down; use the camera's up vector as the heading
			heading = cam->GetUp();
			heading.y = 0.0f;
		}
		heading.Normalize();

		scale = impl->worldScale;
		origin = savedPos;
		xrOrigin = float3(0.0f, 0.0f, 0.0f);
		axisX = heading.cross(UpVector).Normalize();
		axisZ = -heading;
	}

	const auto ToWorldDir = [&](const XrVector3f& v) {
		return (axisX * v.x) + (UpVector * v.y) + (axisZ * v.z);
	};
	const auto ToWorldPos = [&](const XrVector3f& p) {
		return origin + ToWorldDir({p.x - xrOrigin.x, p.y - xrOrigin.y, p.z - xrOrigin.z}) * scale;
	};

	// controllers, mapped into the world once per frame (same for both eyes)
	HandVisual handVisuals[CVRInput::NUM_HANDS];
	if (impl->inputReady) {
		for (int h = 0; h < CVRInput::NUM_HANDS; ++h) {
			const VRHandState& hand = impl->input.Hand(h);
			HandVisual& hv = handVisuals[h];

			if ((hv.gripValid = hand.gripValid)) {
				hv.gripPos = ToWorldPos(hand.grip.position);
				hv.gripAxes[0] = ToWorldDir(Rotate(hand.grip.orientation, {0.04f, 0.0f, 0.0f})) * scale;
				hv.gripAxes[1] = ToWorldDir(Rotate(hand.grip.orientation, {0.0f, 0.04f, 0.0f})) * scale;
				hv.gripAxes[2] = ToWorldDir(Rotate(hand.grip.orientation, {0.0f, 0.0f, 0.04f})) * scale;
				hv.held = ((impl->grabMask >> h) & 1) != 0;
			}

			if ((hv.aimValid = hand.aimValid)) {
				const float3 dir = ToWorldDir(Rotate(hand.aim.orientation, {0.0f, 0.0f, -1.0f})).SafeNormalize();
				const float maxLen = POINTER_LENGTH * scale;

				hv.rayFrom = ToWorldPos(hand.aim.position);
				// pointer hand: white, orange while a mouse button is held;
				// other hand: dim grey, cyan while its trigger holds Shift
				if (h == impl->pointerHand) {
					const bool clicking = (impl->triggerRole[h] == Impl::TRIGGER_CLICK) || impl->commandClick[h];
					hv.rayCol = clicking ? SColor(255, 160, 40, 255) : SColor(220, 225, 235, 255);
				} else {
					hv.rayCol = (impl->triggerRole[h] == Impl::TRIGGER_SHIFT) ? SColor(60, 200, 255, 255) : SColor(110, 115, 125, 255);
				}
				hv.markerSize = 0.008f * scale;

				// unsynced heightmap query: rendering only, never game state
				const float hitDist = CGround::LineGroundCol(hv.rayFrom, hv.rayFrom + dir * maxLen, false);

				hv.rayHit = (hitDist >= 0.0f);
				hv.rayTo = hv.rayFrom + dir * (hv.rayHit ? hitDist : (0.4f * scale));

				// the mouse pointer: end the ray where the click goes (a unit, the
				// ground or the desktop panel)
				if (h == impl->pointerHand && impl->pointerHit != 0) {
					hv.rayHit = (impl->pointerHit == 1);
					hv.rayTo = impl->pointerHitPos;
				}
			}
		}
	}

	// the selection box's footprint, traced while the camera is still the desktop one
	std::vector<float3> selectionOutline;
	if (impl->inputReady)
		VRPointer::SelectionFootprint(selectionOutline);

	// both eyes in the world, so per-eye decisions (unit icons) can agree
	for (uint32_t i = 0; i < NUM_EYES; ++i)
		VRState::eyePos[i] = ToWorldPos(impl->views[i].pose.position);

	// keep the desktop camera for screen<->world queries made while the eyes
	// are drawn (the mouse, Spring.TraceScreenRay, area-command previews)
	static CCamera desktopCamera;
	desktopCamera = *cam;
	VRState::desktopCamera = &desktopCamera;

	bool ok = true;

	// the miniature has no fog and no sky: push fog past the far plane and
	// make the clear colour (the sky's fog colour) a dark room
	ISky* sky = ISky::GetSky().get();
	const float savedFogStart = sky->fogStart;
	const float savedFogEnd = sky->fogEnd;
	const float4 savedFogColor = sky->fogColor;
	if (impl->tableView) {
		sky->fogStart = 100.0f;
		sky->fogEnd = 101.0f;
		sky->fogColor = float4(0.07f, 0.075f, 0.08f, 1.0f);
	}

	impl->eyeFrames += 1;
	const bool captureThisFrame = (impl->debugCaptureFrame > 0 && impl->eyeFrames == impl->debugCaptureFrame);
	// log the eyes on the first frame, every ~10 s at 90 Hz, and when capturing
	const bool logThisFrame = (impl->eyeFrames == 1 || (impl->eyeFrames % 900) == 0 || captureThisFrame);

	if (logThisFrame) {
		const XrVector3f& a = impl->views[0].pose.position;
		const XrVector3f& b = impl->views[1].pose.position;
		const float ipd = std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z));
		LOG("[VR] eye frame %d: view=%s scale=%.0f elmos/m, head=(%.2f, %.2f, %.2f) m, eye separation %.1f mm = %.0f elmos",
			impl->eyeFrames, impl->tableView ? "table" : "camera", scale, a.x, a.y, a.z, ipd * 1000.0f, ipd * scale);
	}

	// the overlay layer is submitted only when both eyes got their image
	uint32_t overlayCount = 0;
	impl->overlayDrawn = false;

	for (uint32_t i = 0; i < NUM_EYES && ok; ++i) {
		const XrPosef& pose = impl->views[i].pose;
		const XrFovf& fov = impl->views[i].fov;

		CCamera::VROverride& ovr = cam->vrOverride;
		ovr.active  = true;
		ovr.forward = ToWorldDir(Rotate(pose.orientation, {0.0f, 0.0f, -1.0f})).Normalize();
		ovr.up      = ToWorldDir(Rotate(pose.orientation, {0.0f, 1.0f,  0.0f})).Normalize();
		ovr.right   = ToWorldDir(Rotate(pose.orientation, {1.0f, 0.0f,  0.0f})).Normalize();
		ovr.minViewRange = impl->tableView ? (8.0f * scale) : 0.0f; // see the room around the table
		ovr.tanFov  = float4(std::tan(fov.angleLeft), std::tan(fov.angleRight), std::tan(fov.angleDown), std::tan(fov.angleUp));

		// the culling frustum is symmetric and uses the desktop aspect ratio;
		// pick a vertical fov wide enough to contain the eye's off-axis frustum
		const float tanVert = std::max(std::fabs(ovr.tanFov.z), std::fabs(ovr.tanFov.w));
		const float tanHorz = std::max(std::fabs(ovr.tanFov.x), std::fabs(ovr.tanFov.y));
		const float aspect = std::max(globalRendering->aspectRatio, 0.01f);
		const float tanNeeded = std::max(tanVert, tanHorz / aspect);
		cam->SetVFOV(2.0f * std::atan(tanNeeded) * (180.0f / 3.14159265f));

		cam->SetPos(ToWorldPos(pose.position));
		cam->Update();

		// visibility (unit/feature draw flags, ground patches) is computed in
		// the update step from the active camera; redo it per eye. Fog
		// distances (shader params) also come from the player camera.
		VRState::eyePass = true;
		updateForCamera();
		UniformConstants::GetInstance().UpdateMatrices();
		UniformConstants::GetInstance().UpdateParams();

		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		cam->LoadViewport();

		drawWorld();

		if (impl->tableView) {
			const auto xrToWorld = [&](float x, float y, float z) { return ToWorldPos(XrVector3f{x, y, z}); };
			DrawGreyboxRoom(cam, xrToWorld, scale, impl->tableHeight, impl->tableDistance);
		}
		if (impl->inputReady)
			DrawHands(cam->GetProjectionMatrix(), cam->GetViewMatrix(), handVisuals, CVRInput::NUM_HANDS, selectionOutline, 0.002f * scale);

		if (logThisFrame) {
			const float3 p = cam->GetPos();
			const float3 f = cam->GetForward();
			LOG("[VR] eye %u: pos=(%.0f, %.0f, %.0f) dir=(%.2f, %.2f, %.2f) tanFov=(%.2f, %.2f, %.2f, %.2f) far=%.0f vfov=%.1f viewport=%dx%d",
				i, p.x, p.y, p.z, f.x, f.y, f.z, ovr.tanFov.x, ovr.tanFov.y, ovr.tanFov.z, ovr.tanFov.w,
				cam->GetFarPlaneDist(), cam->GetVFOV(), cam->viewport[2], cam->viewport[3]);
		}
		if (captureThisFrame)
			CaptureBackBuffer("vr-eye-" + std::to_string(i) + ".png", cam->viewport[0], cam->viewport[1], cam->viewport[2], cam->viewport[3]);

		ok = impl->CopyBackBufferTo(impl->eyes[i], cam->viewport[0], cam->viewport[1], cam->viewport[2], cam->viewport[3]);

		// the pointer rays again, on a transparent layer submitted over the UI panels
		if (ok && impl->inputReady && impl->tableView) {
			const std::vector<float3> noOutline;
			overlayCount += impl->DrawOverlay(i, [&]() { DrawHands(cam->GetProjectionMatrix(), cam->GetViewMatrix(), handVisuals, CVRInput::NUM_HANDS, noOutline, 0.0f, true); }) ? 1 : 0;
		}
	}

	impl->overlayDrawn = (overlayCount == NUM_EYES);

	cam->vrOverride.active = false;
	cam->SetPos(savedPos);
	cam->SetVFOV(savedFov);
	cam->Update();

	sky->fogStart = savedFogStart;
	sky->fogEnd = savedFogEnd;
	sky->fogColor = savedFogColor;

	// restore the desktop camera's visibility and params for the desktop draw
	VRState::eyePass = false;
	VRState::desktopCamera = nullptr;
	updateForCamera();
	UniformConstants::GetInstance().UpdateMatrices();
	UniformConstants::GetInstance().UpdateParams();

	if (captureThisFrame)
		LOG("[VR] saved vr-eye-0.png and vr-eye-1.png to the write dir");

	impl->eyesDrawn = ok;
}


// the empty war room for the menu, in OpenXR LOCAL space (metres): the floor
// and its grid and the table slab (where the next game's map will lie), as
// DrawGreyboxRoom shows them in a game
static void DrawMenuRoomGeometry(const float* projMat, const float* viewMat, const float3& tableCenter, float tableYaw, float tableHalf, float floorY)
{
	const SColor topCol(58, 56, 52, 255);
	const SColor sideCol(40, 38, 35, 255);
	const SColor floorCol(28, 29, 31, 255);
	const SColor gridCol(70, 74, 80, 255);

	auto& rb = RenderBuffer::GetTypedRenderBuffer<VA_TYPE_C>();
	auto& shader = rb.GetShader();

	glMatrixMode(GL_PROJECTION); glPushMatrix(); glLoadMatrixf(projMat);
	glMatrixMode(GL_MODELVIEW);  glPushMatrix(); glLoadMatrixf(viewMat);

	// no depth buffer: drawn back to front (floor, grid, table sides, table top)
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_BLEND);
	glDisable(GL_CULL_FACE);

	shader.Enable();

	// floor 8 x 8 m round the starting position
	rb.AddQuadTriangles({{-4.0f, floorY, -4.0f}, floorCol}, {{4.0f, floorY, -4.0f}, floorCol}, {{4.0f, floorY, 4.0f}, floorCol}, {{-4.0f, floorY, 4.0f}, floorCol});
	rb.DrawElements(GL_TRIANGLES);

	// grid lines every 0.5 m
	for (int i = -8; i <= 8; ++i) {
		const float d = i * 0.5f;
		rb.AddVertex({{d, floorY, -4.0f}, gridCol}); rb.AddVertex({{d, floorY, 4.0f}, gridCol});
		rb.AddVertex({{-4.0f, floorY, d}, gridCol}); rb.AddVertex({{4.0f, floorY, d}, gridCol});
	}
	rb.DrawArrays(GL_LINES);

	// the table slab: its top just under sea level of the map that will lie on it
	const float h = tableHalf + 0.03f;
	const float topY = tableCenter.y - 0.002f;
	const float botY = topY - 0.04f;
	const float corners[4][2] = {{-h, -h}, {h, -h}, {h, h}, {-h, h}};
	float3 c[4];
	for (int k = 0; k < 4; ++k)
		c[k] = tableCenter + RotY(float3(corners[k][0], 0.0f, corners[k][1]), tableYaw);

	for (int k = 0; k < 4; ++k) {
		const float3& a = c[k];
		const float3& b = c[(k + 1) % 4];
		rb.AddQuadTriangles({{a.x, topY, a.z}, sideCol}, {{b.x, topY, b.z}, sideCol}, {{b.x, botY, b.z}, sideCol}, {{a.x, botY, a.z}, sideCol});
	}
	rb.AddQuadTriangles({{c[0].x, topY, c[0].z}, topCol}, {{c[1].x, topY, c[1].z}, topCol}, {{c[2].x, topY, c[2].z}, topCol}, {{c[3].x, topY, c[3].z}, topCol});
	rb.DrawElements(GL_TRIANGLES);

	shader.Disable();

	glMatrixMode(GL_PROJECTION); glPopMatrix();
	glMatrixMode(GL_MODELVIEW);  glPopMatrix();
}


void CVRManager::Impl::DrawMenuRoom()
{
	roomDrawn = false;
	overlayDrawn = false;

	// the controllers, in LOCAL space
	HandVisual hands[CVRInput::NUM_HANDS];
	if (inputReady) {
		for (int h = 0; h < CVRInput::NUM_HANDS; ++h) {
			const VRHandState& hand = input.Hand(h);
			HandVisual& hv = hands[h];

			if ((hv.gripValid = hand.gripValid)) {
				hv.gripPos = ToF3(hand.grip.position);
				hv.gripAxes[0] = ToF3(Rotate(hand.grip.orientation, {0.04f, 0.0f, 0.0f}));
				hv.gripAxes[1] = ToF3(Rotate(hand.grip.orientation, {0.0f, 0.04f, 0.0f}));
				hv.gripAxes[2] = ToF3(Rotate(hand.grip.orientation, {0.0f, 0.0f, 0.04f}));
				hv.held = ((grabMask >> h) & 1) != 0;
			}

			if ((hv.aimValid = (hand.aimValid && pointerEnabled))) {
				const float3 dir = ToF3(Rotate(hand.aim.orientation, {0.0f, 0.0f, -1.0f}));
				hv.rayFrom = ToF3(hand.aim.position);
				hv.rayHit = false;

				if (h == pointerHand) {
					const bool clicking = (triggerRole[h] == TRIGGER_CLICK) || commandClick[h];
					hv.rayCol = clicking ? SColor(255, 160, 40, 255) : SColor(220, 225, 235, 255);
					// ends on the panel it points at, otherwise a metre long
					hv.rayTo = (pointerHit == 2) ? pointerHitXR : hv.rayFrom + dir * 1.0f;
				} else {
					hv.rayCol = SColor(110, 115, 125, 255);
					hv.rayTo = hv.rayFrom + dir * 0.4f;
				}
			}
		}
	}

	// GL state the menu's own drawing expects back afterwards
	const GLboolean prevDepth = glIsEnabled(GL_DEPTH_TEST);
	const GLboolean prevBlend = glIsEnabled(GL_BLEND);
	const GLboolean prevCull = glIsEnabled(GL_CULL_FACE);

	const std::vector<float3> noOutline;
	const float roomClear[4] = {0.07f, 0.075f, 0.08f, 1.0f};
	const float tableHalf = MenuTableHalf(table);
	const float floorY = tableHeight - 0.75f;

	bool ok = true;
	uint32_t overlayCount = 0;

	for (uint32_t i = 0; i < NUM_EYES && ok; ++i) {
		const XrPosef& pose = views[i].pose;
		const XrFovf& fov = views[i].fov;

		// off-axis projection from the eye's field of view (column-major)
		constexpr float N = 0.05f;
		constexpr float F = 100.0f;
		const float l = std::tan(fov.angleLeft) * N;
		const float r = std::tan(fov.angleRight) * N;
		const float b = std::tan(fov.angleDown) * N;
		const float t = std::tan(fov.angleUp) * N;
		const float proj[16] = {
			2.0f * N / (r - l), 0.0f, 0.0f, 0.0f,
			0.0f, 2.0f * N / (t - b), 0.0f, 0.0f,
			(r + l) / (r - l), (t + b) / (t - b), -(F + N) / (F - N), -1.0f,
			0.0f, 0.0f, -2.0f * F * N / (F - N), 0.0f,
		};

		// the inverse of the eye's pose (column-major): its axes are the rows
		const float3 ex = ToF3(Rotate(pose.orientation, {1.0f, 0.0f, 0.0f}));
		const float3 ey = ToF3(Rotate(pose.orientation, {0.0f, 1.0f, 0.0f}));
		const float3 ez = ToF3(Rotate(pose.orientation, {0.0f, 0.0f, 1.0f}));
		const float3 ep = ToF3(pose.position);
		const float view[16] = {
			ex.x, ey.x, ez.x, 0.0f,
			ex.y, ey.y, ez.y, 0.0f,
			ex.z, ey.z, ez.z, 0.0f,
			-ex.dot(ep), -ey.dot(ep), -ez.dot(ep), 1.0f,
		};

		ok = DrawInto(eyes[i], roomClear, [&]() {
			DrawMenuRoomGeometry(proj, view, table.center, table.yaw, tableHalf, floorY);
			if (inputReady)
				DrawHands(proj, view, hands, CVRInput::NUM_HANDS, noOutline, 0.0f);
		}, "menu room");

		// the rays again, over the panels
		if (ok && inputReady)
			overlayCount += DrawOverlay(i, [&]() { DrawHands(proj, view, hands, CVRInput::NUM_HANDS, noOutline, 0.0f, true); }) ? 1 : 0;
	}

	if (prevDepth) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
	if (prevBlend) glEnable(GL_BLEND); else glDisable(GL_BLEND);
	if (prevCull) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);

	static bool loggedFirst = false;
	if (ok && !loggedFirst) {
		LOG("[VR] drawing the war room in the menu (table %.2f m across)", tableHalf * 2.0f);
		loggedFirst = true;
	}

	roomDrawn = ok;
	overlayDrawn = (overlayCount == NUM_EYES);
}


void CVRManager::EndOfFrame(bool haveFrame)
{
	if (impl->instance == XR_NULL_HANDLE || !impl->frameBegun)
		return;

	// the lobby draws no world: draw the empty war room here, so the headset
	// shows a room round the TV rather than the TV floating in the dark
	impl->roomDrawn = false;
	if (impl->inLobby && impl->stereo && impl->tableView && !impl->eyesDrawn && impl->frameState.shouldRender && impl->viewsValid)
		impl->DrawMenuRoom();

	impl->SubmitFrame(haveFrame);
	impl->frameBegun = false;
	impl->eyesDrawn = false;
	impl->roomDrawn = false;
}


bool CVRManager::IsSessionRunning() const
{
	return impl->running;
}
