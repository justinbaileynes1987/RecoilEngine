/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include "VRManager.h"

#include <algorithm>
#include <cstring>
#include <vector>

#include "Rendering/GL/myGL.h"
#include "Rendering/GlobalRendering.h"
#include "System/Config/ConfigHandler.h"
#include "System/Log/ILog.h"

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

CONFIG(bool, VREnable).defaultValue(false).description("Zero-K VR: start an OpenXR session and show the game in a connected headset.");
CONFIG(float, VRMirrorDistance).defaultValue(1.5f).minimumValue(0.3f).description("Zero-K VR: distance in metres from the starting head position to the mirrored game screen.");
CONFIG(float, VRMirrorWidth).defaultValue(1.6f).minimumValue(0.1f).description("Zero-K VR: width in metres of the mirrored game screen.");


struct CVRManager::Impl
{
	XrInstance instance = XR_NULL_HANDLE;
	XrSystemId systemId = XR_NULL_SYSTEM_ID;
	XrSession session = XR_NULL_HANDLE;
	XrSpace localSpace = XR_NULL_HANDLE;
	XrSwapchain swapchain = XR_NULL_HANDLE;
	std::vector<XrSwapchainImageOpenGLKHR> images;
	int32_t swapSizeX = 0;
	int32_t swapSizeY = 0;
	GLuint blitFBO = 0;

	XrSessionState state = XR_SESSION_STATE_UNKNOWN;
	bool running = false;

	float mirrorDistance = 1.5f;
	float mirrorWidth = 1.6f;

	bool Check(XrResult result, const char* what) const;

	bool CreateInstance();
	bool CreateSession();
	bool CreateSwapchain();
	void Destroy();

	void PollEvents();
	void SubmitMirrorFrame();
	bool CopyBackBufferToSwapchain();
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


bool CVRManager::Impl::CreateSwapchain()
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

	// the desktop back buffer already holds sRGB-encoded bytes, so an sRGB
	// swapchain (filled by a raw copy) makes the runtime display them correctly
	int64_t format = formats[0];
	for (const int64_t preferred: {static_cast<int64_t>(GL_SRGB8_ALPHA8), static_cast<int64_t>(GL_RGBA8)}) {
		if (std::find(formats.begin(), formats.end(), preferred) != formats.end()) {
			format = preferred;
			break;
		}
	}

	swapSizeX = globalRendering->winSizeX;
	swapSizeY = globalRendering->winSizeY;

	XrSwapchainCreateInfo swapchainInfo = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
	swapchainInfo.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
	swapchainInfo.format = format;
	swapchainInfo.sampleCount = 1;
	swapchainInfo.width = static_cast<uint32_t>(swapSizeX);
	swapchainInfo.height = static_cast<uint32_t>(swapSizeY);
	swapchainInfo.faceCount = 1;
	swapchainInfo.arraySize = 1;
	swapchainInfo.mipCount = 1;

	if (!Check(xrCreateSwapchain(session, &swapchainInfo, &swapchain), "xrCreateSwapchain"))
		return false;

	uint32_t imageCount = 0;
	if (!Check(xrEnumerateSwapchainImages(swapchain, 0, &imageCount, nullptr), "xrEnumerateSwapchainImages"))
		return false;

	images.assign(imageCount, {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR});
	if (!Check(xrEnumerateSwapchainImages(swapchain, imageCount, &imageCount, reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data())), "xrEnumerateSwapchainImages"))
		return false;

	glGenFramebuffers(1, &blitFBO);

	LOG("[VR] mirror swapchain %dx%d, format 0x%x, %u images", swapSizeX, swapSizeY, static_cast<unsigned>(format), imageCount);
	return true;
}


void CVRManager::Impl::Destroy()
{
	if (blitFBO != 0) {
		glDeleteFramebuffers(1, &blitFBO);
		blitFBO = 0;
	}

	images.clear();

	if (swapchain != XR_NULL_HANDLE) {
		xrDestroySwapchain(swapchain);
		swapchain = XR_NULL_HANDLE;
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


bool CVRManager::Impl::CopyBackBufferToSwapchain()
{
	uint32_t imageIndex = 0;
	XrSwapchainImageAcquireInfo acquireInfo = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
	if (!Check(xrAcquireSwapchainImage(swapchain, &acquireInfo, &imageIndex), "xrAcquireSwapchainImage"))
		return false;

	XrSwapchainImageWaitInfo waitInfo = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
	waitInfo.timeout = XR_INFINITE_DURATION;
	if (!Check(xrWaitSwapchainImage(swapchain, &waitInfo), "xrWaitSwapchainImage"))
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
	glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, images[imageIndex].image, 0);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);

	glBlitFramebuffer(
		0, 0, globalRendering->winSizeX, globalRendering->winSizeY,
		0, 0, swapSizeX, swapSizeY,
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
	return Check(xrReleaseSwapchainImage(swapchain, &releaseInfo), "xrReleaseSwapchainImage");
}


void CVRManager::Impl::SubmitMirrorFrame()
{
	XrFrameWaitInfo frameWaitInfo = {XR_TYPE_FRAME_WAIT_INFO};
	XrFrameState frameState = {XR_TYPE_FRAME_STATE};
	if (!Check(xrWaitFrame(session, &frameWaitInfo, &frameState), "xrWaitFrame"))
		return;

	XrFrameBeginInfo frameBeginInfo = {XR_TYPE_FRAME_BEGIN_INFO};
	if (!Check(xrBeginFrame(session, &frameBeginInfo), "xrBeginFrame"))
		return;

	XrCompositionLayerQuad quad = {XR_TYPE_COMPOSITION_LAYER_QUAD};
	const XrCompositionLayerBaseHeader* layers[] = {reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad)};
	uint32_t layerCount = 0;

	if (frameState.shouldRender && CopyBackBufferToSwapchain()) {
		quad.space = localSpace;
		quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
		quad.subImage.swapchain = swapchain;
		quad.subImage.imageRect.offset = {0, 0};
		quad.subImage.imageRect.extent = {swapSizeX, swapSizeY};
		quad.subImage.imageArrayIndex = 0;
		quad.pose.orientation.w = 1.0f;
		quad.pose.position = {0.0f, 0.0f, -mirrorDistance};
		quad.size = {mirrorWidth, mirrorWidth * static_cast<float>(swapSizeY) / static_cast<float>(swapSizeX)};
		layerCount = 1;
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

	impl->mirrorDistance = configHandler->GetFloat("VRMirrorDistance");
	impl->mirrorWidth = configHandler->GetFloat("VRMirrorWidth");

	if (impl->CreateInstance() && impl->CreateSession() && impl->CreateSwapchain()) {
		LOG("[VR] OpenXR session created; waiting for the runtime to start it");
		return;
	}

	LOG_L(L_WARNING, "[VR] could not start VR; continuing on the desktop only");
	impl->Destroy();
}


void CVRManager::Kill()
{
	impl->Destroy();
}


void CVRManager::EndOfFrame(bool haveFrame)
{
	if (impl->instance == XR_NULL_HANDLE)
		return;

	impl->PollEvents();

	if (impl->running && haveFrame)
		impl->SubmitMirrorFrame();
}


bool CVRManager::IsSessionRunning() const
{
	return impl->running;
}
