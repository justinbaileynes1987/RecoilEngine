/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */


#include "TraceRay.h"
#include "Camera.h"
#include "GlobalUnsynced.h"
#include "Map/Ground.h"
#include "Rendering/GlobalRendering.h"
#include "Rendering/IconHandler.h"
#include "Rendering/VR/VRState.h"
#include "Sim/Features/Feature.h"
#include "Sim/Features/FeatureHandler.h"
#include "Sim/Misc/CollisionHandler.h"
#include "Sim/Misc/CollisionVolume.h"
#include "Sim/Misc/GeometricObjects.h"
#include "Sim/Misc/GlobalConstants.h"
#include "Sim/Misc/LosHandler.h"
#include "Sim/Misc/QuadField.h"
#include "Sim/Misc/TeamHandler.h"
#include "Sim/Units/UnitDef.h"
#include "Sim/Units/UnitHandler.h"
#include "Sim/Units/UnitTypes/Factory.h"
#include "Sim/Weapons/PlasmaRepulser.h"
#include "Sim/Weapons/WeaponDef.h"
#include "System/GlobalConfig.h"
#include "System/SpringMath.h"

#include <algorithm>
#include <vector>

#include "System/Misc/TracyDefs.h"

//////////////////////////////////////////////////////////////////////
// Local/Helper functions
//////////////////////////////////////////////////////////////////////

/**
 * helper for TestCone
 * @return true if object <o> is in the firing cone, false otherwise
 */
inline static bool TestConeHelper(
	const float3& tstPos,
	const float3& tstDir,
	const float length,
	const float spread,
	const CSolidObject* obj
) {
	const CollisionVolume* cv = &obj->collisionVolume;

	const float3 cvRelVec = cv->GetWorldSpacePos(obj) - tstPos;

	const float  cvRelDst = std::clamp(cvRelVec.dot(tstDir), 0.0f, length);
	const float  coneSize = cvRelDst * spread + 1.0f;

	// theoretical impact position assuming no spread
	const float3 hitVec = tstDir * cvRelDst;
	const float3 hitPos = tstPos + hitVec;

	bool ret = false;

	if (obj->GetBlockingMapID() < unitHandler.MaxUnits()) {
		// obj is a unit
		ret = ret || ((cv->GetPointSurfaceDistance(static_cast<const CUnit*>(obj), nullptr, tstPos) - coneSize) <= 0.0f);
		ret = ret || ((cv->GetPointSurfaceDistance(static_cast<const CUnit*>(obj), nullptr, hitPos) - coneSize) <= 0.0f);
	} else {
		// obj is a feature
		ret = ret || ((cv->GetPointSurfaceDistance(static_cast<const CFeature*>(obj), nullptr, tstPos) - coneSize) <= 0.0f);
		ret = ret || ((cv->GetPointSurfaceDistance(static_cast<const CFeature*>(obj), nullptr, hitPos) - coneSize) <= 0.0f);
	}

	if (globalRendering->drawDebugTraceRay) {
		#define go geometricObjects

		if (ret) {
			go->SetColor(go->AddLine(hitPos - (UpVector * hitPos.dot(UpVector)), hitPos, 3, 1, GAME_SPEED), 1.0f, 0.0f, 0.0f, 1.0f);
		} else {
			go->SetColor(go->AddLine(hitPos - (UpVector * hitPos.dot(UpVector)), hitPos, 3, 1, GAME_SPEED), 0.0f, 1.0f, 0.0f, 1.0f);
		}

		#undef go
	}

	return ret;
}

/**
 * helper for TestTrajectoryCone
 * @return true if object <o> is in the firing trajectory, false otherwise
 */
inline static bool TestTrajectoryConeHelper(
	const float3& tstPos,
	const float3& tstDir, // 2D
	float length,
	float linear,
	float quadratic,
	float spread,
	float baseSize,
	const CSolidObject* obj
) {
	// trajectory is a parabola f(x)=a*x*x + b*x with
	// parameters a = quadratic, b = linear, and c = 0
	// (x = objDst1D, negative values represent objects
	// "behind" the testee whose collision volumes might
	// still be intersected by its trajectory arc)
	//
	// firing-cone is centered along tstDir with radius
	// <x * spread + baseSize> (usually baseSize != 0
	// so weapons with spread = 0 will test against a
	// cylinder, not an infinitely thin line as safety
	// measure against friendly-fire damage in tightly
	// packed unit groups)
	// 
	// baseSize is usually hardcoded to 0 in the functions calling this 
	//
	// return true iff the world-space point <x, f(x)>
	// lies on or inside the object's collision volume
	// (where 'x' is actually the projected xz-distance
	// to the object's colvol-center along tstDir)
	//
	// !NOTE!:
	//   THE TRAJECTORY CURVE MIGHT STILL INTERSECT
	//   EVEN WHEN <x, f(x)> DOES NOT LIE INSIDE CV
	//   SO THIS CAN GENERATE FALSE NEGATIVES
	//   Chord checks should solve this problem. 
	const CollisionVolume* cv = &obj->collisionVolume;

	const float3 cvRelVec = cv->GetWorldSpacePos(obj) - tstPos;

	const float  cvRelDst = std::clamp(cvRelVec.dot(tstDir), 0.0f, length);
	const float  coneSize = cvRelDst * spread + baseSize;

	// theoretical impact position assuming no spread
	// note that unlike TestConeHelper these positions
	// lie along curve f(x) here, not a straight line
	// (if object-distance is 0, tstPos == hitPos)
	const float3 hitVec = tstDir * cvRelDst;
	const float3 hitPos = (tstPos + hitVec) + (UpVector * (quadratic * cvRelDst * cvRelDst + linear * cvRelDst));
	float3 endPos = 0.f; 
	bool ret = false;

	CollisionQuery cq;
	// chord check to hitPos
	const CMatrix44f objTransform = obj->GetTransformMatrix(true);

	// use heuristic to choose which chord to check
	// if projectile is traveling upwards, check from muzzle to position
	// if projectile is traveling downwards, check from position to target
	if ((2 * quadratic * cvRelDst + linear) > 0) {
		if (CCollisionHandler::DetectHit(obj, objTransform, tstPos, hitPos, &cq, true)) {
			ret = true;
		}
	}
	else {
		//only compute endPos if needed
		endPos = (tstPos + tstDir * length) + (UpVector * (quadratic * length * length + linear * length));
		if (CCollisionHandler::DetectHit(obj, objTransform, hitPos, endPos, &cq, true)) {
			ret = true;
		}
	}

	if (globalRendering->drawDebugTraceRay) {

		#define go geometricObjects

		if (ret) {
			// red line pointing to hitPos
			go->SetColor(go->AddLine(tstPos + hitVec, hitPos, 3, 0, GAME_SPEED), 1.0f, 0.0f, 0.0f, 1.0f);
			// blue lines showing chord checks
			if ((2 * quadratic * cvRelDst + linear) > 0) {
				go->SetColor(go->AddLine(tstPos, hitPos, 3, 0, GAME_SPEED), 0.0f, 0.0f, 1.0f, 1.0f);
			}
			else {
				go->SetColor(go->AddLine(hitPos, endPos, 3, 0, GAME_SPEED), 0.0f, 0.0f, 1.0f, 1.0f);
			}
			// While using this debug, on SlowUpdate Frames, CWeapon::TryTargetHeading will assume the unit chassis rotates to face the target, 
			// so the tstPos will not be lined up with any part of the unit. 
			// resulting in two visible debug lines. One from the weaponmuzzle, one from the rotated weaponmuzzle
		} else {
			// green line pointing to hitPos
			go->SetColor(go->AddLine(tstPos + hitVec, hitPos, 3, 0, GAME_SPEED), 0.0f, 1.0f, 0.0f, 1.0f);
		}

		#undef go
	}

	return ret;
}



//////////////////////////////////////////////////////////////////////
// Raytracing
//////////////////////////////////////////////////////////////////////

namespace TraceRay {

// called by {CRifle, CBeamLaser, CLightningCannon}::Fire(), CWeapon::HaveFreeLineOfFire(), and Skirmish AIs
float TraceRay(const float3& p, const float3& d, float l, int f, const CUnit* o, CUnit*& hu, CFeature*& hf, CollisionQuery* cq)
{
	assert(o != nullptr);
	return (TraceRay(p, d, l, f, o->allyteam, o, hu, hf, cq));
}

float TraceRay(
	const float3& pos,
	const float3& dir,
	float traceLength,
	int traceFlags,
	int allyTeam,
	const CUnit* owner,
	CUnit*& hitUnit,
	CFeature*& hitFeature,
	CollisionQuery* hitColQuery
) {
	RECOIL_DETAILED_TRACY_ZONE;
	// NOTE:
	//   the bits here and in Test*Cone are interpreted as "do not scan for {enemy,friendly,...}
	//   objects in quads" rather than "return false if ray hits an {enemy,friendly,...} object"
	//   consequently a weapon with (e.g.) avoidFriendly=true that wants to check whether it has
	//   a free line of fire should *not* set the NOFRIENDLIES bit in its trace-flags, etc
	const bool scanForEnemies  = ((traceFlags & Collision::NOENEMIES   ) == 0);
	const bool scanForAllies   = ((traceFlags & Collision::NOFRIENDLIES) == 0);
	const bool scanForFeatures = ((traceFlags & Collision::NOFEATURES  ) == 0);
	const bool scanForNeutrals = ((traceFlags & Collision::NONEUTRALS  ) == 0);
	const bool scanForGround   = ((traceFlags & Collision::NOGROUND    ) == 0);
	const bool scanForCloaked  = ((traceFlags & Collision::NOCLOAKED   ) == 0);

	const bool scanForAnyUnits = scanForEnemies || scanForAllies || scanForNeutrals || scanForCloaked;

	hitFeature = nullptr;
	hitUnit = nullptr;

	if (dir == ZeroVector)
		return -1.0f;

	if (scanForFeatures || scanForAnyUnits) {
		CollisionQuery cq;

		QuadFieldQuery qfQuery;
		quadField.GetQuadsOnRay(qfQuery, pos, dir, traceLength);

		// locally point somewhere non-NULL; we cannot pass hitColQuery
		// to DetectHit directly because each call resets it internally
		if (hitColQuery == nullptr)
			hitColQuery = &cq;

		// feature intersection
		if (scanForFeatures) {
			for (const int quadIdx: *qfQuery.quads) {
				const CQuadField::Quad& quad = quadField.GetQuad(quadIdx);

				for (CFeature* f: quad.features) {
					// NOTE:
					//   if f is non-blocking, ProjectileHandler will not test
					//   for collisions with projectiles so we can skip it here
					if (!f->HasCollidableStateBit(CSolidObject::CSTATE_BIT_QUADMAPRAYS))
						continue;

					if (CCollisionHandler::DetectHit(f, f->GetTransformMatrix(true), pos, pos + dir * traceLength, &cq, true)) {
						const float len = cq.GetHitPosDist(pos, dir);

						// we want the closest feature (intersection point) on the ray
						if (len >= traceLength)
							continue;

						traceLength = len;

						hitFeature = f;
						*hitColQuery = cq;
					}
				}
			}
		}

		// unit intersection
		if (scanForAnyUnits) {
			for (const int quadIdx: *qfQuery.quads) {
				const CQuadField::Quad& quad = quadField.GetQuad(quadIdx);

				for (CUnit* u: quad.units) {
					if (u == owner)
						continue;

					if (!u->HasCollidableStateBit(CSolidObject::CSTATE_BIT_QUADMAPRAYS))
						continue;

					bool doHitTest = false;

					doHitTest |= (scanForAllies   && u->allyteam == owner->allyteam);
					doHitTest |= (scanForEnemies  && u->allyteam != owner->allyteam);
					doHitTest |= (scanForNeutrals && u->IsNeutral());
					doHitTest |= (scanForCloaked  && u->IsCloaked());

					if (!doHitTest)
						continue;

					if (CCollisionHandler::DetectHit(u, u->GetTransformMatrix(true), pos, pos + dir * traceLength, &cq, true)) {
						const float len = cq.GetHitPosDist(pos, dir);

						// we want the closest unit (intersection point) on the ray
						if (len >= traceLength)
							continue;

						traceLength = len;

						hitUnit = u;
						*hitColQuery = cq;
					}
				}
			}

			// units override features, so feature != null implies no unit was hit
			if (hitUnit != nullptr)
				hitFeature = nullptr;

		}
	}

	if (scanForGround) {
		// ground intersection
		const float groundLength = CGround::LineGroundCol(pos, pos + dir * traceLength);

		if (traceLength > groundLength && groundLength > 0.0f) {
			traceLength = groundLength;

			hitUnit = nullptr;
			hitFeature = nullptr;
		}
	}

	// no intersection if no decrease in length
	return traceLength;
}


void TraceRayShields(
	const CWeapon* emitter,
	const float3& start,
	const float3& dir,
	float length,
	std::vector<SShieldDist>& hitShields
) {
	RECOIL_DETAILED_TRACY_ZONE;
	CollisionQuery cq;

	QuadFieldQuery qfQuery;
	quadField.GetQuadsOnRay(qfQuery, start, dir, length);

	for (const int quadIdx: *qfQuery.quads) {
		const CQuadField::Quad& quad = quadField.GetQuad(quadIdx);

		for (CPlasmaRepulser* r: quad.repulsers) {
			if (!r->CanIntercept(emitter->weaponDef->interceptedByShieldType, emitter->owner->allyteam))
				continue;

			if (CCollisionHandler::DetectHit(r->owner, &r->collisionVolume, r->owner->GetTransformMatrix(true), start, start + dir * length, &cq, true)) {
				if (cq.InsideHit() && r->weaponDef->exteriorShield)
					continue;

				const float len = cq.GetHitPosDist(start, dir);

				if (len <= 0.0f)
					continue;

				const auto hitCmp = [](const float a, const SShieldDist& b) { return (a < b.dist); };
				const auto insPos = std::upper_bound(hitShields.begin(), hitShields.end(), len, hitCmp);

				hitShields.insert(insPos, {r, len});
			}
		}
	}
}


// Zero-K VR: is the unit drawn as an icon in the headset, and how big? Mirrors
// UpdateUnitIconStateVR (UnitDrawerData.cpp) and the eye-pass icon size in
// CUnitDrawerGLSL::DrawUnitIcon. Unsynced: only used for GUI picking.
static bool VRPickIcon(const CUnit* u, float& iconRadius)
{
	if (u->currentIconIndex == icon::INVALID_ICON_INDEX || u->noDraw || u->IsInVoid())
		return false;

	const unsigned short losStatus = u->losStatus[gu->myAllyTeam];

	const float dist = VRState::IconSwitchDistance(u->GetObjDrawMidPos());
	const auto& iconData = icon::iconHandler.GetIconData(u->currentIconIndex);

	iconRadius = iconData.GetSize() * dist * VRState::iconHalfSizeTan;
	if (iconData.GetRadiusAdjust() && u->currentIconIndex != icon::iconHandler.GetDefaultIconIdx())
		iconRadius *= (u->radius / iconData.GetRadiusScale());

	if ((losStatus & LOS_INLOS) == 0 && !gu->spectatingFullView)
		return ((losStatus & LOS_INRADAR) != 0);

	return (u->radius < dist * VRState::iconSwitchTan);
}

// Zero-K VR: a GUI trace along the cursor ray while the controller pointer
// drives the cursor; answers with what the pointer picked (see VRState::CursorPick)
static float VRCursorTrace(
	const float3& start,
	const float3& dir,
	const float length,
	const CUnit*& hitUnit,
	const CFeature*& hitFeature,
	bool useRadar,
	bool ignoreWater
) {
	const VRState::CursorPick& pick = VRState::cursorPick;

	const CUnit* unit = (pick.unitID >= 0) ? unitHandler.GetUnit(pick.unitID) : nullptr;
	const CFeature* feature = (pick.featureID >= 0) ? featureHandler.GetFeature(pick.featureID) : nullptr;

	// the pick sees radar blips; callers that don't want them get the ground
	if (unit != nullptr && !useRadar && !gu->spectatingFullView) {
		const bool enemy = !teamHandler.Ally(unit->allyteam, gu->myAllyTeam);
		const bool inSight = (unit->losStatus[gu->myAllyTeam] & (LOS_INLOS | LOS_CONTRADAR)) != 0;
		if (enemy && !inSight)
			unit = nullptr;
	}

	// the cursor pixel is the pointer's hit point seen from the desktop camera,
	// so the hit lies on this ray
	const float dist = (pick.pos - start).dot(dir);

	if ((unit != nullptr || feature != nullptr) && dist > 0.0f && dist <= length) {
		hitUnit = unit;
		hitFeature = (unit != nullptr) ? nullptr : feature;
		return dist;
	}

	// nothing picked: the ground (or water) under the cursor
	const CUnit* groundUnit = nullptr;
	const CFeature* groundFeature = nullptr;
	return GuiTraceRay(start, dir, length, nullptr, groundUnit, groundFeature, useRadar, true, ignoreWater);
}


float GuiTraceRay(
	const float3& start,
	const float3& dir,
	const float length,
	const CUnit* exclude,
	const CUnit*& hitUnit,
	const CFeature*& hitFeature,
	bool useRadar,
	bool groundOnly,
	bool ignoreWater
) {
	RECOIL_DETAILED_TRACY_ZONE;
	hitUnit = nullptr;
	hitFeature = nullptr;

	if (dir == ZeroVector)
		return -1.0f;

	// Zero-K VR: the cursor ray while the controller pointer drives the cursor
	if (VRState::cursorPick.active && !VRState::pickFromEye && !groundOnly && exclude == nullptr &&
	    start.SqDistance(VRState::cursorPick.camPos) < 1.0f && dir.dot(VRState::cursorPick.camDir) > 0.9999999f)
		return VRCursorTrace(start, dir, length, hitUnit, hitFeature, useRadar, ignoreWater);

	// ground and water-plane intersection
	const float    guiRayLength = length;
	const float groundRayLength = CGround::LineGroundCol(start, dir, guiRayLength, false);
	const float  waterRayLength = CGround::LinePlaneCol(start, dir, guiRayLength, CGround::GetWaterPlaneLevel());

	float minRayLength = groundRayLength;
	float minIngressDist = length;
	float minEgressDist = length;

	bool hitFactory = false;

	// if ray cares about water, take minimum
	// of distance to ground and water surface
	if (!ignoreWater)
		minRayLength = std::min(groundRayLength, waterRayLength);

	// Zero-K VR: the pointer ray is short (a few metres of table), so it often
	// reaches the ground but not the water plane, or (past the map's edge) the
	// water plane but not the ground. The plain min then picks the miss (-1)
	// and the ray "hits" nothing short of its full length, in mid-air. Take
	// whichever of the two it really reaches.
	if (VRState::pickFromEye && !ignoreWater) {
		if (groundRayLength < 0.0f)
			minRayLength = waterRayLength;
		else if (waterRayLength < 0.0f)
			minRayLength = groundRayLength;
	}

	if (groundOnly)
		return minRayLength;

	if (VRState::pickFromEye) {
		VRState::pickDebug = {};
		VRState::pickDebugIcon = {};
		VRState::pickDebug.groundDist = groundRayLength;
		VRState::pickDebug.waterDist = ignoreWater ? -1.0f : waterRayLength;
	}
	int pickHow = VRState::PICK_NONE;

	// set maximum ray until ground intersection taking lenience into account later
	float maxRayLength;
	if (minRayLength >= 0.0) {
		// normal intersection
		maxRayLength = minRayLength;
	} else if (waterRayLength >= 0.0) {
		// out of map we still want to intersect somewhere if possible
		maxRayLength = waterRayLength;
	} else {
		// pointing upwards
		maxRayLength = length;
	}
	maxRayLength = std::min(maxRayLength + globalConfig.selectThroughGround, length);

	// Zero-K VR: the eye views draw unit icons over the terrain, so the pointer
	// can pick an icon it sees behind a hill: look along the whole ray, and
	// let only models and features be hidden by the ground (below)
	const bool iconsThroughGround = VRState::pickFromEye;
	const float queryRayLength = iconsThroughGround ? length : maxRayLength;
	bool hitIsIcon = false;

	CollisionQuery cq;

	QuadFieldQuery qfQuery;
	if (useRadar) {
		const float allyTeamError = losHandler->GetAllyTeamRadarErrorSize(gu->myAllyTeam);
		quadField.GetQuadsOnWideRay(qfQuery, start, dir, queryRayLength, allyTeamError);
	} else {
		quadField.GetQuadsOnRay(qfQuery, start, dir, queryRayLength);
	}

	for (const int quadIdx: *qfQuery.quads) {
		const CQuadField::Quad& quad = quadField.GetQuad(quadIdx);

		// Unit Intersection
		for (const CUnit* u: quad.units) {
			const bool unitIsEnemy = !teamHandler.Ally(u->allyteam, gu->myAllyTeam);
			const bool unitOnRadar = (useRadar && losHandler->InRadar(u, gu->myAllyTeam));
			const bool unitInSight = (u->losStatus[gu->myAllyTeam] & (LOS_INLOS | LOS_CONTRADAR));
			const bool unitVisible = !unitIsEnemy || unitOnRadar || unitInSight || gu->spectatingFullView;

			if (u == exclude)
				continue;
			#if 0
			// test this bit only in synced traces, rely on noSelect here
			if (!u->HasCollidableStateBit(CSolidObject::CSTATE_BIT_QUADMAPRAYS))
				continue;
			#endif
			if (u->noSelect)
				continue;
			if (!unitVisible)
				continue;
			// Zero-K VR: the pointer never hits what the headset does not show
			if (VRState::pickFromEye && u->noDraw)
				continue;

			CollisionVolume cv = u->selectionVolume;

			// Zero-K VR: the pointer picks icons as the headset shows them
			float iconRadius = u->iconRadius;
			const bool isIcon = VRState::pickFromEye ? VRPickIcon(u, iconRadius) : u->GetIsIcon();

			// for iconified units, just pretend the collision
			// volume is a sphere of radius <unit->IconRadius>
			// (count radar blips as such too)
			if (isIcon || (!unitInSight && unitOnRadar && unitIsEnemy))
				cv.InitSphere(iconRadius);

			const bool shownAsIcon = (isIcon || (!unitInSight && unitOnRadar && unitIsEnemy));
			float ingressDist = 0.0f;
			float  egressDist = 0.0f;
			int unitHow = shownAsIcon ? VRState::PICK_ICON : VRState::PICK_HULL;

			if (iconsThroughGround && shownAsIcon) {
				// Zero-K VR: an icon is a flat picture facing the eye, drawn
				// where CUnitDrawerGLSL::DrawUnitIcon puts it (lifted clear of
				// the ground by its own size). The pointer hits it when it
				// passes within the icon's half-size of that point, and the hit
				// is there, on the icon, not on the skin of a sphere around the
				// unit (seen in the headset 2026-11-25: a solar collector's icon
				// stopped the pointer up to 14 cm above the table). The size is
				// capped at a few cm of table, whatever the eye distance says.
				float3 center = gu->spectatingFullView ? u->GetObjDrawMidPos() : u->GetObjDrawErrorPos(gu->myAllyTeam);
				const float maxRadius = VRState::ICON_PICK_MAX_RADIUS_M * VRState::pickElmosPerMetre * std::max(icon::iconHandler.GetIconData(u->currentIconIndex).GetSize(), 1.0f);
				const float radius = std::min(iconRadius, maxRadius);
				const float h = CGround::GetHeightReal(center.x, center.z, false);
				center.y = std::max(center.y, h + radius);

				const float t = (center - start).dot(dir);
				if (t <= 0.0f || t > guiRayLength)
					continue;
				if ((start + dir * t).SqDistance(center) > radius * radius)
					continue;

				ingressDist = t;
				egressDist = t;

				if (VRState::pickDebugIcon.unitID < 0 || t < VRState::pickDebugIcon.dist) {
					VRState::pickDebugIcon.unitID = u->id;
					VRState::pickDebugIcon.dist = t;
					VRState::pickDebugIcon.center = center;
					VRState::pickDebugIcon.radius = iconRadius;
					VRState::pickDebugIcon.eyeDist = VRState::IconSwitchDistance(u->GetObjDrawMidPos());
				}
			} else if (iconsThroughGround && !shownAsIcon) {
				// Zero-K VR: games enlarge selection volumes (Zero-K: 1.35 to 1.5
				// times, or a footprint-sized ellipsoid) to make clicking easy.
				// A pointer ray passing over a unit clipped that invisible shell
				// and stopped in mid-air, out of reach of the ground behind. So
				// the unit's own hull counts as a hit; the enlarged shell only
				// when the ray meets the ground (or water) right at the unit,
				// and then the hit is that ground point, not the shell.
				const CMatrix44f mat = u->GetTransformMatrix(false);
				const float3 end = start + dir * guiRayLength;

				if (CCollisionHandler::MouseHit(u, mat, start, end, &u->collisionVolume, &cq)) {
					ingressDist = cq.GetIngressPosDist(start, dir);
					egressDist = cq.GetEgressPosDist(start, dir);
				} else if (CCollisionHandler::MouseHit(u, mat, start, end, &cv, &cq)) {
					const float in = cq.GetIngressPosDist(start, dir);
					const float out = cq.GetEgressPosDist(start, dir);
					const float margin = std::max(u->radius * 0.25f, static_cast<float>(SQUARE_SIZE));

					if (minRayLength < 0.0f || minRayLength > out + margin)
						continue; // passes over or beside it, on to the ground beyond

					ingressDist = std::clamp(minRayLength, in, out);
					egressDist = out;
					unitHow = VRState::PICK_SHELL;
				} else {
					continue;
				}
			} else if (CCollisionHandler::MouseHit(u, u->GetTransformMatrix(false), start, start + dir * guiRayLength, &cv, &cq)) {
				// get the distance to the ray-volume ingress point
				// (not likely to generate inside-hit special cases)
				ingressDist = cq.GetIngressPosDist(start, dir);
				egressDist = cq.GetEgressPosDist(start, dir);
			} else {
				continue;
			}

			{
				// Zero-K VR: a model behind the ground stays hidden; an icon does not
				if (iconsThroughGround && !shownAsIcon && ingressDist > maxRayLength)
					continue;

				const bool factoryUnderCursor = u->unitDef->IsFactoryUnit();
				const bool factoryHitBeforeUnit = ((hitFactory && ingressDist < minIngressDist) || (!hitFactory &&  egressDist < minIngressDist));
				const bool unitHitInsideFactory = ((hitFactory && ingressDist <  minEgressDist) || (!hitFactory && ingressDist < minIngressDist));

				// give units in a factory higher priority than the factory itself
				if (hitUnit == nullptr || (factoryUnderCursor && factoryHitBeforeUnit) || (!factoryUnderCursor && unitHitInsideFactory)) {
					hitFactory = factoryUnderCursor;
					minIngressDist = ingressDist;
					minEgressDist = egressDist;

					hitUnit = u;
					hitFeature = nullptr;
					hitIsIcon = shownAsIcon;
					pickHow = unitHow;
				}
			}
		}

		// Feature Intersection
		for (const CFeature* f: quad.features) {
			if (!gu->spectatingFullView && !f->IsInLosForAllyTeam(gu->myAllyTeam))
				continue;
			#if 0
			// test this bit only in synced traces, rely on noSelect here
			if (!f->HasCollidableStateBit(CSolidObject::CSTATE_BIT_QUADMAPRAYS))
				continue;
			#endif
			if (f->noSelect)
				continue;

			const CollisionVolume& cv = f->selectionVolume;

			if (CCollisionHandler::MouseHit(f, f->GetTransformMatrix(false), start, start + dir * guiRayLength, &cv, &cq)) {
				float hitDist = cq.GetHitPosDist(start, dir);
				int featureHow = VRState::PICK_FEATURE_HULL;

				// Zero-K VR: as for units, the enlarged shell only counts where the
				// ray meets the ground at the feature, and then at that ground point
				if (iconsThroughGround) {
					CollisionQuery hullQuery;
					if (CCollisionHandler::MouseHit(f, f->GetTransformMatrix(false), start, start + dir * guiRayLength, &f->collisionVolume, &hullQuery)) {
						hitDist = hullQuery.GetHitPosDist(start, dir);
					} else {
						const float out = cq.GetEgressPosDist(start, dir);
						const float margin = std::max(f->radius * 0.25f, static_cast<float>(SQUARE_SIZE));
						if (minRayLength < 0.0f || minRayLength > out + margin)
							continue;
						hitDist = std::clamp(minRayLength, hitDist, std::max(out, hitDist));
						featureHow = VRState::PICK_FEATURE_SHELL;
					}
					if (hitDist > maxRayLength)
						continue;
				}

				const bool factoryHitBeforeUnit = ( hitFactory && hitDist <  minEgressDist);
				const bool unitHitInsideFactory = (!hitFactory && hitDist < minIngressDist);

				// we want the closest feature (intersection point) on the ray
				// give features in a factory (?) higher priority than the factory itself
				if (hitUnit == nullptr || factoryHitBeforeUnit || unitHitInsideFactory) {
					hitFactory = false;
					minIngressDist = hitDist;

					hitFeature = f;
					hitUnit = nullptr;
					hitIsIcon = false;
					pickHow = featureHow;
				}
			}
		}
	}

	// Zero-K VR: a picked icon wins over the ground in front of it
	if ((minRayLength > 0.0f) && (maxRayLength < minIngressDist) && !(iconsThroughGround && hitIsIcon && hitUnit != nullptr)) {
		minIngressDist = minRayLength;

		hitUnit    = nullptr;
		hitFeature = nullptr;
		pickHow = VRState::PICK_NONE;
	}

	if (VRState::pickFromEye)
		VRState::pickDebug.how = pickHow;

	// Zero-K VR: the pointer reached neither ground nor water and hit nothing:
	// report a miss, not a hit at the end of the ray (in mid-air)
	if (iconsThroughGround && hitUnit == nullptr && hitFeature == nullptr && minRayLength < 0.0f)
		return -1.0f;

	return minIngressDist;
}


bool TestCone(
	const float3& from,
	const float3& dir,
	float length,
	float spread,
	int allyteam,
	int traceFlags,
	CUnit* owner
) {
	RECOIL_DETAILED_TRACY_ZONE;
	QuadFieldQuery qfQuery;
	quadField.GetQuadsOnRay(qfQuery, from, dir, length);

	if (qfQuery.quads->empty())
		return true;

	const bool scanForAllies   = ((traceFlags & Collision::NOFRIENDLIES) == 0);
	const bool scanForNeutrals = ((traceFlags & Collision::NONEUTRALS  ) == 0);
	const bool scanForFeatures = ((traceFlags & Collision::NOFEATURES  ) == 0);

	for (const int quadIdx: *qfQuery.quads) {
		const CQuadField::Quad& quad = quadField.GetQuad(quadIdx);

		if (scanForAllies) {
			for (const CUnit* u: quad.teamUnits[allyteam]) {
				if (u == owner)
					continue;
				if (!u->HasCollidableStateBit(CSolidObject::CSTATE_BIT_QUADMAPRAYS))
					continue;

				if (TestConeHelper(from, dir, length, spread, u))
					return true;
			}
		}

		if (scanForNeutrals) {
			for (const CUnit* u: quad.units) {
				if (!u->IsNeutral())
					continue;
				if (u == owner)
					continue;
				if (!u->HasCollidableStateBit(CSolidObject::CSTATE_BIT_QUADMAPRAYS))
					continue;

				if (TestConeHelper(from, dir, length, spread, u))
					return true;
			}
		}

		if (scanForFeatures) {
			for (const CFeature* f: quad.features) {
				if (!f->HasCollidableStateBit(CSolidObject::CSTATE_BIT_QUADMAPRAYS))
					continue;

				if (TestConeHelper(from, dir, length, spread, f))
					return true;
			}
		}
	}

	return false;
}



bool TestTrajectoryCone(
	const float3& from,
	const float3& dir,
	float length,
	float linear,
	float quadratic,
	float spread,
	int allyteam,
	int traceFlags,
	CUnit* owner
) {
	RECOIL_DETAILED_TRACY_ZONE;
	QuadFieldQuery qfQuery;
	quadField.GetQuadsOnRay(qfQuery, from, dir, length);

	if (qfQuery.quads->empty())
		return true;

	const bool scanForAllies   = ((traceFlags & Collision::NOFRIENDLIES) == 0);
	const bool scanForNeutrals = ((traceFlags & Collision::NONEUTRALS  ) == 0);
	const bool scanForFeatures = ((traceFlags & Collision::NOFEATURES  ) == 0);

	for (const int quadIdx: *qfQuery.quads) {
		const CQuadField::Quad& quad = quadField.GetQuad(quadIdx);

		// friendly units in this quad
		if (scanForAllies) {
			for (const CUnit* u: quad.teamUnits[allyteam]) {
				if (u == owner)
					continue;
				if (!u->HasCollidableStateBit(CSolidObject::CSTATE_BIT_QUADMAPRAYS))
					continue;

				if (TestTrajectoryConeHelper(from, dir, length, linear, quadratic, spread, 0.0f, u))
					return true;

			}
		}

		// neutral units in this quad
		if (scanForNeutrals) {
			for (const CUnit* u: quad.units) {
				if (!u->IsNeutral())
					continue;
				if (u == owner)
					continue;
				if (!u->HasCollidableStateBit(CSolidObject::CSTATE_BIT_QUADMAPRAYS))
					continue;

				if (TestTrajectoryConeHelper(from, dir, length, linear, quadratic, spread, 0.0f, u))
					return true;
			}
		}

		// features in this quad
		if (scanForFeatures) {
			for (const CFeature* f: quad.features) {
				if (!f->HasCollidableStateBit(CSolidObject::CSTATE_BIT_QUADMAPRAYS))
					continue;

				if (TestTrajectoryConeHelper(from, dir, length, linear, quadratic, spread, 0.0f, f))
					return true;
			}
		}
	}

	return false;
}



} //namespace TraceRay
