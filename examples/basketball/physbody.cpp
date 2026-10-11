#include "physbody.hpp"

#include <cmath>
#include <cstring>
#include <fstream>

namespace bball
{
	namespace
	{
		// LocoMuJoCo's BaseRobotHumanoid.sites_for_mimic, in its order (the first is the reference frame).
		const char* kMimicSites[] = {"upper_body_mimic", "head_mimic", "pelvis_mimic", "left_shoulder_mimic", "left_elbow_mimic",
		    "left_hand_mimic", "left_hip_mimic", "left_knee_mimic", "left_foot_mimic", "right_shoulder_mimic", "right_elbow_mimic",
		    "right_hand_mimic", "right_hip_mimic", "right_knee_mimic", "right_foot_mimic"};

		std::vector<char> ReadAll(const std::wstring& path)
		{
			std::ifstream f(path, std::ios::binary);
			return std::vector<char>((std::istreambuf_iterator<char>(f)), {});
		}

		struct Reader
		{
			const std::vector<char>& b;
			size_t o = 0;
			bool ok = true;
			template <class T> T Get()
			{
				T v{};
				if (o + sizeof(T) > b.size())
					return ok = false, v;
				std::memcpy(&v, b.data() + o, sizeof(T));
				o += sizeof(T);
				return v;
			}
			void Floats(std::vector<float>& out, size_t n)
			{
				out.resize(n);
				if (o + 4 * n > b.size())
				{
					ok = false;
					return;
				}
				std::memcpy(out.data(), b.data() + o, 4 * n);
				o += 4 * n;
			}
			bool Magic(const char* m)
			{
				if (o + 4 > b.size() || std::memcmp(b.data() + o, m, 4) != 0)
					return false;
				o += 4;
				return Get<uint32_t>() == 1;
			}
		};

		// Rotation vector of a row-major rotation matrix (as scipy's Rotation.as_rotvec).
		void RotVec(const double* r, double* v)
		{
			double q[4];
			mju_mat2Quat(q, r);
			mju_quat2Vel(v, q, 1.0);
		}
	}

	bool Policy::Load(const std::wstring& path, std::string& error)
	{
		const auto b = ReadAll(path);
		Reader r{b};
		if (!r.Magic("BBPL"))
			return error = "policy: not a BBPL file", false;
		const uint32_t layers = r.Get<uint32_t>(), obs = r.Get<uint32_t>();
		r.Floats(m_mean, obs);
		r.Floats(m_var, obs);
		m_layers.resize(layers);
		for (Layer& l : m_layers)
		{
			l.in = static_cast<int>(r.Get<uint32_t>());
			l.out = static_cast<int>(r.Get<uint32_t>());
			r.Floats(l.kernel, static_cast<size_t>(l.in) * l.out);
			r.Floats(l.bias, l.out);
		}
		if (!r.ok || m_layers.empty() || m_layers[0].in != static_cast<int>(obs))
			return error = "policy: truncated or inconsistent", false;
		return true;
	}

	void Policy::Act(const std::vector<float>& obs, std::vector<float>& action) const
	{
		std::vector<float> x(obs.size()), y;
		for (size_t i = 0; i < obs.size(); ++i)
			x[i] = (obs[i] - m_mean[i]) / std::sqrt(m_var[i] + 1e-8f);
		for (size_t li = 0; li < m_layers.size(); ++li)
		{
			const Layer& l = m_layers[li];
			y.assign(l.bias.begin(), l.bias.end());
			for (int i = 0; i < l.in; ++i)
			{
				const float xi = x[i];
				const float* row = &l.kernel[static_cast<size_t>(i) * l.out];
				for (int j = 0; j < l.out; ++j)
					y[j] += xi * row[j];
			}
			if (li + 1 < m_layers.size())
				for (float& v : y)
					v = std::tanh(v);
			x.swap(y);
		}
		action = x;
	}

	bool Reference::Load(const std::wstring& path, std::string& error)
	{
		const auto b = ReadAll(path);
		Reader r{b};
		if (!r.Magic("BBRF"))
			return error = "reference: not a BBRF file", false;
		steps = static_cast<int>(r.Get<uint32_t>());
		nq = static_cast<int>(r.Get<uint32_t>());
		nv = static_cast<int>(r.Get<uint32_t>());
		dt = r.Get<float>();
		r.Floats(qpos, static_cast<size_t>(steps) * nq);
		r.Floats(qvel, static_cast<size_t>(steps) * nv);
		return r.ok || (error = "reference: truncated", false);
	}

	PhysBody::~PhysBody()
	{
		if (m_refData)
			mj_deleteData(m_refData);
		if (m_d)
			mj_deleteData(m_d);
		if (m_m)
			mj_deleteModel(m_m);
	}

	bool PhysBody::Load(const std::wstring& dir, std::string& error)
	{
		const std::wstring mjb = dir + L"\\body.mjb";
		std::string narrow; // MuJoCo takes char paths (the game folder is ASCII)
		for (const wchar_t c : mjb)
			narrow += static_cast<char>(c);
		m_m = mj_loadModel(narrow.c_str(), nullptr);
		if (!m_m)
			return error = "body.mjb did not load", false;
		m_d = mj_makeData(m_m);
		m_refData = mj_makeData(m_m);
		if (!m_policy.Load(dir + L"\\policy.bin", error) || !m_ref.Load(dir + L"\\reference.bin", error))
			return false;
		if (m_ref.nq != m_m->nq || m_ref.nv != m_m->nv)
			return error = "reference does not match the model", false;
		for (const char* name : kMimicSites)
		{
			const int id = mj_name2id(m_m, mjOBJ_SITE, name);
			if (id < 0)
				return error = std::string("model has no site ") + name, false;
			m_sites.push_back(id);
		}
		m_substeps = static_cast<int>(std::lround(m_ref.dt / m_m->opt.timestep));
		Reset(0);
		return true;
	}

	void PhysBody::Reset(int clipStep)
	{
		m_step = clipStep;
		mj_resetData(m_m, m_d);
		for (int i = 0; i < m_m->nq; ++i)
			m_d->qpos[i] = m_ref.qpos[static_cast<size_t>(clipStep) * m_m->nq + i];
		for (int i = 0; i < m_m->nv; ++i)
			m_d->qvel[i] = m_ref.qvel[static_cast<size_t>(clipStep) * m_m->nv + i];
		mj_forward(m_m, m_d);
	}

	void PhysBody::SiteQuantities(const mjData* d, float* out)
	{
		const int n = static_cast<int>(m_sites.size());
		// 6D site velocities (angular, linear) in the world frame, about the site (LocoMuJoCo calc_site_velocities).
		std::vector<double> vel(6 * static_cast<size_t>(n));
		for (int s = 0; s < n; ++s)
		{
			const int site = m_sites[s], body = m_m->site_bodyid[site], root = m_m->body_rootid[body];
			const double* cvel = d->cvel + 6 * body;
			double r[3], cr[3];
			mju_sub3(r, d->site_xpos + 3 * site, d->subtree_com + 3 * root);
			mju_cross(cr, r, cvel);
			for (int k = 0; k < 3; ++k)
			{
				vel[6 * s + k] = cvel[k];
				vel[6 * s + 3 + k] = cvel[3 + k] - cr[k];
			}
		}
		const double* mainPos = d->site_xpos + 3 * m_sites[0];
		const double* mainMat = d->site_xmat + 9 * m_sites[0];
		const double* mainVel = &vel[0];
		float* rpos = out;
		float* rang = out + 3 * (n - 1);
		float* rvel = out + 6 * (n - 1);
		for (int s = 1; s < n; ++s)
		{
			const int site = m_sites[s];
			for (int k = 0; k < 3; ++k)
				rpos[3 * (s - 1) + k] = static_cast<float>(d->site_xpos[3 * site + k] - mainPos[k]);
			// Relative rotation main^T * site, as a rotation vector.
			double rel[9], v[3];
			mju_mulMatTMat(rel, mainMat, d->site_xmat + 9 * site, 3, 3, 3);
			RotVec(rel, v);
			for (int k = 0; k < 3; ++k)
				rang[3 * (s - 1) + k] = static_cast<float>(v[k]);
			// Relative velocity (LocoMuJoCo calculate_relative_velocity_in_local_frame):
			// angular rel^T * w_site - w_main, linear mainMat * (v_main - v_site).
			double angB[3], lin[3], dl[3];
			mju_mulMatTVec(angB, rel, &vel[6 * s], 3, 3);
			for (int k = 0; k < 3; ++k)
				dl[k] = mainVel[3 + k] - vel[6 * s + 3 + k];
			mju_mulMatVec(lin, mainMat, dl, 3, 3);
			for (int k = 0; k < 3; ++k)
			{
				rvel[6 * (s - 1) + k] = static_cast<float>(angB[k] - mainVel[k]);
				rvel[6 * (s - 1) + 3 + k] = static_cast<float>(lin[k]);
			}
		}
	}

	void PhysBody::Observe(int clipStep, std::vector<float>& obs)
	{
		const int nq = static_cast<int>(m_m->nq), nv = static_cast<int>(m_m->nv), n = static_cast<int>(m_sites.size()) - 1;
		obs.clear();
		for (int i = 2; i < nq; ++i)
			obs.push_back(static_cast<float>(m_d->qpos[i]));
		for (int i = 0; i < nv; ++i)
			obs.push_back(static_cast<float>(m_d->qvel[i]));
		// Goal: the body's site quantities, then the reference's qpos (no root x/y), qvel and site quantities.
		const size_t sites = static_cast<size_t>(n) * 12;
		size_t at = obs.size();
		obs.resize(at + sites);
		SiteQuantities(m_d, obs.data() + at);
		const int step = clipStep < m_ref.steps ? clipStep : m_ref.steps - 1;
		const float* rq = &m_ref.qpos[static_cast<size_t>(step) * nq];
		const float* rv = &m_ref.qvel[static_cast<size_t>(step) * nv];
		for (int i = 0; i < nq; ++i)
			m_refData->qpos[i] = rq[i];
		for (int i = 0; i < nv; ++i)
			m_refData->qvel[i] = rv[i];
		mj_forward(m_m, m_refData);
		for (int i = 2; i < nq; ++i)
			obs.push_back(rq[i]);
		for (int i = 0; i < nv; ++i)
			obs.push_back(rv[i]);
		at = obs.size();
		obs.resize(at + sites);
		SiteQuantities(m_refData, obs.data() + at);
	}

	bool PhysBody::Step()
	{
		if (m_step >= m_ref.steps - 1)
			return false;
		Observe(m_step, m_obs);
		m_policy.Act(m_obs, m_action);
		for (int i = 0; i < m_m->nu && i < static_cast<int>(m_action.size()); ++i)
			m_d->ctrl[i] = m_action[i];
		for (int k = 0; k < m_substeps; ++k)
			mj_step(m_m, m_d);
		++m_step;
		return true;
	}
}
