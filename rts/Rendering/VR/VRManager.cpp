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

// after the OpenXR headers: Camera.h defines a `camera` macro that would
// clash with OpenXR struct members of the same name
#include "Game/Camera.h"
#include "Game/CameraHandler.h"
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
CONFIG(float, VRWorldScale).defaultValue(300.0f).minimumValue(1.0f).description("Zero-K VR (camera view): game distance units (elmos) per real-world metre of head movement.");
CONFIG(float, VRMirrorDistance).defaultValue(1.5f).minimumValue(0.3f).description("Zero-K VR: distance in metres from the starting head position to the desktop panel (UI). In-game with the table view, the panel stands behind the table instead.");
CONFIG(int, VRDebugCapture).defaultValue(0).minimumValue(0).description("Zero-K VR: if N > 0, save both eye images (vr-eye-0.png, vr-eye-1.png) to the write dir on the Nth frame that draws the eyes, and log the eye positions.");
CONFIG(bool, VRPanel).defaultValue(true).description("Zero-K VR: show the desktop frame (with the UI) as a floating panel. Turn off to see only the stereo view.");
CONFIG(float, VRMirrorWidth).defaultValue(1.6f).minimumValue(0.1f).description("Zero-K VR: width in metres of the desktop panel (UI).");


namespace {
	constexpr uint32_t NUM_EYES = 2;

	struct ColorSwapchain {
		XrSwapchain handle = XR_NULL_HANDLE;
		std::vector<XrSwapchainImageOpenGLKHR> images;
		int32_t sizeX = 0;
		int32_t sizeY = 0;
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
	GLuint blitFBO = 0;

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
	float mirrorDistance = 1.5f;
	float mirrorWidth = 1.6f;

	bool showPanel = true;
	int debugCaptureFrame = 0; // VRDebugCapture
	int eyeFrames = 0;         // frames on which the eyes were drawn

	bool Check(XrResult result, const char* what) const;

	bool CreateInstance();
	bool CreateSession();
	bool ChooseColorFormat();
	bool CreateColorSwapchain(int32_t sizeX, int32_t sizeY, ColorSwapchain& sc);
	bool CreateSwapchains();
	void Destroy();

	void PollEvents();
	void LocateViews();
	bool CopyBackBufferTo(ColorSwapchain& sc, int srcX, int srcY, int srcSizeX, int srcSizeY);
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

	return Check(xrCreateReferenceSpace(session, &spaceInfo, &localSpace), "xrCreateReferenceSpace");
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

	if (!Check(xrCreateSwapchain(session, &swapchainInfo, &sc.handle), "xrCreateSwapchain"))
		return false;

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

	for (ColorSwapchain* sc: {&mirror, &eyes[0], &eyes[1]}) {
		if (sc->handle != XR_NULL_HANDLE)
			xrDestroySwapchain(sc->handle);

		*sc = ColorSwapchain();
	}

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


bool CVRManager::Impl::CopyBackBufferTo(ColorSwapchain& sc, int srcX, int srcY, int srcSizeX, int srcSizeY)
{
	uint32_t imageIndex = 0;
	XrSwapchainImageAcquireInfo acquireInfo = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
	if (!Check(xrAcquireSwapchainImage(sc.handle, &acquireInfo, &imageIndex), "xrAcquireSwapchainImage"))
		return false;

	XrSwapchainImageWaitInfo waitInfo = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
	waitInfo.timeout = XR_INFINITE_DURATION;
	if (!Check(xrWaitSwapchainImage(sc.handle, &waitInfo), "xrWaitSwapchainImage"))
		return false;

	GLint prevReadFBO = 0;
	GLint prevDrawFBO = 0;
	glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prevReadFBO);
	glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prevDrawFBO);

	const GLboolean prevScissor = glIsEnabled(GL_SCISSOR_TEST);
	const GLboolean prevSRGB = glIsEnabled(GL_FRAMEBUFFER_SRGB);
	glDisable(GL_SCISSOR_TEST);
	glDisable(GL_FRAMEBUFFER_SRGB);

	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, blitFBO);
	glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, sc.images[imageIndex].image, 0);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);

	glBlitFramebuffer(
		srcX, srcY, srcX + srcSizeX, srcY + srcSizeY,
		0, 0, sc.sizeX, sc.sizeY,
		GL_COLOR_BUFFER_BIT, GL_LINEAR
	);

	glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, prevReadFBO);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, prevDrawFBO);

	if (prevScissor)
		glEnable(GL_SCISSOR_TEST);
	if (prevSRGB)
		glEnable(GL_FRAMEBUFFER_SRGB);

	XrSwapchainImageReleaseInfo releaseInfo = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
	return Check(xrReleaseSwapchainImage(sc.handle, &releaseInfo), "xrReleaseSwapchainImage");
}


void CVRManager::Impl::SubmitFrame(bool haveFrame)
{
	XrCompositionLayerProjectionView projViews[NUM_EYES];
	XrCompositionLayerProjection projection = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
	XrCompositionLayerQuad panel = {XR_TYPE_COMPOSITION_LAYER_QUAD};

	const XrCompositionLayerBaseHeader* layers[2];
	uint32_t layerCount = 0;

	if (frameState.shouldRender && eyesDrawn) {
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

	if (frameState.shouldRender && haveFrame && (showPanel || !eyesDrawn) && CopyBackBufferTo(mirror, 0, 0, globalRendering->winSizeX, globalRendering->winSizeY)) {
		panel.space = localSpace;
		panel.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
		panel.subImage.swapchain = mirror.handle;
		panel.subImage.imageRect.offset = {0, 0};
		panel.subImage.imageRect.extent = {mirror.sizeX, mirror.sizeY};
		panel.subImage.imageArrayIndex = 0;

		const float aspect = static_cast<float>(mirror.sizeY) / static_cast<float>(mirror.sizeX);

		panel.pose.orientation.w = 1.0f;
		panel.size = {mirrorWidth, mirrorWidth * aspect};

		if (eyesDrawn && tableView) {
			// in-game: a screen on the war-room wall behind the table, its bottom
			// edge at eye level so the table below stays in clear view
			const float behindTable = tableDistance + tableSize * 0.5f + 0.3f;
			panel.pose.position = {0.0f, panel.size.height * 0.5f, -std::max(behindTable, mirrorDistance)};
		} else {
			panel.pose.position = {0.0f, 0.0f, -mirrorDistance};
		}

		layers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&panel);
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
	impl->tableSize = configHandler->GetFloat("VRTableSize");
	impl->tableDistance = configHandler->GetFloat("VRTableDistance");
	impl->tableHeight = configHandler->GetFloat("VRTableHeight");
	impl->worldScale = configHandler->GetFloat("VRWorldScale");
	impl->mirrorDistance = configHandler->GetFloat("VRMirrorDistance");
	impl->mirrorWidth = configHandler->GetFloat("VRMirrorWidth");

	if (impl->CreateInstance() && impl->CreateSession() && impl->CreateSwapchains()) {
		LOG("[VR] OpenXR session created (stereo=%d, view=%s); waiting for the runtime to start it", impl->stereo, impl->tableView ? "table" : "camera");
		return;
	}

	LOG_L(L_WARNING, "[VR] could not start VR; continuing on the desktop only");
	impl->Destroy();
}


void CVRManager::Kill()
{
	impl->Destroy();
}


void CVRManager::BeginFrame()
{
	if (impl->instance == XR_NULL_HANDLE)
		return;

	impl->PollEvents();

	if (!impl->running || impl->frameBegun)
		return;

	impl->eyesDrawn = false;
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
		// the whole map as a miniature on a table in front of the player:
		// map north points away from the player, sea level is the table top
		const float mapSizeX = static_cast<float>(mapDims.mapx * SQUARE_SIZE);
		const float mapSizeZ = static_cast<float>(mapDims.mapy * SQUARE_SIZE);

		scale = std::max(mapSizeX, mapSizeZ) / impl->tableSize;
		origin = float3(mapSizeX * 0.5f, 0.0f, mapSizeZ * 0.5f);
		xrOrigin = float3(0.0f, impl->tableHeight, -impl->tableDistance);
		axisX = float3(1.0f, 0.0f, 0.0f);
		axisZ = float3(0.0f, 0.0f, 1.0f);
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
	}

	cam->vrOverride.active = false;
	cam->SetPos(savedPos);
	cam->SetVFOV(savedFov);
	cam->Update();

	sky->fogStart = savedFogStart;
	sky->fogEnd = savedFogEnd;
	sky->fogColor = savedFogColor;

	// restore the desktop camera's visibility and params for the desktop draw
	VRState::eyePass = false;
	updateForCamera();
	UniformConstants::GetInstance().UpdateMatrices();
	UniformConstants::GetInstance().UpdateParams();

	if (captureThisFrame)
		LOG("[VR] saved vr-eye-0.png and vr-eye-1.png to the write dir");

	impl->eyesDrawn = ok;
}


void CVRManager::EndOfFrame(bool haveFrame)
{
	if (impl->instance == XR_NULL_HANDLE || !impl->frameBegun)
		return;

	impl->SubmitFrame(haveFrame);
	impl->frameBegun = false;
	impl->eyesDrawn = false;
}


bool CVRManager::IsSessionRunning() const
{
	return impl->running;
}
