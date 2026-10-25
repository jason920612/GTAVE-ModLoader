#include "bounds.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace worldgen::bounds
{
	using ue5export::drawable::View;
	using Vec = std::array<float, 3>;

	namespace
	{
		Vec Add(const Vec& a, const Vec& b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
		Vec Sub(const Vec& a, const Vec& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
		Vec Scale(const Vec& a, float s) { return {a[0] * s, a[1] * s, a[2] * s}; }

		Vec Apply(const Matrix& m, const Vec& p)
		{
			Vec out = m[3];
			for (int r = 0; r < 3; ++r)
				for (int c = 0; c < 3; ++c)
					out[c] += p[r] * m[r][c];
			return out;
		}

		Vec Read3(const View& view, uint64_t at)
		{
			return {view.Get<float>(at), view.Get<float>(at + 4), view.Get<float>(at + 8)};
		}

		// A parallelepiped from a corner and its three edges, as 12 triangles (outward faces).
		void AddBox(Mesh& out, const Matrix& toWorld, const Vec& o, const Vec& a, const Vec& b, const Vec& c, uint8_t material)
		{
			const uint32_t base = static_cast<uint32_t>(out.vertices.size());
			for (int i = 0; i < 8; ++i)
				out.vertices.push_back(Apply(toWorld, Add(Add(Add(o, i & 1 ? a : Vec{}), i & 2 ? b : Vec{}), i & 4 ? c : Vec{})));
			// the faces by corner (bit 0 a, bit 1 b, bit 2 c), counter-clockwise seen from outside when a, b, c are
			// right-handed (a left-handed set turns every face round): -a +a -b +b -c +c
			static constexpr uint32_t kFaces[6][4] = {{0, 4, 6, 2}, {1, 3, 7, 5}, {0, 1, 5, 4}, {2, 6, 7, 3}, {0, 2, 3, 1}, {4, 5, 7, 6}};
			const Vec n = {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
			const bool flip = n[0] * c[0] + n[1] * c[1] + n[2] * c[2] < 0;
			for (const auto& f : kFaces)
			{
				std::array<uint32_t, 3> t1{base + f[0], base + f[1], base + f[2]}, t2{base + f[0], base + f[2], base + f[3]};
				if (flip)
				{
					std::swap(t1[1], t1[2]);
					std::swap(t2[1], t2[2]);
				}
				out.triangles.push_back(t1);
				out.triangles.push_back(t2);
				out.materials.push_back(material);
				out.materials.push_back(material);
			}
		}

		void AddAabb(Mesh& out, const Matrix& toWorld, const Vec& lo, const Vec& hi, uint8_t material)
		{
			AddBox(out, toWorld, lo, {hi[0] - lo[0], 0, 0}, {0, hi[1] - lo[1], 0}, {0, 0, hi[2] - lo[2]}, material);
		}

		// A box round the segment from p to q with the radius (an upright capsule or cylinder: its footprint and height).
		void AddRound(Mesh& out, const Matrix& toWorld, const Vec& p, const Vec& q, float r, uint8_t material)
		{
			const Vec lo = {std::min(p[0], q[0]) - r, std::min(p[1], q[1]) - r, std::min(p[2], q[2]) - r};
			const Vec hi = {std::max(p[0], q[0]) + r, std::max(p[1], q[1]) + r, std::max(p[2], q[2]) + r};
			AddAabb(out, toWorld, lo, hi, material);
		}

		bool Geometry(const View& view, uint64_t at, const Matrix& toWorld, Mesh& out, std::string& error)
		{
			const uint32_t vertexCount = view.Get<uint32_t>(at + 0xD0), polyCount = view.Get<uint32_t>(at + 0xD4);
			const uint64_t polys = view.Get<uint64_t>(at + 0x88), vertices = view.Get<uint64_t>(at + 0xB0);
			const uint64_t materials = view.Get<uint64_t>(at + 0xF0), polyMaterials = view.Get<uint64_t>(at + 0x118);
			const uint8_t materialCount = view.Get<uint8_t>(at + 0x120);
			if (!view.At(polys, polyCount * 16ull) || !view.At(vertices, vertexCount * 6ull))
				return error = "bound geometry outside the resource", false;
			const Vec quantum = Read3(view, at + 0x90), offset = Read3(view, at + 0xA0);
			const auto vertex = [&](uint32_t i) {
				const uint8_t* q = view.At(vertices + 6ull * i, 6);
				int16_t v[3];
				std::memcpy(v, q, 6);
				return Vec{offset[0] + v[0] * quantum[0], offset[1] + v[1] * quantum[1], offset[2] + v[2] * quantum[2]};
			};
			const uint32_t base = static_cast<uint32_t>(out.vertices.size());
			for (uint32_t i = 0; i < vertexCount; ++i)
				out.vertices.push_back(Apply(toWorld, vertex(i)));
			for (uint32_t i = 0; i < polyCount; ++i)
			{
				const uint8_t* p = view.At(polys + 16ull * i, 16);
				uint16_t h[8];
				std::memcpy(h, p, 16);
				float radius;
				std::memcpy(&radius, p + 4, 4);
				const uint8_t index = polyMaterials ? view.Get<uint8_t>(polyMaterials + i) : 0;
				const uint8_t material = materials && index < materialCount ? view.Get<uint8_t>(materials + 8ull * index) : 0;
				const auto valid = [&](uint32_t v) { return v < vertexCount; };
				switch (p[0] & 7)
				{
				case 0: // triangle
				{
					const uint32_t a = h[2] & 0x7FFF, b = h[3] & 0x7FFF, c = h[4] & 0x7FFF;
					if (valid(a) && valid(b) && valid(c))
					{
						out.triangles.push_back({base + a, base + b, base + c});
						out.materials.push_back(material);
						++out.counts[0];
					}
					break;
				}
				case 1: // sphere
					if (valid(h[1]))
					{
						const Vec c = vertex(h[1]);
						AddRound(out, toWorld, c, c, radius, material);
						++out.counts[2];
					}
					break;
				case 2: // capsule
				case 4: // cylinder
					if (valid(h[1]) && valid(h[4]))
					{
						AddRound(out, toWorld, vertex(h[1]), vertex(h[4]), radius, material);
						++out.counts[(p[0] & 7) == 2 ? 3 : 4];
					}
					break;
				case 3: // box: alternate corners 000, 110, 101, 011
					if (valid(h[2]) && valid(h[3]) && valid(h[4]) && valid(h[5]))
					{
						const Vec v0 = vertex(h[2]), v1 = vertex(h[3]), v2 = vertex(h[4]), v3 = vertex(h[5]);
						const Vec a = Scale(Sub(Add(v1, v2), Add(v3, v0)), 0.5f), b = Scale(Sub(Add(v1, v3), Add(v2, v0)), 0.5f),
						          c = Scale(Sub(Add(v2, v3), Add(v1, v0)), 0.5f);
						AddBox(out, toWorld, v0, a, b, c, material);
						++out.counts[1];
					}
					break;
				default:
					break;
				}
			}
			return true;
		}

		bool Bound(const View& view, uint64_t at, const Matrix& toWorld, Mesh& out, int depth, std::string& error)
		{
			if (depth > 8 || !view.At(at, 0x70))
				return error = "bound outside the resource or nested too deep", false;
			const uint8_t type = view.Get<uint8_t>(at + 0x10);
			switch (type)
			{
			case 10: // composite
			{
				const uint64_t children = view.Get<uint64_t>(at + 0x70), matrices = view.Get<uint64_t>(at + 0x78);
				const uint16_t count = view.Get<uint16_t>(at + 0xA0);
				for (uint16_t i = 0; i < count; ++i)
				{
					const uint64_t child = view.Get<uint64_t>(children + 8ull * i);
					if (!child)
						continue;
					Matrix m = kIdentity;
					if (matrices)
						for (int r = 0; r < 4; ++r)
							m[r] = Read3(view, matrices + 64ull * i + 16ull * r);
					if (!Bound(view, child, Multiply(m, toWorld), out, depth + 1, error))
						return false;
				}
				return true;
			}
			case 4: // geometry
			case 8: // BVH
				return Geometry(view, at, toWorld, out, error);
			default: // box, sphere, capsule, cylinder, disc...: their box (+0x4C: their material, as far as seen)
			{
				AddAabb(out, toWorld, Read3(view, at + 0x30), Read3(view, at + 0x20), view.Get<uint8_t>(at + 0x4C));
				++out.counts[5];
				return true;
			}
			}
		}
	}

	Matrix Multiply(const Matrix& inner, const Matrix& outer)
	{
		Matrix out{};
		for (int r = 0; r < 3; ++r)
			for (int c = 0; c < 3; ++c)
				out[r][c] = inner[r][0] * outer[0][c] + inner[r][1] * outer[1][c] + inner[r][2] * outer[2][c];
		out[3] = Apply(outer, inner[3]);
		return out;
	}

	bool Collect(const View& view, uint64_t at, const Matrix& toWorld, Mesh& out, std::string& error)
	{
		return Bound(view, at, toWorld, out, 0, error);
	}
}
