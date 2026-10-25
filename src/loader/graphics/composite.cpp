#include "composite.hpp"

#include <Windows.h>
#include <d3dcompiler.h>

#include <atomic>
#include <cstring>

#include "../log.hpp"

namespace loader::graphics::composite
{
	namespace
	{
		// Stencil at DLSS time (the low three bits a kind): 1 peds, 2 vehicles, the 0x80 bit the player (0 buildings
		// and roads, 3 trees, 4 grass and terrain). Each output pixel takes the renderer's picture, the game's own where
		// those are - but not where the renderer's world is in front of them (its depth, metres, in the picture's alpha,
		// against the game's depth there: the game's leaner world may leave out a wall or a fence the renderer draws).
		// Their edges: the stencil is at the render size and jittered from frame to frame (by DLSS's sub-pixel offsets),
		// so taken as it is, a ped's outline is a staircase that crawls as the view moves - a cut-out pasted on. DLSS's
		// output has the outline smooth and steady, but against the game's own background. So at an edge the pixel is
		// unmixed: the mean colour of the ped's and of the background's samples about it (the game's picture before DLSS,
		// a 4 x 4 of the render size's, weighted by distance) give how much of the pixel is the ped (where the two
		// differ enough, and within half of what the stencil's own bilinear footprint says); the ped's own colour in the
		// pixel is what is left of it once that much of the background is taken out, kept within the colours of the ped's
		// samples about it (an estimate off by a little would otherwise make a colour neither has: a red rim on a dark
		// shoe over a pink floor), and that much of it goes over the renderer's picture.
		// The renderer's picture is scaled to the game's brightness first: Measure sums the log luminance of both
		// pictures over a sparse grid (not the sky: depth 0, reversed; not peds and vehicles), Update moves the scale a
		// little towards their ratio each frame, Composite applies it. A view of nothing but sky goes by the skies
		// instead (else the game's eye adaptation would brighten a night sky it cannot see the ground of). The game's own exposure and tone mapping come
		// after, as for its own scene. The renderer's highlights saturate as a camera's sensor does (from 7 stops over
		// the game's mean luminance, never past 10): physical light (the moon's 2,500 nits over a night city's few) is
		// beyond what the game's own picture ever holds, and its lens effects then streak across the screen. Indoors they
		// stop nearer the game's own brightest (Headroom): a room's lamp shades, a few hundred nits over a dim room, were
		// flat white shapes where the game's are lit cloth. Not a
		// number (a shader's 0/0) is black, not a blot the upscaler and the bloom spread.
		// Its colour balance follows the game's grading the same way (the timecycle's light colours, the region's and the
		// moment's tints: the game's people and vehicles are lit by them): at the same samples, the difference of each
		// picture's red to green and blue to green (log2), in 64 bins of 1/16 stop; the medians (the scenery both draw
		// alike outweighs what only one of them has, as the renderer's grass) are the white balance the renderer's picture
		// takes, at its own luminance. Lit samples only (−4 to +6 stops about the game's mean), not the sky.
		// Its highlights are kept near the game's own brightest (the 99.5th percentile of the game's picture, sky and lamps
		// included, by the same samples: 64 bins of 1/4 stop from 2 under its mean): the game's bloom and lens effects are
		// made for what its own picture holds, and a lamp physically a few thousand nits, ten stops over a dim room, spread
		// over the whole screen.
		// A lightning flash: Update keeps a baseline of the game's sky's mean log luminance and of its whole picture's
		// (followed but while a stop or more above it: a flash does not raise it), and the flash is how far above them this
		// frame is (a stop or more: a flash); the
		// figure is copied out to a small ring the CPU reads two frames later (Flash()) for the renderer's sky. Nothing
		// is matched to the game's picture while it flashes.
		// The game's particles (fire, smoke, sparks, rain: research §60.11, §60.21) go over the renderer's picture: drawn
		// again over black and over white (particles::TakeLayer), per pixel and channel they add c (over black) and let t
		// of what is behind them through (over white, less c); the renderer's pixel takes the same: picture t + c.
		// Measured samples skip them.
		// Its decals (bullet holes, blood, scorch marks: research §60.17) are on its surfaces: the G-buffer's surface colour
		// copied before and after them gives, per pixel and channel, how much they darkened or tinted it (after / before);
		// the renderer's pixel takes the same where its surface is the game's (no ped or vehicle there, the two depths alike).
		constexpr char kShader[] = R"(
Texture2D<float4> Picture : register(t0);
Texture2D<uint2> Stencil : register(t1);
Texture2D<float> Depth : register(t2);
Texture2D<float4> Input : register(t3); // the game's picture before DLSS (the render size)
Texture2D<float4> ParticlesBlack : register(t4); // the game's particles drawn over black and over white (the render size)
Texture2D<float4> ParticlesWhite : register(t5);
Texture2D<float4> DecalBefore : register(t6); // the G-buffer's surface colour before and after the game's decals
Texture2D<float4> DecalAfter : register(t7);
RWTexture2D<float4> Output : register(u0);
RWByteAddressBuffer Exposure : register(u1); // 0 game sum, 4 renderer sum (log2 * 64), 8 count, 12 scale (float),
                                             // 16 the game's mean log2 luminance (float); the sky's: 20 game sum,
                                             // 24 renderer sum, 28 count; 32, 36 the colour balance (log2 red / green,
                                             // blue / green, floats); 40 the game's brightest (log2, float); 44 the
                                             // flash (stops), 48, 52 its baselines (sky, picture), 56 the scale before
                                             // it, 60 flashing (uint); 64.. the red bins,
                                             // 320.. the blue bins, 576.. the luminance bins; 832.., 1088.. the game's
                                             // and the renderer's lit luminance bins; 1344 the contrast (float); 1348
                                             // the frames a flash has lasted
cbuffer Sizes : register(b0)
{
	uint2 OutSize; uint2 RenderSize; float Adapt; float Match; float Grade;
	float DepthNumerator; float DepthScale; float DepthOffset; // the game's depth d to metres: n / (d s - o)
	uint Matte; // 1: Input can be read (edges unmixed), 0: blended by the stencil's footprint alone
	float ContrastMatch; // how far the renderer's spread of brightness is brought down to the game's (0..1)
	uint Storm; // 1: the game's weather can flash (lightning)
	uint Particles; // 1: ParticlesBlack and ParticlesWhite hold this frame's particles
	uint Decals; // 1: DecalBefore and DecalAfter hold this frame's decals, linear; 2: sRGB-encoded
	float Headroom; // stops over the game's brightest the renderer's highlights may reach (indoors less: see below)
};

// Metres along the view at a depth of the game's (0, the sky: very far).
float GameMetres(float d)
{
	float w = d * DepthScale - DepthOffset;
	return d > 0 && abs(w) > 1e-12 ? abs(DepthNumerator / w) : 1e9;
}

bool Dynamic(uint s)
{
	uint kind = s & 7;
	return (s & 0x80) != 0 || kind == 1 || kind == 2;
}

float Luminance(float3 c)
{
	return dot(max(c, 0), float3(0.2126, 0.7152, 0.0722));
}

int2 RenderPixel(uint2 p)
{
	return min(int2((float2(p) + 0.5) * float2(RenderSize) / float2(OutSize)), int2(RenderSize) - 1);
}

// What the game's particles did at a pixel of the render size: how much of what is behind them shows through (t, per
// channel) and the light they put there (c; less than none where one darkens by subtracting).
void ParticleAt(int2 q, out float3 t, out float3 c)
{
	float3 black = ParticlesBlack.Load(int3(q, 0)).rgb, white = ParticlesWhite.Load(int3(q, 0)).rgb;
	t = 1;
	c = 0;
	if (any(isnan(black)) || any(isnan(white)) || any(isinf(black)) || any(isinf(white)))
		return;
	t = saturate(white - black);
	c = black;
}

// The same at a point of the render size between pixels (bilinear).
void ParticlesAt(float2 p, out float3 t, out float3 c)
{
	int2 b = int2(floor(p));
	float2 f = p - float2(b);
	t = 0;
	c = 0;
	[unroll] for (int y = 0; y <= 1; ++y)
	{
		[unroll] for (int x = 0; x <= 1; ++x)
		{
			float3 tt, tc;
			ParticleAt(clamp(b + int2(x, y), int2(0, 0), int2(RenderSize) - 1), tt, tc);
			float w = (x ? f.x : 1 - f.x) * (y ? f.y : 1 - f.y);
			t += tt * w;
			c += tc * w;
		}
	}
}

float3 Linear(float3 c)
{
	c = saturate(c);
	return Decals == 2 ? (c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4)) : c;
}

// How the game's decals changed the colour of its surface at a pixel of the render size, per channel (1: not at all) -
// where the renderer's surface there is the game's: no ped or vehicle (theirs come with them), the renderer's depth (near,
// metres) the game's.
float3 DecalAt(int2 q, float near)
{
	float3 before = DecalBefore.Load(int3(q, 0)).rgb, after = DecalAfter.Load(int3(q, 0)).rgb;
	if (all(abs(after - before) < 1.5 / 255.0) || any(isnan(after)) || any(isnan(before)))
		return 1;
	if (Dynamic(Stencil.Load(int3(q, 0)).y))
		return 1;
	float far = GameMetres(Depth.Load(int3(q, 0)));
	if (abs(near - far) > 0.3 + 0.03 * far)
		return 1;
	return clamp(Linear(after) / max(Linear(before), 0.004), 0.0, 2.0);
}

// The same at a point of the render size between pixels (bilinear).
float3 DecalsAt(float2 p, float near)
{
	int2 b = int2(floor(p));
	float2 f = p - float2(b);
	float3 ratio = 0;
	[unroll] for (int y = 0; y <= 1; ++y)
		[unroll] for (int x = 0; x <= 1; ++x)
			ratio += DecalAt(clamp(b + int2(x, y), int2(0, 0), int2(RenderSize) - 1), near) * (x ? f.x : 1 - f.x) * (y ? f.y : 1 - f.y);
	return ratio;
}

// Whether the game's particles touched a pixel of the render size (more than a little).
bool Touched(int2 q, float3 game)
{
	if (!Particles)
		return false;
	float3 t, c;
	ParticleAt(q, t, c);
	return any(t < 0.98) || Luminance(abs(c)) > 0.02 * (Luminance(game) + 1e-4);
}

groupshared int g_game[64];
groupshared int g_renderer[64];
groupshared int g_count[64];
groupshared uint g_bins[320];

[numthreads(8, 8, 1)]
void Measure(uint2 id : SV_DispatchThreadID, uint index : SV_GroupIndex)
{
	g_bins[index] = 0;
	g_bins[index + 64] = 0;
	g_bins[index + 128] = 0;
	g_bins[index + 192] = 0;
	g_bins[index + 256] = 0;
	GroupMemoryBarrierWithGroupSync();
	uint2 p = id * 8 + 4;
	int game = 0, renderer = 0, count = 0, sky = 0;
	if (all(p < OutSize))
	{
		int2 q = RenderPixel(p);
		// (not the game's peds and vehicles, nor its particles: the renderer has neither)
		if (!Dynamic(Stencil.Load(int3(q, 0)).y) && !Touched(q, Output[p].rgb))
		{
			float3 g = max(Output[p].rgb, 1e-4), r = max(Picture.Load(int3(p, 0)).rgb, 1e-4);
			float gl = log2(max(Luminance(g), 1e-4)), rl = log2(max(Luminance(r), 1e-4));
			game = int(clamp(gl, -14.0, 14.0) * 64);
			renderer = int(clamp(rl, -14.0, 14.0) * 64);
			count = 1;
			sky = Depth.Load(int3(q, 0)) > 0 ? 0 : 1;
			float key = asfloat(Exposure.Load(16)), scale = asfloat(Exposure.Load(12));
			rl += scale > 0 ? log2(scale) : 0;
			if (abs(key) < 20.0)
			{
				uint ignored;
				InterlockedAdd(g_bins[128 + uint(clamp(floor((gl - key + 2.0) * 4.0), 0.0, 63.0))], 1, ignored);
				// (both pictures' spread of brightness, the ground's: 1/4 stop bins from 10 under the mean)
				if (!sky)
				{
					InterlockedAdd(g_bins[192 + uint(clamp(floor((gl - key + 10.0) * 4.0), 0.0, 63.0))], 1, ignored);
					InterlockedAdd(g_bins[256 + uint(clamp(floor((rl - key + 10.0) * 4.0), 0.0, 63.0))], 1, ignored);
				}
			}
			if (!sky && abs(key) < 20.0 && gl > key - 4.0 && gl < key + 6.0 && rl > key - 4.0 && rl < key + 6.0)
			{
				float3 lg = log2(g), lr = log2(r);
				float2 d = float2((lg.r - lg.g) - (lr.r - lr.g), (lg.b - lg.g) - (lr.b - lr.g));
				uint2 bin = uint2(clamp(floor((d + 2.0) * 16.0), 0.0, 63.0));
				uint ignored;
				InterlockedAdd(g_bins[bin.x], 1, ignored);
				InterlockedAdd(g_bins[64 + bin.y], 1, ignored);
			}
		}
	}
	// (the ground's in 0..63 of the group's slots, the sky's in a second pass)
	[unroll] for (uint part = 0; part < 2; ++part)
	{
		int take = (count > 0 && uint(sky) == part) ? 1 : 0;
		g_game[index] = game * take;
		g_renderer[index] = renderer * take;
		g_count[index] = take;
		GroupMemoryBarrierWithGroupSync();
		[unroll] for (uint step = 32; step > 0; step >>= 1)
		{
			if (index < step)
			{
				g_game[index] += g_game[index + step];
				g_renderer[index] += g_renderer[index + step];
				g_count[index] += g_count[index + step];
			}
			GroupMemoryBarrierWithGroupSync();
		}
		if (index == 0 && g_count[0] > 0)
		{
			uint ignored;
			Exposure.InterlockedAdd(part * 20 + 0, uint(g_game[0]), ignored);
			Exposure.InterlockedAdd(part * 20 + 4, uint(g_renderer[0]), ignored);
			Exposure.InterlockedAdd(part * 20 + 8, uint(g_count[0]), ignored);
		}
		GroupMemoryBarrierWithGroupSync();
	}
	uint ignored;
	if (g_bins[index] > 0)
		Exposure.InterlockedAdd(64 + index * 4, g_bins[index], ignored);
	if (g_bins[index + 64] > 0)
		Exposure.InterlockedAdd(320 + index * 4, g_bins[index + 64], ignored);
	if (g_bins[index + 128] > 0)
		Exposure.InterlockedAdd(576 + index * 4, g_bins[index + 128], ignored);
	if (g_bins[index + 192] > 0)
		Exposure.InterlockedAdd(832 + index * 4, g_bins[index + 192], ignored);
	if (g_bins[index + 256] > 0)
		Exposure.InterlockedAdd(1088 + index * 4, g_bins[index + 256], ignored);
}

// The fraction p's point of 64 bins at offset (log2 from the mean - 10, 1/4 stops), and how many.
float Percentile(uint offset, float p, out uint total)
{
	total = 0;
	[loop] for (uint i = 0; i < 64; ++i)
		total += Exposure.Load(offset + i * 4);
	float value = 0;
	uint below = 0;
	bool found = false;
	[loop] for (uint j = 0; j < 64; ++j)
	{
		uint n = Exposure.Load(offset + j * 4);
		if (!found && n > 0 && float(below + n) >= p * total)
		{
			value = (float(j) + (p * total - below) / n) / 4.0 - 10.0;
			found = true;
		}
		below += n;
	}
	return value;
}

// The median of 64 bins at 64 + base * 4 (log2, from -2 in 1/16 stops), and how many; the bins emptied.
float Median(uint base, out uint total)
{
	total = 0;
	[loop] for (uint i = 0; i < 64; ++i)
		total += Exposure.Load(64 + (base + i) * 4);
	float median = 0;
	uint below = 0;
	bool found = false;
	[loop] for (uint j = 0; j < 64; ++j)
	{
		uint n = Exposure.Load(64 + (base + j) * 4);
		if (!found && n > 0 && 2 * (below + n) >= total)
		{
			median = (float(j) + (0.5 * total - below) / n) / 16.0 - 2.0;
			found = true;
		}
		below += n;
		Exposure.Store(64 + (base + j) * 4, 0);
	}
	return median;
}
)" // (in two pieces: one string literal holds 16 KB at most)
		                           R"(
[numthreads(1, 1, 1)]
void Update()
{
	int game = int(Exposure.Load(0)), renderer = int(Exposure.Load(4)), count = int(Exposure.Load(8));
	{
		int skyGame = int(Exposure.Load(20)), skyCount = int(Exposure.Load(28));
		float skyBase = asfloat(Exposure.Load(48)), pictureBase = asfloat(Exposure.Load(52)), flash = 0, sky = 0, picture = 0;
		if (skyCount > 64)
		{
			sky = float(skyGame) / (64.0 * skyCount);
			skyBase = abs(skyBase) < 40.0 ? skyBase : sky;
			flash = sky - skyBase;
		}
		if (count > 64)
		{
			picture = float(game) / (64.0 * count);
			pictureBase = abs(pictureBase) < 40.0 ? pictureBase : picture;
			if (skyCount <= 64)
				flash = picture - pictureBase;
		}
		// (a flash is the game's lightning: only while it storms, and short - brighter for longer than 90 frames is the
		// light itself changing, out of a tunnel or the clock moved on, and the baselines take it at once: taken for a
		// flash, it held the brightness and the colour balance as they were in the dark for seconds, a blue picture)
		uint flashFrames = flash > 1.0 ? Exposure.Load(1348) + 1 : 0;
		if (!Storm || flashFrames > 90)
		{
			skyBase = skyCount > 64 ? sky : skyBase;
			pictureBase = count > 64 ? picture : pictureBase;
			flash = 0;
			flashFrames = 0;
		}
		else
		{
			if (skyCount > 64)
				skyBase = lerp(skyBase, sky, sky > skyBase + 1.0 ? 0.002 : 0.05);
			if (count > 64)
				pictureBase = lerp(pictureBase, picture, picture > pictureBase + 1.0 ? 0.002 : 0.05);
		}
		Exposure.Store(1348, flashFrames);
		Exposure.Store3(44, asuint(float3(clamp(flash, 0.0, 8.0), skyBase, pictureBase)));
	}
	// (while it flashes the brightness is matched at once, frame by frame - the renderer's own flash comes a few frames
	// late and is not the game's strength - and the scale before it comes back after it; nothing else follows a flash)
	bool flashing = asfloat(Exposure.Load(44)) > 1.0;
	bool wasFlashing = Exposure.Load(60) != 0;
	float steady = asfloat(Exposure.Load(56));
	if (count <= 64) // (nothing but sky)
	{
		game = int(Exposure.Load(20)), renderer = int(Exposure.Load(24)), count = int(Exposure.Load(28));
	}
	float scale = asfloat(Exposure.Load(12));
	float key = asfloat(Exposure.Load(16));
	if (!(abs(key) < 20.0)) // (the buffer's first contents)
		key = count > 64 ? float(game) / (64.0 * count) : 0.0;
	if (flashing && !wasFlashing)
		steady = scale;
	if (!flashing && wasFlashing && steady > 0)
		scale = steady;
	if (count > 64)
	{
		float target = clamp((float(game) - float(renderer)) / (64.0 * count), -6.0, 6.0) * Match; // log2 of the ratio
		float current = scale > 0 ? log2(scale) : target;
		// (the renderer's sum is of its picture as it comes, before the scale)
		if (flashing)
			scale = exp2(target);
		else
		{
			scale = exp2(lerp(current, target, Adapt));
			key = lerp(key, float(game) / (64.0 * count), Adapt);
		}
	}
	Exposure.Store2(56, uint2(asuint(steady), flashing ? 1u : 0u));
	// (the game's brightest: the 99.5th percentile of its picture, eased like the rest; before any, 7 stops over the mean)
	uint lit = 0;
	[loop] for (uint i = 0; i < 64; ++i)
		lit += Exposure.Load(576 + i * 4);
	float top = asfloat(Exposure.Load(40));
	if (!(abs(top) < 40.0))
		top = key + 7.0;
	if (lit > 64 && !flashing)
	{
		uint above = 0;
		float found = 61.0;
		[loop] for (int j = 63; j >= 0; --j)
		{
			above += Exposure.Load(576 + uint(j) * 4);
			if (above * 200 >= lit)
			{
				found = float(j);
				break;
			}
		}
		top = lerp(top, key + (found + 1.0) / 4.0 - 2.0, Adapt);
	}
	[loop] for (uint k = 0; k < 64; ++k)
		Exposure.Store(576 + k * 4, 0);
	Exposure.Store4(0, uint4(0, 0, 0, asuint(scale > 0 ? scale : 1.0)));
	Exposure.Store4(16, uint4(asuint(key), 0, 0, 0));
	Exposure.Store(40, asuint(top));
	float2 balance = float2(asfloat(Exposure.Load(32)), asfloat(Exposure.Load(36)));
	if (!all(abs(balance) < 4.0))
		balance = 0;
	uint red, blue;
	float2 target = float2(Median(0, red), Median(64, blue));
	if (red > 64 && blue > 64 && !flashing)
		balance = lerp(balance, clamp(target, -1.5, 1.5) * Grade, Adapt);
	Exposure.Store2(32, asuint(balance));
	// The spread of brightness (the 10th to the 90th percentile, in stops) brought to the game's where the renderer's is
	// wider: indoors its lamps lit physically and its corners in their own shade spread three to five stops wider than
	// the game's rooms (research §60.7). A camera's sensor holds it all; an eye adapts from place to place and sees a room
	// as the game draws it, and the game's people and cars are lit for that room. Only ever narrowed, at most to 0.6.
	uint gameCount, rendererCount;
	float g10 = Percentile(832, 0.1, gameCount), g90 = Percentile(832, 0.9, gameCount);
	float r10 = Percentile(1088, 0.1, rendererCount), r90 = Percentile(1088, 0.9, rendererCount);
	[loop] for (uint m = 0; m < 128; ++m)
		Exposure.Store(832 + m * 4, 0);
	float contrast = asfloat(Exposure.Load(1344));
	if (!(contrast >= 0.5 && contrast <= 1.0))
		contrast = 1.0;
	if (gameCount > 64 && rendererCount > 64 && !flashing && r90 - r10 > 0.5)
		contrast = lerp(contrast, lerp(1.0, clamp((g90 - g10) / (r90 - r10), 0.6, 1.0), ContrastMatch), Adapt);
	Exposure.Store(1344, asuint(contrast));
}

[numthreads(8, 8, 1)]
void Composite(uint2 id : SV_DispatchThreadID)
{
	if (any(id >= OutSize))
		return;
	float2 p = (float2(id) + 0.5) * float2(RenderSize) / float2(OutSize) - 0.5;
	int2 b = int2(floor(p));
	float2 f = p - float2(b);
	float4 picture = Picture.Load(int3(id, 0));
	// (the renderer's depth here, metres; a ped or vehicle of the game's farther than that, by more than the two worlds'
	// grounds and shapes may differ, is behind the renderer's world)
	float near = picture.a;
	float covered = 0, foreWeight = 0, backWeight = 0;
	float3 fore = 0, back = 0, foreLow = 1e9, foreHigh = 0;
	[unroll] for (int y = -1; y <= 2; ++y)
	{
		[unroll] for (int x = -1; x <= 2; ++x)
		{
			int2 q = clamp(b + int2(x, y), int2(0, 0), int2(RenderSize) - 1);
			bool shown = false;
			if (Dynamic(Stencil.Load(int3(q, 0)).y))
			{
				float far = GameMetres(Depth.Load(int3(q, 0)));
				shown = near > far - (0.3 + 0.02 * far);
			}
			if (x >= 0 && x <= 1 && y >= 0 && y <= 1)
				covered += shown ? (x ? f.x : 1 - f.x) * (y ? f.y : 1 - f.y) : 0;
			float2 d = abs(float2(x, y) - f);
			float w = max(2.0 - d.x, 0) * max(2.0 - d.y, 0);
			float3 c = Matte ? max(Input.Load(int3(q, 0)).rgb, 0) : 0;
			if (shown)
			{
				fore += c * w, foreWeight += w;
				foreLow = min(foreLow, c), foreHigh = max(foreHigh, c);
			}
			else
				back += c * w, backWeight += w;
		}
	}
	float scale = asfloat(Exposure.Load(12));
	picture = float4(picture.rgb * scale, Output[id].a);
	if (any(isnan(picture.rgb)) || any(isinf(picture.rgb)))
		picture.rgb = 0;
	float2 balance = float2(asfloat(Exposure.Load(32)), asfloat(Exposure.Load(36)));
	if (all(abs(balance) < 4.0))
	{
		float before = Luminance(picture.rgb);
		picture.rgb *= exp2(float3(balance.x, 0, balance.y));
		float after = Luminance(picture.rgb);
		picture.rgb *= after > 0 ? before / after : 0;
	}
	float key = asfloat(Exposure.Load(16)), brightest = asfloat(Exposure.Load(40));
	float spread = asfloat(Exposure.Load(1344)); // (the brightness's spread, narrowed about the mean)
	if (spread >= 0.5 && spread < 0.999 && abs(key) < 20.0)
	{
		float l = Luminance(picture.rgb);
		if (l > 0)
			picture.rgb *= exp2((log2(l) - key) * (spread - 1.0));
	}
	brightest = abs(brightest) < 40.0 ? brightest : key + 7.0;
	float knee = exp2(min(key + 7.0, brightest - 0.5 - max(1.0 - Headroom, 0.0))), top = exp2(min(key + 10.0, brightest + Headroom));
	float l = Luminance(picture.rgb);
	if (l > knee)
	{
		float over = l - knee;
		picture.rgb *= (knee + over / (1.0 + over / (top - knee))) / l;
	}
	if (Decals)
		picture.rgb *= DecalsAt(p, near);
	if (Particles)
	{
		float3 t, c;
		ParticlesAt(p, t, c);
		picture.rgb = max(picture.rgb * t + c, 0);
	}
	if (foreWeight <= 0)
	{
		Output[id] = picture;
		return;
	}
	if (backWeight <= 0)
		return;
	// An edge: unmixed (see above).
	float4 game = Output[id];
	float3 F = fore / foreWeight, B = back / backWeight, D = F - B;
	float contrast = dot(D, D), level = Luminance(F) + Luminance(B) + 1e-4;
	float alpha = covered;
	if (Matte && contrast > 0.0025 * level * level)
		alpha = clamp(dot(game.rgb - B, D) / contrast, max(covered - 0.5, 0), min(covered + 0.5, 1));
	if (alpha <= 0.02)
	{
		Output[id] = picture;
		return;
	}
	if (alpha >= 0.98)
		return;
	float3 ped = clamp((game.rgb - (1 - alpha) * B) / max(alpha, 0.05), foreLow, foreHigh);
	Output[id] = float4(lerp(picture.rgb, ped, alpha), game.a);
}
)";
		constexpr UINT kViews = 10; // per frame: picture, stencil, depth, input, the particles over black and white, before and after the decals, output, exposure
		constexpr UINT kSlots = 64; // frames in the ring (frames in flight are far fewer)
		constexpr float kAdapt = 0.03f; // of the way to the measured ratio each frame (about a second)
		constexpr UINT kFlashSlots = 4;  // the flash's read-back ring (frames in flight are three at most)

		ID3D12Device* g_device = nullptr; // (the objects below belong to it)
		ID3D12RootSignature* g_root = nullptr;
		ID3D12PipelineState* g_measure = nullptr;
		ID3D12PipelineState* g_update = nullptr;
		ID3D12PipelineState* g_composite = nullptr;
		ID3D12DescriptorHeap* g_heap = nullptr;
		ID3D12Resource* g_exposure = nullptr;
		ID3D12Resource* g_flashBuffer = nullptr; // (read back: kFlashSlots floats)
		const float* g_flashMapped = nullptr;
		std::atomic<float> g_flash = 0;
		UINT g_flashNext = 0;
		UINT g_increment = 0, g_next = 0;
		bool g_failed = false;

		ID3D12PipelineState* Pipeline(ID3D12Device* device, const char* entry)
		{
			ID3DBlob* code = nullptr;
			ID3DBlob* errors = nullptr;
			if (FAILED(D3DCompile(kShader, sizeof(kShader) - 1, "composite", nullptr, nullptr, entry, "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors)))
			{
				log::Warn("ue5 composite: shader {}: {}", entry, errors ? static_cast<const char*>(errors->GetBufferPointer()) : "failed");
				if (errors)
					errors->Release();
				return nullptr;
			}
			if (errors)
				errors->Release();
			D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline{};
			pipeline.pRootSignature = g_root;
			pipeline.CS = {code->GetBufferPointer(), code->GetBufferSize()};
			ID3D12PipelineState* state = nullptr;
			device->CreateComputePipelineState(&pipeline, IID_PPV_ARGS(&state));
			code->Release();
			return state;
		}

		bool Create(ID3D12Device* device)
		{
			D3D12_DESCRIPTOR_RANGE ranges[2]{};
			ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
			ranges[0].NumDescriptors = 8;
			ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
			ranges[1].NumDescriptors = 2;
			ranges[1].OffsetInDescriptorsFromTableStart = 8;
			D3D12_ROOT_PARAMETER parameters[2]{};
			parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
			parameters[0].Constants.Num32BitValues = 16;
			parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
			parameters[1].DescriptorTable.NumDescriptorRanges = 2;
			parameters[1].DescriptorTable.pDescriptorRanges = ranges;
			D3D12_ROOT_SIGNATURE_DESC root{};
			root.NumParameters = 2;
			root.pParameters = parameters;
			ID3DBlob* serialized = nullptr;
			bool ok = SUCCEEDED(D3D12SerializeRootSignature(&root, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, nullptr)) &&
			          SUCCEEDED(device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(&g_root)));
			if (serialized)
				serialized->Release();
			ok = ok && (g_measure = Pipeline(device, "Measure")) && (g_update = Pipeline(device, "Update")) && (g_composite = Pipeline(device, "Composite"));
			if (ok)
			{
				D3D12_DESCRIPTOR_HEAP_DESC heap{};
				heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
				heap.NumDescriptors = kViews * kSlots;
				heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
				ok = SUCCEEDED(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&g_heap)));
				g_increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
			}
			if (ok)
			{
				// (zeroed when made: no scale yet)
				D3D12_HEAP_PROPERTIES heap{};
				heap.Type = D3D12_HEAP_TYPE_DEFAULT;
				D3D12_RESOURCE_DESC buffer{};
				buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
				buffer.Width = 2048;
				buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = 1;
				buffer.SampleDesc.Count = 1;
				buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
				buffer.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
				ok = SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&g_exposure)));
				heap.Type = D3D12_HEAP_TYPE_READBACK;
				buffer.Width = kFlashSlots * 4;
				buffer.Flags = D3D12_RESOURCE_FLAG_NONE;
				void* mapped = nullptr;
				ok = ok && SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
				               IID_PPV_ARGS(&g_flashBuffer))) &&
				     SUCCEEDED(g_flashBuffer->Map(0, nullptr, &mapped));
				g_flashMapped = static_cast<const float*>(mapped);
			}
			if (!ok)
			{
				log::Warn("ue5 composite: device objects could not be made");
				return false;
			}
			device->AddRef();
			g_device = device;
			log::Info("ue5 composite: ready");
			return true;
		}

		// A typed view format of a colour format (a typeless one's float view), or UNKNOWN.
		DXGI_FORMAT ColorView(DXGI_FORMAT format)
		{
			switch (format)
			{
			case DXGI_FORMAT_R16G16B16A16_TYPELESS:
				return DXGI_FORMAT_R16G16B16A16_FLOAT;
			case DXGI_FORMAT_R32G32B32A32_TYPELESS:
				return DXGI_FORMAT_R32G32B32A32_FLOAT;
			case DXGI_FORMAT_R8G8B8A8_TYPELESS:
			case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: // (read as stored: the shader decodes)
				return DXGI_FORMAT_R8G8B8A8_UNORM;
			case DXGI_FORMAT_B8G8R8A8_TYPELESS:
			case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
			case DXGI_FORMAT_B8G8R8A8_UNORM:
				return DXGI_FORMAT_B8G8R8A8_UNORM;
			case DXGI_FORMAT_R10G10B10A2_TYPELESS:
				return DXGI_FORMAT_R10G10B10A2_UNORM;
			case DXGI_FORMAT_R16G16B16A16_FLOAT:
			case DXGI_FORMAT_R32G32B32A32_FLOAT:
			case DXGI_FORMAT_R11G11B10_FLOAT:
			case DXGI_FORMAT_R8G8B8A8_UNORM:
			case DXGI_FORMAT_R10G10B10A2_UNORM:
				return format;
			default:
				return DXGI_FORMAT_UNKNOWN;
			}
		}

		// The view formats of a depth format's planes (depth, stencil), or UNKNOWN.
		bool PlaneViews(DXGI_FORMAT depth, DXGI_FORMAT& depthView, DXGI_FORMAT& stencilView)
		{
			switch (depth)
			{
			case DXGI_FORMAT_R32G8X24_TYPELESS:
			case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
				depthView = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS, stencilView = DXGI_FORMAT_X32_TYPELESS_G8X24_UINT;
				return true;
			case DXGI_FORMAT_R24G8_TYPELESS:
			case DXGI_FORMAT_D24_UNORM_S8_UINT:
				depthView = DXGI_FORMAT_R24_UNORM_X8_TYPELESS, stencilView = DXGI_FORMAT_X24_TYPELESS_G8_UINT;
				return true;
			default:
				return false;
			}
		}
	}

	float Flash()
	{
		return g_flash;
	}

	bool Record(const streamline::DlssFrame& frame, ID3D12Resource* picture, const particles::Layer& layer, const Settings& settings)
	{
		auto* output = static_cast<ID3D12Resource*>(frame.output);
		auto* depth = static_cast<ID3D12Resource*>(frame.depth);
		auto* input = static_cast<ID3D12Resource*>(frame.input);
		if (g_failed || !depth || !frame.renderWidth || !frame.renderHeight || !(frame.outputState & D3D12_RESOURCE_STATE_UNORDERED_ACCESS))
			return false;
		const D3D12_RESOURCE_DESC outputDesc = output->GetDesc(), depthDesc = depth->GetDesc();
		DXGI_FORMAT depthFormat{}, stencilFormat{};
		if (!PlaneViews(depthDesc.Format, depthFormat, stencilFormat) || !(outputDesc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS))
			return false;
		// (the game's picture before DLSS, for the edges: readable by a compute shader in the state it is left in)
		const DXGI_FORMAT inputFormat = input ? ColorView(input->GetDesc().Format) : DXGI_FORMAT_UNKNOWN;
		const bool matte = inputFormat != DXGI_FORMAT_UNKNOWN && (frame.inputState & D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		const DXGI_FORMAT layerFormat = layer.particles && layer.particlesWhite ? ColorView(layer.particles->GetDesc().Format) : DXGI_FORMAT_UNKNOWN;
		const bool particles = layerFormat != DXGI_FORMAT_UNKNOWN;
		const DXGI_FORMAT decalFormat = layer.decalBefore && layer.decalAfter ? ColorView(layer.decalBefore->GetDesc().Format) : DXGI_FORMAT_UNKNOWN;
		const bool decals = decalFormat != DXGI_FORMAT_UNKNOWN;
		if (static bool told = false; !told && layer.decalBefore)
		{
			told = true;
			log::Info("ue5 composite: the game's decals {} (format {}{})", decals ? "over the picture" : "cannot be read", static_cast<int>(layer.decalBefore->GetDesc().Format),
			    layer.decalSrgb ? ", sRGB-encoded" : "");
		}
		if (static bool told = false; !told)
		{
			told = true;
			log::Info("ue5 composite: the game's picture before DLSS {} (format {}, state {:#x})", matte ? "unmixes the edges" : "cannot be read: edges by the stencil",
			    input ? static_cast<int>(input->GetDesc().Format) : -1, frame.inputState);
		}
		if (!g_device)
		{
			ID3D12Device* device = nullptr;
			if (FAILED(output->GetDevice(IID_PPV_ARGS(&device))))
				return false;
			g_failed = !Create(device);
			device->Release();
			if (g_failed)
				return false;
		}
		// This frame's views in the next slot of the ring.
		const UINT slot = g_next++ % kSlots;
		D3D12_CPU_DESCRIPTOR_HANDLE cpu = g_heap->GetCPUDescriptorHandleForHeapStart();
		cpu.ptr += static_cast<SIZE_T>(kViews * slot) * g_increment;
		D3D12_GPU_DESCRIPTOR_HANDLE gpu = g_heap->GetGPUDescriptorHandleForHeapStart();
		gpu.ptr += static_cast<UINT64>(kViews * slot) * g_increment;
		D3D12_SHADER_RESOURCE_VIEW_DESC view{};
		view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		view.Texture2D.MipLevels = 1;
		view.Format = picture->GetDesc().Format;
		g_device->CreateShaderResourceView(picture, &view, cpu);
		cpu.ptr += g_increment;
		view.Format = stencilFormat;
		view.Texture2D.PlaneSlice = 1;
		g_device->CreateShaderResourceView(depth, &view, cpu);
		cpu.ptr += g_increment;
		view.Format = depthFormat;
		view.Texture2D.PlaneSlice = 0;
		g_device->CreateShaderResourceView(depth, &view, cpu);
		cpu.ptr += g_increment;
		// (a null view when it cannot be read: the shader does not touch it then)
		view.Format = matte ? inputFormat : DXGI_FORMAT_R16G16B16A16_FLOAT;
		g_device->CreateShaderResourceView(matte ? input : nullptr, &view, cpu);
		cpu.ptr += g_increment;
		view.Format = particles ? layerFormat : DXGI_FORMAT_R16G16B16A16_FLOAT;
		g_device->CreateShaderResourceView(particles ? layer.particles : nullptr, &view, cpu);
		cpu.ptr += g_increment;
		g_device->CreateShaderResourceView(particles ? layer.particlesWhite : nullptr, &view, cpu);
		cpu.ptr += g_increment;
		view.Format = decals ? decalFormat : DXGI_FORMAT_R8G8B8A8_UNORM;
		g_device->CreateShaderResourceView(decals ? layer.decalBefore : nullptr, &view, cpu);
		cpu.ptr += g_increment;
		g_device->CreateShaderResourceView(decals ? layer.decalAfter : nullptr, &view, cpu);
		cpu.ptr += g_increment;
		D3D12_UNORDERED_ACCESS_VIEW_DESC write{};
		write.Format = outputDesc.Format;
		write.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
		g_device->CreateUnorderedAccessView(output, nullptr, &write, cpu);
		cpu.ptr += g_increment;
		D3D12_UNORDERED_ACCESS_VIEW_DESC raw{};
		raw.Format = DXGI_FORMAT_R32_TYPELESS;
		raw.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
		raw.Buffer.NumElements = 512;
		raw.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
		g_device->CreateUnorderedAccessView(g_exposure, nullptr, &raw, cpu);

		auto* list = static_cast<ID3D12GraphicsCommandList*>(frame.commandList);
		D3D12_RESOURCE_BARRIER barriers[2]{};
		barriers[0].Type = barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
		barriers[0].UAV.pResource = output; // (after DLSS's own writes, and between the passes)
		barriers[1].UAV.pResource = g_exposure;
		list->ResourceBarrier(1, barriers);
		list->SetDescriptorHeaps(1, &g_heap);
		list->SetComputeRootSignature(g_root);
		const UINT width = static_cast<UINT>(outputDesc.Width), height = outputDesc.Height;
		const float floats[6] = {kAdapt, settings.match, settings.grade, settings.depthNumerator, settings.depthScale, settings.depthOffset};
		UINT constants[16] = {width, height, frame.renderWidth, frame.renderHeight};
		std::memcpy(&constants[4], floats, sizeof(floats));
		constants[10] = matte ? 1 : 0;
		std::memcpy(&constants[11], &settings.contrast, sizeof(float));
		constants[12] = settings.storm ? 1 : 0;
		constants[13] = particles ? 1 : 0;
		constants[14] = decals ? (layer.decalSrgb ? 2 : 1) : 0;
		std::memcpy(&constants[15], &settings.headroom, sizeof(float));
		list->SetComputeRoot32BitConstants(0, 16, constants, 0);
		list->SetComputeRootDescriptorTable(1, gpu);
		list->SetPipelineState(g_measure);
		list->Dispatch((width / 8 + 7) / 8, (height / 8 + 7) / 8, 1);
		list->ResourceBarrier(2, barriers);
		list->SetPipelineState(g_update);
		list->Dispatch(1, 1, 1);
		list->ResourceBarrier(1, &barriers[1]);
		// (the flash out to the read-back ring; the slot two frames back is done by now, or nearly: a flash is short)
		{
			const UINT flashSlot = g_flashNext++ % kFlashSlots;
			g_flash = g_flashMapped[(flashSlot + 2) % kFlashSlots];
			D3D12_RESOURCE_BARRIER copy{};
			copy.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			copy.Transition.pResource = g_exposure;
			copy.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
			copy.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
			copy.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
			list->ResourceBarrier(1, &copy);
			list->CopyBufferRegion(g_flashBuffer, flashSlot * 4, g_exposure, 44, 4);
			std::swap(copy.Transition.StateBefore, copy.Transition.StateAfter);
			list->ResourceBarrier(1, &copy);
		}
		list->SetPipelineState(g_composite);
		list->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
		list->ResourceBarrier(1, barriers);
		return true;
	}
}
