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
		constexpr char kShader[] = R"(
Texture2D<float4> Picture : register(t0);
Texture2D<uint2> Stencil : register(t1);
RWTexture2D<float4> Output : register(u0);
cbuffer Sizes : register(b0) { uint2 OutSize; uint2 RenderSize; };

bool Dynamic(uint s)
{
	uint kind = s & 7;
	return (s & 0x80) != 0 || kind == 1 || kind == 2;
}

[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID)
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
	float4 picture = Picture.Load(int3(id, 0));
	if (covered <= 0.001)
		Output[id] = picture;
	else if (covered < 0.999)
		Output[id] = lerp(picture, Output[id], covered);
}
)";
		constexpr UINT kSlots = 64; // descriptor sets in the ring (frames in flight are far fewer)

		ID3D12Device* g_device = nullptr; // (the objects below belong to it)
		ID3D12RootSignature* g_root = nullptr;
		ID3D12PipelineState* g_pipeline = nullptr;
		ID3D12DescriptorHeap* g_heap = nullptr;
		UINT g_increment = 0, g_next = 0;
		bool g_failed = false;

		bool Create(ID3D12Device* device)
		{
			ID3DBlob* code = nullptr;
			ID3DBlob* errors = nullptr;
			if (FAILED(D3DCompile(kShader, sizeof(kShader) - 1, "composite", nullptr, nullptr, "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code,
			        &errors)))
			{
				log::Warn("ue5 composite: shader: {}", errors ? static_cast<const char*>(errors->GetBufferPointer()) : "failed");
				if (errors)
					errors->Release();
				return false;
			}
			if (errors)
				errors->Release();
			D3D12_DESCRIPTOR_RANGE ranges[2]{};
			ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
			ranges[0].NumDescriptors = 2;
			ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
			ranges[1].NumDescriptors = 1;
			ranges[1].OffsetInDescriptorsFromTableStart = 2;
			D3D12_ROOT_PARAMETER parameters[2]{};
			parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
			parameters[0].Constants.Num32BitValues = 4;
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
			if (ok)
			{
				D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline{};
				pipeline.pRootSignature = g_root;
				pipeline.CS = {code->GetBufferPointer(), code->GetBufferSize()};
				ok = SUCCEEDED(device->CreateComputePipelineState(&pipeline, IID_PPV_ARGS(&g_pipeline)));
			}
			code->Release();
			if (ok)
			{
				D3D12_DESCRIPTOR_HEAP_DESC heap{};
				heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
				heap.NumDescriptors = 3 * kSlots;
				heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
				ok = SUCCEEDED(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&g_heap)));
				g_increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
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

		// The stencil plane's view format for a depth format, or UNKNOWN.
		DXGI_FORMAT StencilView(DXGI_FORMAT depth)
		{
			switch (depth)
			{
			case DXGI_FORMAT_R32G8X24_TYPELESS:
			case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
				return DXGI_FORMAT_X32_TYPELESS_G8X24_UINT;
			case DXGI_FORMAT_R24G8_TYPELESS:
			case DXGI_FORMAT_D24_UNORM_S8_UINT:
				return DXGI_FORMAT_X24_TYPELESS_G8_UINT;
			default:
				return DXGI_FORMAT_UNKNOWN;
			}
		}
	}

	bool Record(const streamline::DlssFrame& frame, ID3D12Resource* picture)
	{
		auto* output = static_cast<ID3D12Resource*>(frame.output);
		auto* depth = static_cast<ID3D12Resource*>(frame.depth);
		if (g_failed || !depth || !frame.renderWidth || !frame.renderHeight || !(frame.outputState & D3D12_RESOURCE_STATE_UNORDERED_ACCESS))
			return false;
		const D3D12_RESOURCE_DESC outputDesc = output->GetDesc(), depthDesc = depth->GetDesc();
		const DXGI_FORMAT stencilFormat = StencilView(depthDesc.Format);
		if (stencilFormat == DXGI_FORMAT_UNKNOWN || !(outputDesc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS))
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
		// This frame's three views (picture, stencil, output) in the next slot of the ring.
		const UINT slot = g_next++ % kSlots;
		D3D12_CPU_DESCRIPTOR_HANDLE cpu = g_heap->GetCPUDescriptorHandleForHeapStart();
		cpu.ptr += static_cast<SIZE_T>(3 * slot) * g_increment;
		D3D12_GPU_DESCRIPTOR_HANDLE gpu = g_heap->GetGPUDescriptorHandleForHeapStart();
		gpu.ptr += static_cast<UINT64>(3 * slot) * g_increment;
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
		D3D12_UNORDERED_ACCESS_VIEW_DESC write{};
		write.Format = outputDesc.Format;
		write.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
		g_device->CreateUnorderedAccessView(output, nullptr, &write, cpu);

		auto* list = static_cast<ID3D12GraphicsCommandList*>(frame.commandList);
		D3D12_RESOURCE_BARRIER barrier{};
		barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
		barrier.UAV.pResource = output; // (after DLSS's own writes)
		list->ResourceBarrier(1, &barrier);
		list->SetDescriptorHeaps(1, &g_heap);
		list->SetComputeRootSignature(g_root);
		list->SetPipelineState(g_pipeline);
		const UINT sizes[4] = {static_cast<UINT>(outputDesc.Width), outputDesc.Height, frame.renderWidth, frame.renderHeight};
		list->SetComputeRoot32BitConstants(0, 4, sizes, 0);
		list->SetComputeRootDescriptorTable(1, gpu);
		list->Dispatch((sizes[0] + 7) / 8, (sizes[1] + 7) / 8, 1);
		list->ResourceBarrier(1, &barrier);
		return true;
	}
}
