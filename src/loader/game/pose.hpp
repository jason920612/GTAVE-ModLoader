#pragma once
#include <cstdint>

#include <modloader/modloader.h>

// Ped skeletons and pose overrides (research/phase0.md §42). A mod sets joint transforms for a ped; after the game's
// per-ped post-animation update they replace the animated ones (blended by weight), and the object matrices of those
// bones and everything below them are recomputed, so skinning and rendering use them.
namespace loader::game::pose
{
	bool Install(); // finds the game code and hooks the post-animation update; false (pose API off) when not found

	// Game thread (mod fibers). -1 / 0 on failure.
	int32_t BoneCount(int32_t ped);
	int32_t FindBone(int32_t ped, uint32_t tag);
	int32_t BoneInfo(int32_t ped, int32_t bone, MLBoneInfo* out);
	int32_t GetBone(int32_t ped, int32_t bone, int32_t space, float* matrix16);
	int32_t SetPose(int32_t ped, const MLBonePose* bones, int32_t count);
}
