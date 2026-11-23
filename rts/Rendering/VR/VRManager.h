/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#ifndef VR_MANAGER_H
#define VR_MANAGER_H

#include <functional>
#include <memory>

/**
 * @brief OpenXR session owner for the Zero-K VR fork.
 *
 * Unsynced and render-only: nothing here may touch simulation state.
 * Compiled only into engine-legacy when ENABLE_OPENXR is set, and does
 * nothing at runtime unless the VREnable config is true.
 *
 * Per main-loop iteration:
 *   BeginFrame()  - waits for the headset, starts an OpenXR frame, locates the eyes
 *   DrawEyes()    - (in-game only) draws the world once per eye into the eye swapchains
 *   EndOfFrame()  - mirrors the finished desktop frame onto a floating panel and
 *                   submits the panel plus (if drawn) the stereo world
 */
class CVRManager
{
public:
	static CVRManager& GetInstance();

	/// Call once after the GL context is current and GL state is initialized.
	void Init();
	/// Call while the GL context is still alive.
	void Kill();

	/// Call at the start of each main-loop iteration, before any game update or drawing.
	void BeginFrame();

	/**
	 * Call from the game's draw, before the desktop world is drawn. Draws the
	 * world once per eye (as a miniature on a virtual table, or from the player
	 * camera offset by the head pose, per VRView) and copies each result into
	 * that eye's swapchain. Leaves the player camera as it found it.
	 * @param updateForCamera redoes the camera-dependent update work (draw
	 *        flags, ground patches, model uploads) for the active camera; also
	 *        called once more after the eyes to restore the desktop state
	 * @param drawWorld draws the world into the default framebuffer using the
	 *        active camera (normally CWorldDrawer::Draw + ResetMVPMatrices)
	 */
	void DrawEyes(const std::function<void()>& updateForCamera, const std::function<void()>& drawWorld);

	/**
	 * Call once per main-loop iteration, just before the desktop swap.
	 * @param haveFrame whether a new desktop frame was drawn this iteration
	 */
	void EndOfFrame(bool haveFrame);

	bool IsSessionRunning() const;

private:
	CVRManager();
	~CVRManager();

	struct Impl;
	std::unique_ptr<Impl> impl;
};

#endif // VR_MANAGER_H
