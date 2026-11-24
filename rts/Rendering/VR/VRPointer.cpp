/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include "VRPointer.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include <SDL_keyboard.h>
#include <SDL_mouse.h>

#include "Game/Camera.h"
#include "Game/CameraHandler.h"
#include "Game/Game.h"
#include "Game/GameController.h"
#include "Game/TraceRay.h"
#include "Game/UI/KeyBindings.h"
#include "Game/UI/KeyCodes.h"
#include "Game/UI/MouseHandler.h"
#include "Game/UI/ScanCodes.h"
#include "Map/Ground.h"
#include "Map/ReadMap.h"
#include "Rendering/GlobalRendering.h"
#include "Sim/Misc/GlobalConstants.h"
#include "Sim/Units/Unit.h"
#include "System/Input/KeyInput.h"

namespace {
	constexpr int NUM_POINTER_BUTTONS = 2;
	constexpr int SDL_BUTTONS[NUM_POINTER_BUTTONS] = {SDL_BUTTON_LEFT, SDL_BUTTON_RIGHT};

	// after the physical mouse moves, the pointer waits until it moves this
	// many pixels (or a pointer button is pressed) before taking the cursor back
	constexpr int TAKEOVER_PIXELS = 8;

	bool held[NUM_POINTER_BUTTONS] = {false, false};
	int lastX = -1; // window pixel the pointer last moved the cursor to (y from the top)
	int lastY = -1;
	bool yielded = false;
	int anchorX = 0;
	int anchorY = 0;
	bool shiftDown = false; // the pointer is holding Shift
}


bool VRPointer::InGame()
{
	return (game != nullptr && mouse != nullptr && readMap != nullptr);
}


void VRPointer::LockDesktopCamera(float margin)
{
	CCamera* cam = CCameraHandler::GetCamera(CCamera::CAMTYPE_PLAYER);

	const float mapX = static_cast<float>(mapDims.mapx * SQUARE_SIZE);
	const float mapZ = static_cast<float>(mapDims.mapy * SQUARE_SIZE);
	const float aspect = std::max(globalRendering->aspectRatio, 0.1f);
	const float tanHalf = std::tan(cam->GetVFOV() * 0.5f * (3.14159265f / 180.0f));

	// half extents the view must cover, vertically (map z) and horizontally (map x)
	const float halfVert = std::max(mapZ * 0.5f, mapX * 0.5f / aspect) * std::max(margin, 1.0f);
	const float height = halfVert / std::max(tanHalf, 0.01f) + std::max(readMap->GetInitMaxHeight(), 0.0f);

	cam->SetPos(float3(mapX * 0.5f, height, mapZ * 0.5f));
	// a hair off vertical so north is up on screen
	cam->SetDir(float3(0.0f, -1.0f, -0.001f).Normalize());
	cam->Update();
}


bool VRPointer::SelectionFootprint(std::vector<float3>& outline)
{
	outline.clear();

	if (!InGame())
		return false;

	// same conditions as the engine's own box (left drag past the threshold,
	// not chorded, no Lua or map-draw grab)
	float3 bl, br, tl, tr;
	if (!mouse->GetSelectionBoxVertices(bl, br, tl, tr))
		return false;

	const CCamera* cam = CCameraHandler::GetCamera(CCamera::CAMTYPE_PLAYER);
	const CMouseHandler::ButtonPressEvt& bp = mouse->buttons[SDL_BUTTON_LEFT];

	// the box is a rectangle in window pixels; walk its edges and drop each
	// point onto the terrain, so the outline follows hills and valleys
	const float x0 = static_cast<float>(bp.x);
	const float y0 = static_cast<float>(bp.y);
	const float x1 = static_cast<float>(mouse->lastx);
	const float y1 = static_cast<float>(mouse->lasty);
	const float corners[4][2] = {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}};
	constexpr int STEPS = 24; // points per edge

	const float3 camPos = cam->GetPos();
	const float rayLen = cam->GetFarPlaneDist() * 1.4f;

	outline.reserve(4 * STEPS);

	for (int e = 0; e < 4; ++e) {
		const float* a = corners[e];
		const float* b = corners[(e + 1) % 4];

		for (int s = 0; s < STEPS; ++s) {
			const float t = static_cast<float>(s) / STEPS;
			const int px = static_cast<int>(a[0] + (b[0] - a[0]) * t);
			const int py = static_cast<int>(a[1] + (b[1] - a[1]) * t);
			const float3 dir = cam->CalcPixelDir(px, py);

			// unsynced heightmap query: rendering only
			const float dist = CGround::LineGroundCol(camPos, camPos + dir * rayLen, false);

			if (dist >= 0.0f) {
				outline.push_back(camPos + dir * dist);
			} else if (dir.y < -1e-4f) {
				// off the map: use the sea-level plane
				outline.push_back(camPos + dir * (-camPos.y / dir.y));
			}
		}
	}

	return (outline.size() >= 3);
}


void VRPointer::SetShift(bool down)
{
	if (down == shiftDown)
		return;

	shiftDown = down;

	// the same path as debug.emulateKeyPress/Release: the emulated key is merged
	// with the real keyboard in KeyInput::Update, so GetKeyModState, Lua's
	// GetModKeyState and the queue-order logic all see Shift held
	constexpr SDL_Keycode key = SDLK_LSHIFT;
	const SDL_Scancode sc = SDL_GetScancodeFromKey(key);

	int numKeys = 0;
	const uint8_t* kbState = SDL_GetKeyboardState(&numKeys);
	const bool physicalDown = (kbState != nullptr && static_cast<int>(sc) < numKeys && kbState[sc] != 0);

	KeyInput::SetKeyEmulated(key, down);
	KeyInput::Update(keyBindings.GetFakeMetaKey());

	// press and release events, unless the real Shift key is already down
	if (activeController == nullptr || physicalDown)
		return;

	const int keyCode = CKeyCodes::GetNormalizedSymbol(key);
	const int scanCode = CScanCodes::GetNormalizedSymbol(sc);

	if (down) {
		activeController->KeyPressed(keyCode, scanCode, false);
	} else {
		activeController->KeyReleased(keyCode, scanCode);
	}
}


void VRPointer::Release()
{
	SetShift(false);

	if (mouse == nullptr) {
		std::fill(std::begin(held), std::end(held), false);
		return;
	}

	for (int b = 0; b < NUM_POINTER_BUTTONS; ++b) {
		if (!held[b])
			continue;
		mouse->MouseRelease(lastX, lastY, SDL_BUTTONS[b]);
		held[b] = false;
	}
}


VRPointer::Result VRPointer::Update(const Input& in)
{
	Result result;

	if (!InGame()) {
		Release();
		return result;
	}

	const int offsetY = globalRendering->viewWindowOffsetY;

	// where the pointer is, as a window pixel
	bool haveTarget = false;
	int px = 0;
	int py = 0;

	if (in.onPanel) {
		haveTarget = true;
		px = in.panelX;
		py = in.panelY;
	} else if (in.rayValid) {
		const CUnit* hitUnit = nullptr;
		const CFeature* hitFeature = nullptr;
		const float dist = TraceRay::GuiTraceRay(in.from, in.dir, in.length, nullptr, hitUnit, hitFeature, true);

		if (dist >= 0.0f) {
			result.worldHit = true;
			result.pos = in.from + in.dir * dist;

			// aim at the middle of a unit so the click from the overhead
			// desktop camera lands on it too
			const float3 target = (hitUnit != nullptr) ? hitUnit->drawMidPos : result.pos;
			const CCamera* cam = CCameraHandler::GetCamera(CCamera::CAMTYPE_PLAYER);
			const float3 vp = cam->CalcViewPortCoordinates(target);

			const int vx = std::clamp(static_cast<int>(vp.x), 0, globalRendering->viewSizeX - 1);
			const int vy = std::clamp(static_cast<int>(vp.y), 0, globalRendering->viewSizeY - 1);

			haveTarget = true;
			px = globalRendering->viewPosX + vx;
			py = (globalRendering->viewSizeY - 1 - vy) + offsetY;
		}
	}

	const bool anyHeld = held[0] || held[1];
	const bool wants[NUM_POINTER_BUTTONS] = {in.left, in.right};
	const bool newPress = (wants[0] && !held[0]) || (wants[1] && !held[1]);

	// the physical mouse moved the cursor: leave it alone until the pointer
	// clearly moves or clicks, so the desktop mouse still works next to it
	if (!anyHeld && lastX >= 0 && (mouse->lastx != lastX || (mouse->lasty + offsetY) != lastY)) {
		if (!yielded) {
			yielded = true;
			anchorX = px;
			anchorY = py;
		}
		lastX = mouse->lastx;
		lastY = mouse->lasty + offsetY;
	}
	if (yielded && haveTarget && (newPress || std::abs(px - anchorX) > TAKEOVER_PIXELS || std::abs(py - anchorY) > TAKEOVER_PIXELS))
		yielded = false;

	if (haveTarget && !yielded && (px != lastX || py != lastY)) {
		mouse->MouseMove(px, py, (lastX >= 0) ? (px - lastX) : 0, (lastY >= 0) ? (py - lastY) : 0);
		lastX = px;
		lastY = py;
	}

	for (int b = 0; b < NUM_POINTER_BUTTONS; ++b) {
		if (wants[b] && !held[b]) {
			// presses need a target; releases always happen, at the last position
			if (haveTarget && !yielded) {
				mouse->MousePress(lastX, lastY, SDL_BUTTONS[b]);
				held[b] = true;
			}
		} else if (!wants[b] && held[b]) {
			mouse->MouseRelease(lastX, lastY, SDL_BUTTONS[b]);
			held[b] = false;
		}
	}

	return result;
}
