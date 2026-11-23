/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#ifndef VR_MANAGER_H
#define VR_MANAGER_H

#include <memory>

/**
 * @brief OpenXR session owner for the Zero-K VR fork.
 *
 * Unsynced and render-only: nothing here may touch simulation state.
 * Compiled only into engine-legacy when ENABLE_OPENXR is set, and does
 * nothing at runtime unless the VREnable config is true.
 *
 * Milestone 0 shows the finished desktop frame on a world-locked quad
 * in the headset, which proves the loader, session, swapchain and frame
 * loop before stereo rendering is attempted.
 */
class CVRManager
{
public:
	static CVRManager& GetInstance();

	/// Call once after the GL context is current and GL state is initialized.
	void Init();
	/// Call while the GL context is still alive.
	void Kill();

	/**
	 * Call once per main-loop iteration, just before the desktop swap.
	 * Polls OpenXR events and, if a session is running, submits the
	 * current back buffer to the headset.
	 * @param haveFrame whether a new frame was drawn this iteration
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
