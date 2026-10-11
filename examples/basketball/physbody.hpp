// A physically simulated body driven by a trained imitation policy (research/phase0.md §50).
//
// The model, policy and reference come from tools/train/loco/export.py (LocoMuJoCo training). The observation is
// rebuilt exactly as LocoMuJoCo builds it (checked against its own rollout by bball_physcheck):
//   proprioception  root height + orientation (qpos[2:7]), joint angles, root velocity (qvel[0:6]), joint velocities
//   goal (GoalTrajMimic)  for the body now and for the reference at the current clip step, the 14 mimic sites relative
//                   to the upper body (position, rotation vector, 6D velocity), plus the reference's qpos (without the
//                   root's x/y) and qvel.
// Policy: normalised observation -> tanh(512) -> tanh(256) -> linear 37 = motor controls (clamped to [-1, 1]).
// Control at 100 Hz, 5 physics steps of 2 ms each.
#pragma once
#include <string>
#include <vector>

#include <array>

#include <mujoco/mujoco.h>

namespace bball
{
	class Policy
	{
	public:
		bool Load(const std::wstring& path, std::string& error);
		void Act(const std::vector<float>& obs, std::vector<float>& action) const;
		size_t ObsSize() const { return m_mean.size(); }

	private:
		struct Layer
		{
			int in = 0, out = 0;
			std::vector<float> kernel, bias; // kernel[in * out], row-major in x out
		};
		std::vector<float> m_mean, m_var;
		std::vector<Layer> m_layers;
	};

	struct Reference
	{
		int steps = 0, nq = 0, nv = 0;
		float dt = 0;
		std::vector<float> qpos, qvel; // steps x nq, steps x nv
		bool Load(const std::wstring& path, std::string& error);
	};

	class PhysBody
	{
	public:
		~PhysBody();
		// dir: the export folder (body.mjb, policy.bin, reference.bin).
		bool Load(const std::wstring& dir, std::string& error);

		// Starts at a clip step, in the reference's pose and velocity.
		void Reset(int clipStep);
		// One control step (policy + 5 physics steps). False when the clip has ended.
		bool Step();

		const mjModel* Model() const { return m_m; }
		mjData* Data() { return m_d; }
		int ClipStep() const { return m_step; }
		int ClipSteps() const { return m_ref.steps; }
		double ControlDt() const { return m_ref.dt; }
		// Pelvis displacement over the clip (x, y, z), in the simulation's frame.
		std::array<float, 3> ReferenceDisplacement() const
		{
			const size_t last = static_cast<size_t>(m_ref.steps - 1) * m_ref.nq;
			return {m_ref.qpos[last] - m_ref.qpos[0], m_ref.qpos[last + 1] - m_ref.qpos[1], m_ref.qpos[last + 2] - m_ref.qpos[2]};
		}

		// The observation for the current state at a clip step (exposed for the golden check).
		void Observe(int clipStep, std::vector<float>& obs);
		const Policy& GetPolicy() const { return m_policy; }

	private:
		void SiteQuantities(const mjData* d, float* out); // 14 x (3 + 3 + 6), as LocoMuJoCo orders them
		mjModel* m_m = nullptr;
		mjData* m_d = nullptr;
		mjData* m_refData = nullptr; // the reference's state, for its site quantities
		Policy m_policy;
		Reference m_ref;
		std::vector<int> m_sites;    // mimic sites, upper body first
		int m_step = 0;
		int m_substeps = 5;
		std::vector<float> m_obs, m_action;
	};
}
