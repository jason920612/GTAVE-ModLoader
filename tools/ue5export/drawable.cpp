#include "drawable.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>

namespace ue5export::drawable
{
	namespace
	{
		float Half(uint16_t h)
		{
			const uint32_t sign = (h >> 15) & 1, exponent = (h >> 10) & 0x1F, mantissa = h & 0x3FF;
			float v;
			if (exponent == 0)
				v = std::ldexp(static_cast<float>(mantissa), -24);
			else if (exponent == 31)
				v = mantissa ? NAN : INFINITY;
			else
				v = std::ldexp(static_cast<float>(mantissa | 0x400), static_cast<int>(exponent) - 25);
			return sign ? -v : v;
		}

		// One vertex element in a DXGI format -> up to 4 floats; the number of components, 0 for an unknown format.
		int Decode(uint8_t format, const uint8_t* p, float out[4])
		{
			float f[4];
			uint16_t h[4];
			int16_t s[4];
			switch (format)
			{
			case 2: // R32G32B32A32_FLOAT
				std::memcpy(out, p, 16);
				return 4;
			case 6: // R32G32B32_FLOAT
				std::memcpy(out, p, 12);
				return 3;
			case 16: // R32G32_FLOAT
				std::memcpy(out, p, 8);
				return 2;
			case 41: // R32_FLOAT
				std::memcpy(out, p, 4);
				return 1;
			case 10: // R16G16B16A16_FLOAT
				std::memcpy(h, p, 8);
				for (int i = 0; i < 4; ++i)
					out[i] = Half(h[i]);
				return 4;
			case 34: // R16G16_FLOAT
				std::memcpy(h, p, 4);
				out[0] = Half(h[0]), out[1] = Half(h[1]);
				return 2;
			case 13: // R16G16B16A16_SNORM
				std::memcpy(s, p, 8);
				for (int i = 0; i < 4; ++i)
					out[i] = std::max(-1.0f, s[i] / 32767.0f);
				return 4;
			case 37: // R16G16_SNORM
				std::memcpy(s, p, 4);
				out[0] = std::max(-1.0f, s[0] / 32767.0f), out[1] = std::max(-1.0f, s[1] / 32767.0f);
				return 2;
			case 11: // R16G16B16A16_UNORM
				std::memcpy(h, p, 8);
				for (int i = 0; i < 4; ++i)
					out[i] = h[i] / 65535.0f;
				return 4;
			case 35: // R16G16_UNORM
				std::memcpy(h, p, 4);
				out[0] = h[0] / 65535.0f, out[1] = h[1] / 65535.0f;
				return 2;
			case 28: // R8G8B8A8_UNORM
			case 30: // R8G8B8A8_UINT
				for (int i = 0; i < 4; ++i)
					out[i] = p[i] / 255.0f;
				return 4;
			case 87: // B8G8R8A8_UNORM
				out[0] = p[2] / 255.0f, out[1] = p[1] / 255.0f, out[2] = p[0] / 255.0f, out[3] = p[3] / 255.0f;
				return 4;
			case 31: // R8G8B8A8_SNORM
				for (int i = 0; i < 4; ++i)
					out[i] = std::max(-1.0f, static_cast<int8_t>(p[i]) / 127.0f);
				return 4;
			case 24: // R10G10B10A2_UNORM
			{
				uint32_t v;
				std::memcpy(&v, p, 4);
				f[0] = (v & 0x3FF) / 1023.0f, f[1] = ((v >> 10) & 0x3FF) / 1023.0f, f[2] = ((v >> 20) & 0x3FF) / 1023.0f, f[3] = (v >> 30) / 3.0f;
				std::memcpy(out, f, 16);
				return 4;
			}
			default:
				return 0;
			}
		}

		constexpr int kPosition = 0, kNormal = 4, kTangent = 8, kColor0 = 24, kColor1 = 25, kUv0 = 28;
	}

	const uint8_t* View::At(uint64_t pointer, size_t size) const
	{
		const size_t offset = pointer & 0x0FFFFFFF;
		const loader::convert::Bytes* block = (pointer >> 28) == 5 ? &r_.virtualBlock : (pointer >> 28) == 6 ? &r_.physicalBlock : nullptr;
		if (!block || (pointer >> 32) || offset + size > block->size())
			return nullptr;
		return block->data() + offset;
	}

	std::string View::String(uint64_t pointer) const
	{
		std::string s;
		for (const uint8_t* p = At(pointer, 1); p && *p; p = At(++pointer, 1))
			s += static_cast<char>(*p);
		return s;
	}

	std::vector<Texture> Textures(const View& view, uint64_t dictionary)
	{
		std::vector<Texture> out;
		const uint64_t list = view.Get<uint64_t>(dictionary + 0x30);
		const uint16_t count = view.Get<uint16_t>(dictionary + 0x38);
		for (uint16_t i = 0; i < count; ++i)
		{
			const uint64_t t = view.Get<uint64_t>(list + 8u * i);
			if (!view.At(t, 0x80))
				continue;
			Texture x;
			x.name = view.String(view.Get<uint64_t>(t + 0x28));
			x.width = view.Get<uint16_t>(t + 0x18);
			x.height = view.Get<uint16_t>(t + 0x1A);
			x.depth = std::max<uint16_t>(1, view.Get<uint16_t>(t + 0x1C));
			x.dxgi = view.Get<uint8_t>(t + 0x1F);
			x.mips = std::max<uint8_t>(1, view.Get<uint8_t>(t + 0x22));
			const size_t size = static_cast<size_t>(view.Get<uint32_t>(t + 0x08)) * view.Get<uint16_t>(t + 0x0C);
			if (const uint8_t* data = view.At(view.Get<uint64_t>(t + 0x38), size))
				x.data.assign(data, data + size);
			out.push_back(std::move(x));
		}
		return out;
	}

	namespace
	{
		using Quat = std::array<float, 4>; // x, y, z, w
		using Vec = std::array<float, 3>;

		Vec Rotate(const Quat& q, const Vec& v)
		{
			// v + 2 w (q x v) + 2 q x (q x v)
			const Vec u{q[0], q[1], q[2]};
			const auto cross = [](const Vec& a, const Vec& b) { return Vec{a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]}; };
			const Vec t = cross(u, v);
			const Vec tt = cross(u, t);
			return {v[0] + 2 * (q[3] * t[0] + tt[0]), v[1] + 2 * (q[3] * t[1] + tt[1]), v[2] + 2 * (q[3] * t[2] + tt[2])};
		}

		Quat Multiply(const Quat& a, const Quat& b)
		{
			return {a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1], a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
			    a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3], a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2]};
		}

		struct Pose
		{
			Quat rotation{0, 0, 0, 1};
			Vec translation{};
		};

		// The rest pose of a drawable's bones in the model, by bone tag. The skeleton (drawable +0x18): +0x20 the bones,
		// +0x5E their count; a bone (0x50 bytes): +0x00 rotation, +0x10 translation (both from its parent), +0x32
		// parent index, +0x44 tag.
		std::map<uint16_t, Pose> Bones(const View& view, uint64_t drawable, std::vector<Pose>* byIndex = nullptr)
		{
			std::map<uint16_t, Pose> out;
			const uint64_t skeleton = view.Get<uint64_t>(drawable + 0x18);
			if (!skeleton || !view.At(skeleton, 0x60))
				return out;
			const uint64_t bones = view.Get<uint64_t>(skeleton + 0x20);
			const uint16_t count = view.Get<uint16_t>(skeleton + 0x5E);
			std::vector<Pose> model;
			for (uint16_t i = 0; i < count && bones && view.At(bones + 0x50u * i, 0x50); ++i)
			{
				const uint64_t b = bones + 0x50u * i;
				Pose local;
				for (int k = 0; k < 4; ++k)
					local.rotation[k] = view.Get<float>(b + 4 * k);
				for (int k = 0; k < 3; ++k)
					local.translation[k] = view.Get<float>(b + 0x10 + 4 * k);
				const uint16_t parent = view.Get<uint16_t>(b + 0x32);
				Pose p = local;
				if (parent < i)
				{
					const Pose& up = model[parent];
					p.rotation = Multiply(up.rotation, local.rotation);
					const Vec moved = Rotate(up.rotation, local.translation);
					p.translation = {up.translation[0] + moved[0], up.translation[1] + moved[1], up.translation[2] + moved[2]};
				}
				model.push_back(p);
				out[view.Get<uint16_t>(b + 0x44)] = p;
			}
			if (byIndex)
				*byIndex = std::move(model);
			return out;
		}
	}

	std::vector<Light> Lights(const View& view, uint64_t field, uint64_t drawable)
	{
		std::vector<Light> out;
		const std::map<uint16_t, Pose> bones = Bones(view, drawable);
		const uint64_t list = view.Get<uint64_t>(field);
		const uint16_t count = view.Get<uint16_t>(field + 8);
		constexpr uint32_t kStride = 0xA8;
		// +0x08 position, +0x18 colour (rgb), +0x1C intensity, +0x20 flags, +0x24 bone, +0x26 type, +0x28 hours,
		// +0x2C falloff, +0x30 its exponent, +0x74 direction (where it shines), +0x80 tangent (a capsule's axis: a strip
		// light 20 m long shining down lies along it), +0x8C / +0x90 cone angles (degrees), +0x94 a capsule's length (m;
		// the same in the two floats after; the LOD lights have it as coneOuterAngleOrCapExt, x 1.8)
		for (uint16_t i = 0; i < count && list; ++i)
		{
			const uint64_t at = list + uint64_t{kStride} * i;
			if (!view.At(at, kStride))
				break;
			Light l;
			for (int k = 0; k < 3; ++k)
			{
				l.position[k] = view.Get<float>(at + 0x08 + 4 * k);
				l.direction[k] = view.Get<float>(at + 0x74 + 4 * k);
				l.tangent[k] = view.Get<float>(at + 0x80 + 4 * k);
				l.color[k] = view.Get<uint8_t>(at + 0x18 + k);
			}
			l.intensity = view.Get<float>(at + 0x1C);
			l.flags = view.Get<uint32_t>(at + 0x20);
			l.bone = view.Get<uint16_t>(at + 0x24);
			l.type = view.Get<uint8_t>(at + 0x26);
			l.timeFlags = view.Get<uint32_t>(at + 0x28);
			l.falloff = view.Get<float>(at + 0x2C);
			l.falloffExponent = view.Get<float>(at + 0x30);
			l.coneInner = view.Get<float>(at + 0x8C);
			l.coneOuter = view.Get<float>(at + 0x90);
			l.length = l.type == 4 ? view.Get<float>(at + 0x94) : 0.0f;
			// (+0x5C the glow's size in metres, 0 for none: a street light's 3, a traffic signal's 2, a shaded lamp's 0;
			// +0x6C its intensity)
			l.coronaSize = view.Get<float>(at + 0x5C);
			l.coronaIntensity = view.Get<float>(at + 0x6C);
			if (!(l.coronaSize >= 0 && l.coronaSize < 100 && l.coronaIntensity >= 0 && l.coronaIntensity < 1000))
				l.coronaSize = l.coronaIntensity = 0;
			if (!(l.length >= 0 && l.length < 500))
				l.length = 0;
			const bool sane = (l.type == 1 || l.type == 2 || l.type == 4) && l.falloff > 0 && l.falloff < 500 && l.intensity >= 0 && l.intensity < 10000;
			if (!sane)
				continue;
			if (l.bone != 0)
			{
				const auto pose = bones.find(l.bone);
				if (pose == bones.end())
					continue; // (a bone the skeleton does not have)
				const Vec at = Rotate(pose->second.rotation, l.position);
				l.position = {pose->second.translation[0] + at[0], pose->second.translation[1] + at[1], pose->second.translation[2] + at[2]};
				l.direction = Rotate(pose->second.rotation, l.direction);
				l.tangent = Rotate(pose->second.rotation, l.tangent);
			}
			out.push_back(l);
		}
		return out;
	}

	std::map<uint32_t, uint64_t> Drawables(const View& view, const std::string& extension)
	{
		std::map<uint32_t, uint64_t> out;
		if (extension == "ydr")
			out[0] = View::kRoot;
		else if (extension == "yft")
		{
			if (const uint64_t d = view.Get<uint64_t>(View::kRoot + 0x30))
				out[0] = d;
		}
		else if (extension == "ydd")
		{
			const uint64_t hashes = view.Get<uint64_t>(View::kRoot + 0x20), list = view.Get<uint64_t>(View::kRoot + 0x30);
			const uint16_t count = std::min(view.Get<uint16_t>(View::kRoot + 0x28), view.Get<uint16_t>(View::kRoot + 0x38));
			for (uint16_t i = 0; i < count; ++i)
				out[view.Get<uint32_t>(hashes + 4u * i)] = view.Get<uint64_t>(list + 8u * i);
		}
		return out;
	}

	bool Read(const View& view, uint64_t at, Model& out, std::string& error, bool allLods)
	{
		if (!view.At(at, 0xB0))
			return error = "no drawable", false;
		// +0x20 bounding sphere, +0x30 / +0x40 box, +0x50 the model lists of the four levels of detail, +0x70 their
		// distances.
		for (int i = 0; i < 4; ++i)
		{
			out.sphere[i] = view.Get<float>(at + 0x20 + 4 * i);
			out.lodDistances[i] = view.Get<float>(at + 0x70 + 4 * i);
		}
		for (int i = 0; i < 3; ++i)
		{
			out.bbMin[i] = view.Get<float>(at + 0x30 + 4 * i);
			out.bbMax[i] = view.Get<float>(at + 0x40 + 4 * i);
		}
		// Shaders: their textures (by name) and constants.
		const uint64_t group = view.Get<uint64_t>(at + 0x10);
		if (const uint64_t dictionary = view.Get<uint64_t>(group + 0x08))
			out.embedded = Textures(view, dictionary);
		const uint64_t shaders = view.Get<uint64_t>(group + 0x10);
		const uint16_t shaderCount = view.Get<uint16_t>(group + 0x18);
		for (uint16_t i = 0; i < shaderCount; ++i)
		{
			const uint64_t s = view.Get<uint64_t>(shaders + 8u * i);
			Material m;
			m.effect = view.Get<uint32_t>(s);
			m.drawBucket = view.Get<uint8_t>(s + 0x39);
			const uint64_t meta = view.Get<uint64_t>(s + 0x20), textures = view.Get<uint64_t>(s + 0x10), buffers = view.Get<uint64_t>(s + 0x08);
			const uint8_t* header = view.At(meta, 8);
			if (!header)
			{
				out.materials.push_back(m);
				continue;
			}
			// Entries: (name hash, info): info & 3 = 0 texture, 2 sampler, 3 constant; textures: (info >> 2) & 0x3F the
			// slot; constants: the same bits the buffer, (info >> 8) & 0xFFF the offset, info >> 20 the size in bytes.
			const int entries = header[4]; // (all of them: textures, samplers, constants)
			for (int k = 0; k < entries; ++k)
			{
				const uint32_t name = view.Get<uint32_t>(meta + 8 + 8u * k), info = view.Get<uint32_t>(meta + 12 + 8u * k);
				const uint32_t index = (info >> 2) & 0x3F;
				if ((info & 3) == 0)
				{
					const uint64_t texture = view.Get<uint64_t>(textures + 8u * index);
					if (texture && view.At(texture, 0x30))
						m.textures[name] = view.String(view.Get<uint64_t>(texture + 0x28));
				}
				else if ((info & 3) == 3)
				{
					const uint32_t offset = (info >> 8) & 0xFFF, size = info >> 20;
					const uint64_t buffer = view.Get<uint64_t>(buffers + 8u * index);
					if (const uint8_t* p = view.At(buffer + offset, size); p && size && size % 4 == 0)
					{
						std::vector<float> values(size / 4);
						std::memcpy(values.data(), p, size);
						m.constants[name] = std::move(values);
					}
				}
			}
			out.materials.push_back(std::move(m));
		}
		// (a model's parts can each hang on a bone - a fragment's: a street light's arm and lamp, a traffic light's
		// heads -, their vertices from that bone: the model's +0x28 has its index in the top byte, the rest 0 for such
		// a rigid part)
		std::vector<Pose> poses;
		Bones(view, at, &poses);
		// The levels of detail, most detailed first.
		for (uint8_t lod = 0; lod < (allLods ? 4 : 1); ++lod)
		{
			const uint64_t list = view.Get<uint64_t>(at + 0x50 + 8u * lod);
			if (!list)
				break;
			out.lodCount = lod + 1;
			const uint64_t models = view.Get<uint64_t>(list);
			const uint16_t modelCount = view.Get<uint16_t>(list + 8);
			for (uint16_t mi = 0; mi < modelCount; ++mi)
			{
				const uint64_t model = view.Get<uint64_t>(models + 8u * mi);
				const uint64_t geometries = view.Get<uint64_t>(model + 0x08), mapping = view.Get<uint64_t>(model + 0x20);
				const uint16_t count = view.Get<uint16_t>(model + 0x10);
				const uint32_t binding = view.Get<uint32_t>(model + 0x28);
				const Pose* bone = (binding & 0xFFFFFF) == 0 && (binding >> 24) < poses.size() ? &poses[binding >> 24] : nullptr;
				if (bone && bone->rotation == Quat{0, 0, 0, 1} && bone->translation == Vec{})
					bone = nullptr;
				for (uint16_t gi = 0; gi < count; ++gi)
				{
					const uint64_t g = view.Get<uint64_t>(geometries + 8u * gi);
					const uint64_t vb = view.Get<uint64_t>(g + 0x18), ib = view.Get<uint64_t>(g + 0x38);
					const uint32_t vertices = view.Get<uint32_t>(vb + 0x08), stride = view.Get<uint16_t>(vb + 0x0C) & 0xFFF;
					const uint64_t decl = view.Get<uint64_t>(vb + 0x38);
					const uint8_t* declaration = view.At(decl, 0x140);
					const uint8_t* data = view.At(view.Get<uint64_t>(vb + 0x18), static_cast<size_t>(vertices) * stride);
					const uint32_t indexCount = view.Get<uint32_t>(ib + 0x08), indexSize = view.Get<uint16_t>(ib + 0x0C) & 0xFFF;
					const uint8_t* indexData = view.At(view.Get<uint64_t>(ib + 0x18), static_cast<size_t>(indexCount) * indexSize);
					if (!declaration || !data || !indexData || !vertices || (indexSize != 2 && indexSize != 4))
					{
						out.warnings.push_back(std::format("geometry {}: unreadable", gi));
						continue;
					}
					uint32_t flags;
					std::memcpy(&flags, declaration + 0x138, 4);
					if (flags & 1)
					{
						out.warnings.push_back(std::format("geometry {}: a stream per element (not supported yet)", gi));
						continue;
					}
					const auto format = [&](int slot) { return declaration[0x104 + slot]; };
					const auto offset = [&](int slot) {
						uint32_t o;
						std::memcpy(&o, declaration + 4 * slot, 4);
						return o;
					};
					Section s;
					s.lod = lod;
					s.material = view.Get<uint16_t>(mapping + 2u * gi);
					s.vertices = vertices;
					int uvSets = 0;
					for (int k = 0; k < 8; ++k)
						if (format(kUv0 + k))
							uvSets = k + 1;
					s.uv.resize(uvSets);
					float v[4];
					for (uint32_t i = 0; i < vertices; ++i)
					{
						const uint8_t* p = data + static_cast<size_t>(i) * stride;
						const int n = Decode(format(kPosition), p + offset(kPosition), v);
						s.position.insert(s.position.end(), {v[0], n > 1 ? v[1] : 0, n > 2 ? v[2] : 0});
						if (format(kNormal) && Decode(format(kNormal), p + offset(kNormal), v) >= 3)
							s.normal.insert(s.normal.end(), {v[0], v[1], v[2]});
						if (format(kTangent) && Decode(format(kTangent), p + offset(kTangent), v) >= 3)
							s.tangent.insert(s.tangent.end(), {v[0], v[1], v[2], v[3]});
						for (const auto& [slot, colors] : {std::pair{kColor0, &s.color0}, std::pair{kColor1, &s.color1}})
							if (format(slot) == 28 || format(slot) == 30)
								colors->insert(colors->end(), p + offset(slot), p + offset(slot) + 4);
							else if (format(slot) && Decode(format(slot), p + offset(slot), v) == 4)
								for (int c = 0; c < 4; ++c)
									colors->push_back(static_cast<uint8_t>(std::lround(std::clamp(v[c], 0.0f, 1.0f) * 255)));
						for (int k = 0; k < uvSets; ++k)
						{
							v[0] = v[1] = 0;
							if (format(kUv0 + k))
								Decode(format(kUv0 + k), p + offset(kUv0 + k), v);
							s.uv[k].insert(s.uv[k].end(), {v[0], v[1]});
						}
					}
					if (s.normal.size() != 3 * static_cast<size_t>(vertices))
						s.normal.clear();
					if (s.tangent.size() != 4 * static_cast<size_t>(vertices))
						s.tangent.clear();
					s.indices.resize(indexCount);
					for (uint32_t i = 0; i < indexCount; ++i)
					{
						uint32_t index = 0;
						std::memcpy(&index, indexData + static_cast<size_t>(i) * indexSize, indexSize);
						s.indices[i] = index < vertices ? index : 0;
					}
					if (bone)
					{
						for (size_t i = 0; i + 2 < s.position.size(); i += 3)
						{
							const Vec p = Rotate(bone->rotation, {s.position[i], s.position[i + 1], s.position[i + 2]});
							for (int k = 0; k < 3; ++k)
								s.position[i + k] = p[k] + bone->translation[k];
						}
						for (size_t i = 0; i + 2 < s.normal.size(); i += 3)
						{
							const Vec n = Rotate(bone->rotation, {s.normal[i], s.normal[i + 1], s.normal[i + 2]});
							std::copy(n.begin(), n.end(), s.normal.begin() + static_cast<ptrdiff_t>(i));
						}
						for (size_t i = 0; i + 3 < s.tangent.size(); i += 4)
						{
							const Vec t = Rotate(bone->rotation, {s.tangent[i], s.tangent[i + 1], s.tangent[i + 2]});
							std::copy(t.begin(), t.end(), s.tangent.begin() + static_cast<ptrdiff_t>(i));
						}
					}
					out.sections.push_back(std::move(s));
				}
			}
		}
		return true;
	}
}
