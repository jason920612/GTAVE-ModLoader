#include "polygon.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace worldgen::polygon
{
	double Area(const std::vector<P2>& ring)
	{
		double a = 0;
		for (size_t i = 0, n = ring.size(); i < n; ++i)
		{
			const P2& p = ring[i];
			const P2& q = ring[(i + 1) % n];
			a += static_cast<double>(p[0]) * q[1] - static_cast<double>(q[0]) * p[1];
		}
		return a / 2;
	}

	bool Inside(const std::vector<P2>& ring, P2 p)
	{
		bool in = false;
		for (size_t i = 0, n = ring.size(), j = n - 1; i < n; j = i++)
		{
			const P2& a = ring[i];
			const P2& b = ring[j];
			if ((a[1] > p[1]) != (b[1] > p[1]) && p[0] < (b[0] - a[0]) * (p[1] - a[1]) / (b[1] - a[1]) + a[0])
				in = !in;
		}
		return in;
	}

	float DistanceToSegment(P2 p, P2 a, P2 b)
	{
		const float dx = b[0] - a[0], dy = b[1] - a[1];
		const float l2 = dx * dx + dy * dy;
		float t = l2 > 0 ? ((p[0] - a[0]) * dx + (p[1] - a[1]) * dy) / l2 : 0;
		t = std::clamp(t, 0.0f, 1.0f);
		return std::hypot(p[0] - (a[0] + t * dx), p[1] - (a[1] + t * dy));
	}

	std::vector<P2> Clean(const std::vector<P2>& ring, float tolerance)
	{
		std::vector<P2> r;
		for (const P2& p : ring)
			if (r.empty() || std::hypot(p[0] - r.back()[0], p[1] - r.back()[1]) > 0.02f)
				r.push_back(p);
		while (r.size() > 1 && std::hypot(r.front()[0] - r.back()[0], r.front()[1] - r.back()[1]) <= 0.02f)
			r.pop_back();
		// (each pass drops the points their neighbours' line passes within the tolerance; the corners stay)
		for (bool changed = true; changed && r.size() > 3;)
		{
			changed = false;
			for (size_t i = 0; i < r.size() && r.size() > 3; ++i)
			{
				const P2& a = r[(i + r.size() - 1) % r.size()];
				const P2& b = r[(i + 1) % r.size()];
				if (DistanceToSegment(r[i], a, b) < tolerance)
				{
					r.erase(r.begin() + static_cast<ptrdiff_t>(i));
					changed = true;
					--i;
				}
			}
		}
		if (Area(r) < 0)
			std::reverse(r.begin(), r.end());
		return r;
	}

	std::vector<std::array<uint32_t, 3>> Triangulate(const std::vector<P2>& ring)
	{
		std::vector<std::array<uint32_t, 3>> out;
		const size_t n = ring.size();
		if (n < 3)
			return out;
		std::vector<uint32_t> idx(n);
		std::iota(idx.begin(), idx.end(), 0u);
		if (Area(ring) < 0)
			std::reverse(idx.begin(), idx.end());
		const auto cross = [&](uint32_t a, uint32_t b, uint32_t c) {
			return (static_cast<double>(ring[b][0]) - ring[a][0]) * (static_cast<double>(ring[c][1]) - ring[a][1]) -
			       (static_cast<double>(ring[b][1]) - ring[a][1]) * (static_cast<double>(ring[c][0]) - ring[a][0]);
		};
		// (a point on the triangle's edges counts as inside: an ear must not touch the rest of the ring)
		const auto inTriangle = [&](uint32_t p, uint32_t a, uint32_t b, uint32_t c) {
			return cross(a, b, p) >= -1e-9 && cross(b, c, p) >= -1e-9 && cross(c, a, p) >= -1e-9;
		};
		while (idx.size() > 3)
		{
			const size_t m = idx.size();
			bool clipped = false;
			for (size_t k = 0; k < m && !clipped; ++k)
			{
				const uint32_t a = idx[(k + m - 1) % m], b = idx[k], c = idx[(k + 1) % m];
				if (cross(a, b, c) <= 1e-9)
					continue;
				bool ear = true;
				for (size_t q = 0; q < m && ear; ++q)
				{
					const uint32_t p = idx[q];
					if (p == a || p == b || p == c)
						continue;
					// (only reflex points can lie inside a convex corner's triangle)
					const uint32_t pa = idx[(q + m - 1) % m], pc = idx[(q + 1) % m];
					if (cross(pa, p, pc) > 1e-9)
						continue;
					if (inTriangle(p, a, b, c) && !(ring[p] == ring[a] || ring[p] == ring[b] || ring[p] == ring[c]))
						ear = false;
				}
				if (!ear)
					continue;
				out.push_back({a, b, c});
				idx.erase(idx.begin() + static_cast<ptrdiff_t>(k));
				clipped = true;
			}
			if (clipped)
				continue;
			// no ear (a ring touching itself, numbers at their limits): drop the flattest corner
			size_t worst = 0;
			double best = 1e300;
			for (size_t k = 0; k < m; ++k)
			{
				const double c = std::fabs(cross(idx[(k + m - 1) % m], idx[k], idx[(k + 1) % m]));
				if (c < best)
					best = c, worst = k;
			}
			if (cross(idx[(worst + m - 1) % m], idx[worst], idx[(worst + 1) % m]) > 0)
				out.push_back({idx[(worst + m - 1) % m], idx[worst], idx[(worst + 1) % m]});
			idx.erase(idx.begin() + static_cast<ptrdiff_t>(worst));
		}
		if (cross(idx[0], idx[1], idx[2]) > 0)
			out.push_back({idx[0], idx[1], idx[2]});
		return out;
	}
}
