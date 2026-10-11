#include "physwalk.hpp"

#include <cmath>
#include <map>

#include <modloader/game.hpp>

namespace bball
{
	namespace b = ml::body;

	namespace
	{
		// Skeleton bone -> simulated body (tools/fit_capsules.py SEGMENTS); bones not listed take their parent's.
		const std::map<std::string, std::string>& Segments()
		{
			static const std::map<std::string, std::string> map = [] {
				std::map<std::string, std::string> m = {{"SKEL_Pelvis", "pelvis"}, {"SKEL_Spine_Root", "abdomen"}, {"SKEL_Spine0", "abdomen"},
				    {"SKEL_Spine1", "abdomen"}, {"SKEL_Spine2", "chest"}, {"SKEL_Spine3", "chest"}, {"SKEL_L_Clavicle", "chest"},
				    {"SKEL_R_Clavicle", "chest"}, {"SKEL_Neck_1", "head"}, {"SKEL_Head", "head"}};
				for (const char* s : {"L", "R"})
				{
					const std::string side(s), low = side == "L" ? "l_" : "r_";
					m["SKEL_" + side + "_UpperArm"] = low + "upperarm";
					m["RB_" + side + "_ArmRoll"] = low + "upperarm";
					m["SKEL_" + side + "_Forearm"] = low + "forearm";
					m["RB_" + side + "_ForeArmRoll"] = low + "forearm";
					m["SKEL_" + side + "_Hand"] = low + "hand";
					m["SKEL_" + side + "_Thigh"] = low + "thigh";
					m["RB_" + side + "_ThighRoll"] = low + "thigh";
					m["SKEL_" + side + "_Calf"] = low + "calf";
					m["SKEL_" + side + "_Foot"] = low + "foot";
					m["SKEL_" + side + "_Toe0"] = low + "toe";
				}
				return m;
			}();
			return map;
		}

		// The bone whose frame a body was built at (fit_capsules.py FRAME).
		std::string FrameBone(const std::string& body)
		{
			static const std::map<std::string, std::string> torso = {
			    {"pelvis", "SKEL_Pelvis"}, {"abdomen", "SKEL_Spine0"}, {"chest", "SKEL_Spine3"}, {"head", "SKEL_Head"}};
			if (const auto it = torso.find(body); it != torso.end())
				return it->second;
			const std::string side = body[0] == 'l' ? "L" : "R", part = body.substr(2);
			static const std::map<std::string, std::string> limbs = {{"upperarm", "UpperArm"}, {"forearm", "Forearm"}, {"hand", "Hand"},
			    {"thigh", "Thigh"}, {"calf", "Calf"}, {"foot", "Foot"}, {"toe", "Toe0"}};
			return "SKEL_" + side + "_" + limbs.at(part);
		}
	}

	bool PhysWalk::Start(int32_t ped, const std::wstring& dataDir, std::string& error)
	{
		m_ped = ped;
		m_body = std::make_unique<PhysBody>();
		if (!m_body->Load(dataDir, error))
			return false;
		m_sk = std::make_unique<b::Skeleton>(ped);
		const b::Skeleton& sk = *m_sk;
		const mjModel* m = m_body->Model();

		// Bind pose in the object frame (forward kinematics of the rest locals).
		std::vector<b::Transform> rest(sk.Count());
		for (int i = 0; i < sk.Count(); ++i)
			rest[i] = sk.Parent(i) < 0 ? sk.Rest(i) : rest[sk.Parent(i)] * sk.Rest(i);

		// Owner body of every bone (MuJoCo body id), climbing to the nearest mapped ancestor; the root has none.
		std::vector<int> owner(sk.Count(), -1);
		for (int i = 0; i < sk.Count(); ++i)
		{
			const auto it = Segments().find(sk.Name(i));
			if (it != Segments().end())
				owner[i] = mj_name2id(m, mjOBJ_BODY, it->second.c_str());
			else if (sk.Parent(i) >= 0)
				owner[i] = owner[sk.Parent(i)];
		}
		// Where the model placed its bind pose: body origin at qpos0 - the frame bone's bind position.
		mjData* d0 = mj_makeData(m);
		mj_kinematics(m, d0);
		const int pelvis = mj_name2id(m, mjOBJ_BODY, "pelvis");
		const int pelvisBone = sk.Find(std::string("SKEL_Pelvis"));
		if (pelvis < 0 || pelvisBone < 0)
		{
			mj_deleteData(d0);
			return error = "no pelvis in the model or skeleton", false;
		}
		m_bindLift = static_cast<float>(d0->xpos[3 * pelvis + 2] - rest[pelvisBone].pos.z);
		mj_deleteData(d0);

		m_links.clear();
		for (int i = 0; i < sk.Count(); ++i)
		{
			if (owner[i] < 0)
				continue;
			const int parent = sk.Parent(i);
			const std::string bodyName = mj_id2name(m, mjOBJ_BODY, owner[i]);
			const int frame = sk.Find(FrameBone(bodyName));
			const bool boundary = parent < 0 || owner[parent] != owner[i] || i == frame;
			if (!boundary || frame < 0)
				continue;
			m_links.push_back({i, owner[i], rest[i].rot, rest[i].pos - rest[frame].pos});
		}

		// The entity as it stands now; the simulation's floor at the ground under it.
		const Vector3 p = ENTITY::GET_ENTITY_COORDS(ped, false);
		float qx = 0, qy = 0, qz = 0, qw = 1;
		ENTITY::GET_ENTITY_QUATERNION(ped, &qx, &qy, &qz, &qw);
		m_entity = {b::Quat{qx, qy, qz, qw}.Normalized(), {p.x, p.y, p.z}};
		float ground = p.z - 1.0f;
		MISC::GET_GROUND_Z_FOR_3D_COORD(p.x, p.y, p.z + 0.5f, &ground, false, false);
		m_floor = ground - p.z; // object-frame z of the ground (the simulation's z = 0)
		m_body->Reset(0);
		m_time = 0;
		ml::Log("physwalk: {} bones posed from {} bodies; bind lift {:.3f} m, ground at {:.3f} m in the object frame, clip {} steps",
		    m_links.size(), m->nbody - 1, m_bindLift, m_floor, m_body->ClipSteps());
		return true;
	}

	bool PhysWalk::Update(float frameTime)
	{
		if (!m_body)
			return false;
		const double dt = m_body->ControlDt();
		m_time += frameTime;
		bool running = true;
		for (int n = 0; m_time >= dt && n < 5; ++n)
		{
			m_time -= dt;
			if (!m_body->Step())
			{
				running = false;
				break;
			}
		}
		Pose();
		return running;
	}

	void PhysWalk::Pose()
	{
		const mjData* d = m_body->Data();
		b::Pose pose(*m_sk);
		for (const BoneLink& l : m_links)
		{
			const double* q = d->xquat + 4 * l.body;
			const double* x = d->xpos + 3 * l.body;
			const b::Quat bodyRot{static_cast<float>(q[1]), static_cast<float>(q[2]), static_cast<float>(q[3]), static_cast<float>(q[0])};
			// Simulation -> object frame: the bind pose sits m_bindLift above the object origin; the floor is at m_floor.
			const b::Vec3 bodyPos{static_cast<float>(x[0]), static_cast<float>(x[1]), static_cast<float>(x[2]) + m_floor};
			const b::Transform object{bodyRot * l.restRot, bodyPos + bodyRot.Rotate(l.offset)};
			const b::Transform world = m_entity * object;
			const int parent = m_sk->Parent(l.bone);
			const b::Transform local = parent < 0 ? world : pose.WorldOf(parent).Inverse() * world;
			pose.SetLocal(l.bone, local.rot);
			pose.SetLocalTranslation(l.bone, local.pos);
		}
		pose.Commit();
	}

	b::Vec3 PhysWalk::WorldDirection() const
	{
		const auto move = m_body->ReferenceDisplacement();
		const b::Vec3 w = m_entity.rot.Rotate(b::Vec3{move[0], move[1], 0});
		const float len = std::sqrt(w.x * w.x + w.y * w.y);
		return len > 1e-4f ? b::Vec3{w.x / len, w.y / len, 0} : b::Vec3{0, 1, 0};
	}

	void PhysWalk::Stop()
	{
		if (m_ped)
			b::Pose::Clear(m_ped);
		m_body.reset();
		m_sk.reset();
		m_links.clear();
	}
}
