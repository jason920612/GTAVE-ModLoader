#include "composite.hpp"

#include <Windows.h>
#include <d3dcompiler.h>

#include <cstring>

#include "../log.hpp"

namespace loader::graphics::composite
{
	namespace
	{
		// Stencil at DLSS time (the low three bits a kind): 1 peds, 2 vehicles, the 0x80 bit the player (0 buildings
		// and roads, 3 trees, 4 grass and terrain). Each output pixel takes the renderer's picture, the game's own where
		// those are, blended by how much of the bilinear footprint in the render-size stencil they cover.
		// The renderer's picture is scaled to the game's brightness first: Measure sums the log luminance of both
		// pictures over a sparse grid (not the sky: depth 0, reversed; not peds and vehicles), Update moves the scale a
		// little towards their ratio each frame, Composite applies it. The game's own exposure and tone mapping come
		// after, as for its own scene.
		constexpr char kShader[] = R"(
Texture2D<float4> Picture : register(t0);
Texture2D<uint2> Stencil : register(t1);
Texture2D<float> Depth : register(t2);
RWTexture2D<float4> Output : register(u0);
RWByteAddressBuffer Exposure : register(u1); // 0 game sum, 4 renderer sum (log2 * 64), 8 count, 12 scale (float)
cbuffer Sizes : register(b0) { uint2 OutSize; uint2 RenderSize; float Adapt; float Match; };

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

groupshared int g_game[64];
groupshared int g_renderer[64];
groupshared int g_count[64];

[numthreads(8, 8, 1)]
void Measure(uint2 id : SV_DispatchThreadID, uint index : SV_GroupIndex)
{
	uint2 p = id * 8 + 4;
	int game = 0, renderer = 0, count = 0;
	if (all(p < OutSize))
	{
		int2 q = RenderPixel(p);
		if (Depth.Load(int3(q, 0)) > 0 && !Dynamic(Stencil.Load(int3(q, 0)).y))
		{
			game = int(clamp(log2(max(Luminance(Output[p].rgb), 1e-4)), -14.0, 14.0) * 64);
			renderer = int(clamp(log2(max(Luminance(Picture.Load(int3(p, 0)).rgb), 1e-4)), -14.0, 14.0) * 64);
			count = 1;
		}
	}
	g_game[index] = game;
	g_renderer[index] = renderer;
	g_count[index] = count;
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
		Exposure.InterlockedAdd(0, uint(g_game[0]), ignored);
		Exposure.InterlockedAdd(4, uint(g_renderer[0]), ignored);
		Exposure.InterlockedAdd(8, uint(g_count[0]), ignored);
	}
}

[numthreads(1, 1, 1)]
void Update()
{
	int game = int(Exposure.Load(0)), renderer = int(Exposure.Load(4)), count = int(Exposure.Load(8));
	float scale = asfloat(Exposure.Load(12));
	if (count > 64)
	{
		float target = clamp((float(game) - float(renderer)) / (64.0 * count), -6.0, 6.0) * Match; // log2 of the ratio
		float current = scale > 0 ? log2(scale) : target;
		scale = exp2(lerp(current, target, Adapt));
	}
	Exposure.Store4(0, uint4(0, 0, 0, asuint(scale > 0 ? scale : 1.0)));
}

[numthreads(8, 8, 1)]
void Composite(uint2 id : SV_DispatchThreadID)
{
	if (any(id >= OutSize))
		return;
	float2 p = (float2(id) + 0.5) * float2(RenderSize) / float2(OutSize) - 0.5;
	int2 b = int2(floor(p));
	float2 f = p - float2(b);
	float covered = 0;
	[unroll] for (int k = 0; k < 4; ++k)
	{
		int2 q = clamp(b + int2(k & 1, k >> 1), int2(0, 0), int2(RenderSize) - 1);
		float w = ((k & 1) ? f.x : 1 - f.x) * ((k >> 1) ? f.y : 1 - f.y);
		covered += Dynamic(Stencil.Load(int3(q, 0)).y) ? w : 0;
	}
	float scale = asfloat(Exposure.Load(12));
	float4 picture = Picture.Load(int3(id, 0)) * float4(scale.xxx, 1);
	if (covered <= 0.001)
		Output[id] = picture;
	else if (covered < 0.999)
		Output[id] = lerp(picture, Output[id], covered);
}
)";
		constexpr UINT kViews = 5;  // per frame: picture, stencil, depth, output, exposure
		constexpr UINT kSlots = 64; // frames in the ring (frames in flight are far fewer)
		constexpr float kAdapt = 0.03f; // of the way to the measured ratio each frame (about a second)

		ID3D12Device* g_device = nullptr; // (the objects below belong to it)
		ID3D12RootSignature* g_root = nullptr;
		ID3D12PipelineState* g_measure = nullptr;
		ID3D12PipelineState* g_update = nullptr;
		ID3D12PipelineState* g_composite = nullptr;
		ID3D12DescriptorHeap* g_heap = nullptr;
		ID3D12Resource* g_exposure = nullptr;
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
			ranges[0].NumDescriptors = 3;
			ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
			ranges[1].NumDescriptors = 2;
			ranges[1].OffsetInDescriptorsFromTableStart = 3;
			D3D12_ROOT_PARAMETER parameters[2]{};
			parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
			parameters[0].Constants.Num32BitValues = 6;
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
				buffer.Width = 256;
				buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = 1;
				buffer.SampleDesc.Count = 1;
				buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
				buffer.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
				ok = SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&g_exposure)));
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

	bool Record(const streamline::DlssFrame& frame, ID3D12Resource* picture, float match)
	{
		auto* output = static_cast<ID3D12Resource*>(frame.output);
		auto* depth = static_cast<ID3D12Resource*>(frame.depth);
		if (g_failed || !depth || !frame.renderWidth || !frame.renderHeight || !(frame.outputState & D3D12_RESOURCE_STATE_UNORDERED_ACCESS))
			return false;
		const D3D12_RESOURCE_DESC outputDesc = output->GetDesc(), depthDesc = depth->GetDesc();
		DXGI_FORMAT depthFormat{}, stencilFormat{};
		if (!PlaneViews(depthDesc.Format, depthFormat, stencilFormat) || !(outputDesc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS))
			return false;
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
		D3D12_UNORDERED_ACCESS_VIEW_DESC write{};
		write.Format = outputDesc.Format;
		write.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
		g_device->CreateUnorderedAccessView(output, nullptr, &write, cpu);
		cpu.ptr += g_increment;
		D3D12_UNORDERED_ACCESS_VIEW_DESC raw{};
		raw.Format = DXGI_FORMAT_R32_TYPELESS;
		raw.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
		raw.Buffer.NumElements = 64;
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
		const float adapt = kAdapt;
		UINT constants[6] = {width, height, frame.renderWidth, frame.renderHeight, 0, 0};
		std::memcpy(&constants[4], &adapt, 4);
		std::memcpy(&constants[5], &match, 4);
		list->SetComputeRoot32BitConstants(0, 6, constants, 0);
		list->SetComputeRootDescriptorTable(1, gpu);
		list->SetPipelineState(g_measure);
		list->Dispatch((width / 8 + 7) / 8, (height / 8 + 7) / 8, 1);
		list->ResourceBarrier(2, barriers);
		list->SetPipelineState(g_update);
		list->Dispatch(1, 1, 1);
		list->ResourceBarrier(1, &barriers[1]);
		list->SetPipelineState(g_composite);
		list->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
		list->ResourceBarrier(1, barriers);
		return true;
	}
}
