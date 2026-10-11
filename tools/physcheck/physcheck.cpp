// Checks the game's physical body (examples/basketball/physbody.cpp) against LocoMuJoCo's own rollout (golden.bin
// from tools/train/loco/export.py): for every recorded step, the observation rebuilt from the recorded qpos/qvel and
// clip step, and the policy's action, must match. Then runs the body on its own for the whole clip.
// usage: bball_physcheck <export dir>      (mujoco.dll next to the exe or on PATH)
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "../../examples/basketball/physbody.hpp"

// Observation blocks (LocoMuJoCo order; 14 mimic sites besides the upper body).
struct Block
{
	const char* name;
	int from, to;
};
const Block kBlocks[] = {{"qpos (root z, quat, joints)", 0, 42}, {"qvel", 42, 85}, {"body site positions", 85, 127},
    {"body site rotations", 127, 169}, {"body site velocities", 169, 253}, {"reference qpos", 253, 295}, {"reference qvel", 295, 338},
    {"reference site positions", 338, 380}, {"reference site rotations+vel", 380, 506}};

double Percentile(std::vector<double> v, double p)
{
	if (v.empty())
		return 0;
	std::sort(v.begin(), v.end());
	return v[std::min(v.size() - 1, static_cast<size_t>(p * (v.size() - 1) + 0.5))];
}

int wmain(int argc, wchar_t** argv)
{
	if (argc < 2)
	{
		std::puts("usage: bball_physcheck <export dir>");
		return 2;
	}
	const std::wstring dir = argv[1];
	bball::PhysBody body;
	std::string error;
	if (!body.Load(dir, error))
	{
		std::printf("load failed: %s\n", error.c_str());
		return 1;
	}
	std::ifstream f(dir + L"\\golden.bin", std::ios::binary);
	std::vector<char> b((std::istreambuf_iterator<char>(f)), {});
	uint32_t hdr[7];
	std::memcpy(hdr, b.data(), sizeof(hdr));
	const uint32_t steps = hdr[2], nq = hdr[3], nv = hdr[4], no = hdr[5], na = hdr[6];
	size_t o = 4 + 6 * 4;
	mjData* d = body.Data();
	double worstObs = 0, worstAct = 0;
	std::vector<double> blockErr[9];
	int worstObsIndex = -1, worstObsStep = -1;
	std::vector<float> obs, action;
	for (uint32_t s = 0; s < steps; ++s)
	{
		uint32_t clip;
		std::memcpy(&clip, b.data() + o, 4);
		o += 4;
		const float* q = reinterpret_cast<const float*>(b.data() + o);
		o += 4 * nq;
		const float* v = reinterpret_cast<const float*>(b.data() + o);
		o += 4 * nv;
		const float* go = reinterpret_cast<const float*>(b.data() + o);
		o += 4 * no;
		const float* ga = reinterpret_cast<const float*>(b.data() + o);
		o += 4 * na;
		for (uint32_t i = 0; i < nq; ++i)
			d->qpos[i] = q[i];
		for (uint32_t i = 0; i < nv; ++i)
			d->qvel[i] = v[i];
		mj_forward(body.Model(), d);
		body.Observe(static_cast<int>(clip), obs);
		if (obs.size() != no)
		{
			std::printf("observation size %zu, golden %u\n", obs.size(), no);
			return 1;
		}
		for (int k = 0; k < 9; ++k)
		{
			double e = 0;
			for (int i = kBlocks[k].from; i < kBlocks[k].to; ++i)
				e = std::fmax(e, std::fabs(obs[i] - go[i]));
			blockErr[k].push_back(e);
		}
		for (uint32_t i = 0; i < no; ++i)
		{
			const double e = std::fabs(obs[i] - go[i]);
			if (e > worstObs)
				worstObs = e, worstObsIndex = static_cast<int>(i), worstObsStep = static_cast<int>(s);
		}
		std::vector<float> gobs(go, go + no);
		body.GetPolicy().Act(gobs, action);
		for (uint32_t i = 0; i < na; ++i)
			worstAct = std::fmax(worstAct, std::fabs(action[i] - ga[i]));
		if (s < 3 || s + 1 == steps)
			std::printf("step %u (clip %u): obs[0..3] %.4f %.4f %.4f golden %.4f %.4f %.4f\n", s, clip, obs[0], obs[1], obs[2], go[0], go[1], go[2]);
	}
	std::printf("observation: largest difference %.6f (index %d, step %d) over %u steps\n", worstObs, worstObsIndex, worstObsStep, steps);
	for (int k = 0; k < 9; ++k)
		std::printf("  %-30s median %.6f, 95%% %.6f, max %.6f\n", kBlocks[k].name, Percentile(blockErr[k], 0.5), Percentile(blockErr[k], 0.95),
		    Percentile(blockErr[k], 1.0));
	std::printf("action from the golden observation: largest difference %.6f\n", worstAct);

	// The body on its own: physics + policy for the whole clip.
	body.Reset(0);
	int n = 0;
	double minHeight = 10;
	while (body.Step())
	{
		++n;
		minHeight = std::fmin(minHeight, body.Data()->qpos[2]);
	}
	std::printf("free run: %d control steps of %d, pelvis lowest %.3f m, final pelvis %.3f %.3f %.3f\n", n, body.ClipSteps() - 1, minHeight,
	    body.Data()->qpos[0], body.Data()->qpos[1], body.Data()->qpos[2]);
	return 0;
}
