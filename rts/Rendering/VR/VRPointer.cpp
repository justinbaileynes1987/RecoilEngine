/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include "VRPointer.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include <SDL_mouse.h>

#include "Game/Camera.h"
#include "Game/CameraHandler.h"
#include "Game/Game.h"
#include "Game/TraceRay.h"
#include "Game/UI/MouseHandler.h"
#include "Map/ReadMap.h"
#include "Rendering/GlobalRendering.h"
#include "Sim/Misc/GlobalConstants.h"
#include "Sim/Units/Unit.h"

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


void VRPointer::Release()
{
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
