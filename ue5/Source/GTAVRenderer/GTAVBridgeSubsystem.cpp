#include "GTAVBridgeSubsystem.h"

#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "Camera/PlayerCameraManager.h"
#include "Components/DirectionalLightComponent.h"
#include "Engine/DirectionalLight.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "UnrealClient.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "GlobalShader.h"
#include "GTAVBridgeShaders.h"
#include "PixelShaderUtils.h"
#include "HAL/IConsoleManager.h"
#include "ID3D12DynamicRHI.h"
#include "Misc/CommandLine.h"
#include "PostProcess/PostProcessMaterialInputs.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RenderTargetPool.h"
#include "SceneViewExtension.h"
#include "ScreenPass.h"

#include "Windows/AllowWindowsPlatformTypes.h"
#include <d3d12.h>
#include "Windows/HideWindowsPlatformTypes.h"

#include "GTAVBridgeProtocol.h"
#include "GTAVPack.h"
#include "GTAVShadows.h"
#include "GTAVSky.h"
#include "GTAVWorldSubsystem.h"

DEFINE_LOG_CATEGORY_STATIC(LogGTAVBridge, Log, All);

// The picture to the game: after motion blur (HDR, before bloom and tone mapping), drawn into the shared texture of its
// game frame with its depth, then the shared fence signalled with the game frame (both on UE's graphics queue, in that
// order). Each frame is drawn with its game frame's camera, set as the view point itself (whatever the camera manager
// did that tick), and labelled on the render thread in order with its own rendering (the game thread is a frame ahead).
class FGTAVBridgeViewExtension : public FSceneViewExtensionBase
{
public:
	FGTAVBridgeViewExtension(const FAutoRegister& AutoRegister, GTAVBridge::Shared* InShared)
		: FSceneViewExtensionBase(AutoRegister), Shared(InShared)
	{
	}
	virtual ~FGTAVBridgeViewExtension() override
	{
		Release();
	}

	// Game thread: the game frame drawn next and its camera (Unreal's space; horizontal field of view, degrees).
	void SetView(uint32 InSequence, const FVector& Location, const FRotator& Rotation, float Fov, float Aspect)
	{
		ViewSequence = InSequence;
		ViewLocation = Location;
		ViewRotation = Rotation;
		ViewFov = Fov;
		ViewAspect = Aspect;
	}

	virtual void SetupViewFamily(FSceneViewFamily& InViewFamily) override {}
	virtual void SetupView(FSceneViewFamily& InViewFamily, FSceneView& InView) override {}
	virtual void SetupViewPoint(APlayerController* Player, FMinimalViewInfo& InViewInfo) override
	{
		if (ViewSequence)
		{
			InViewInfo.Location = ViewLocation;
			InViewInfo.Rotation = ViewRotation;
			InViewInfo.FOV = ViewFov;
			InViewInfo.AspectRatio = ViewAspect;
			InViewInfo.bConstrainAspectRatio = false;
		}
	}
	virtual void BeginRenderViewFamily(FSceneViewFamily& InViewFamily) override
	{
		// (ahead of this family's rendering on the render thread)
		const uint32 S = ViewSequence;
		ENQUEUE_RENDER_COMMAND(GTAVBridgeFrame)([this, S](FRHICommandListImmediate&) { RenderSequence = S; });
	}

	virtual void SubscribeToPostProcessingPass(EPostProcessingPass Pass, const FSceneView& InView, FPostProcessingPassDelegateArray& InOutPassCallbacks,
		bool bIsPassEnabled) override
	{
		if (Pass == EPostProcessingPass::MotionBlur)
		{
			InOutPassCallbacks.Add(FPostProcessingPassDelegate::CreateRaw(this, &FGTAVBridgeViewExtension::AfterMotionBlur));
		}
	}

	// Game thread: the shared texture and fence for this size (made, named, published).
	bool Make(uint32 GamePid, uint32 Width, uint32 Height)
	{
		FlushRenderingCommands();
		Release();
		ID3D12DynamicRHI* RHI = GetID3D12DynamicRHI();
		ID3D12Device* Device = RHI ? RHI->RHIGetDevice(0) : nullptr;
		if (!Device)
		{
			return false;
		}
		D3D12_HEAP_PROPERTIES Heap{};
		Heap.Type = D3D12_HEAP_TYPE_DEFAULT;
		D3D12_RESOURCE_DESC Desc{};
		Desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		Desc.Width = Width;
		Desc.Height = Height;
		Desc.DepthOrArraySize = 1;
		Desc.MipLevels = 1;
		Desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
		Desc.SampleDesc.Count = 1;
		Desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
		const FString Pid = FString::FromInt(GamePid);
		bool bOk = SUCCEEDED(Device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&Fence))) &&
				   SUCCEEDED(Device->CreateSharedHandle(Fence, nullptr, GENERIC_ALL, *(FString(GTAVBridge::kFenceName) + Pid), &FenceHandle)) &&
				   SUCCEEDED(Device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&Consumed))) &&
				   SUCCEEDED(Device->CreateSharedHandle(Consumed, nullptr, GENERIC_ALL, *(FString(GTAVBridge::kConsumedName) + Pid), &ConsumedHandle));
		for (int32 I = 0; I < int32(GTAVBridge::kTextures) && bOk; ++I)
		{
			bOk = SUCCEEDED(Device->CreateCommittedResource(&Heap, D3D12_HEAP_FLAG_SHARED, &Desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&Resource[I]))) &&
				  SUCCEEDED(Device->CreateSharedHandle(Resource[I], nullptr, GENERIC_ALL, *FString::Printf(TEXT("%s%s-%d"), GTAVBridge::kTextureName, *Pid, I),
					  &TextureHandle[I]));
			if (bOk)
			{
				Texture[I] = RHI->RHICreateTexture2DFromResource(PF_FloatRGBA, TexCreate_RenderTargetable | TexCreate_ShaderResource, FClearValueBinding::None, Resource[I]);
				Target[I] = CreateRenderTarget(Texture[I], TEXT("GTAVBridgeFrame"));
			}
		}
		if (!bOk)
		{
			UE_LOG(LogGTAVBridge, Error, TEXT("could not make the shared textures / fence (%ux%u)"), Width, Height);
			Release();
			return false;
		}
		Shared->textureWidth = Width;
		Shared->textureHeight = Height;
		Shared->frameValue = 0;
		for (uint32 I = 0; I < GTAVBridge::kTextures; ++I)
		{
			Shared->frameSequences[I] = 0;
		}
		FPlatformMisc::MemoryBarrier();
		++Shared->textureSequence;
		UE_LOG(LogGTAVBridge, Display, TEXT("shared texture %ux%u"), Width, Height);
		return true;
	}

	void Release()
	{
		for (int32 I = 0; I < int32(GTAVBridge::kTextures); ++I)
		{
			Target[I].SafeRelease();
			Texture[I].SafeRelease();
			if (Resource[I])
			{
				Resource[I]->Release();
				Resource[I] = nullptr;
			}
		}
		for (ID3D12Fence** F : {&Fence, &Consumed})
		{
			if (*F)
			{
				(*F)->Release();
				*F = nullptr;
			}
		}
		for (HANDLE* H : {&TextureHandle[0], &TextureHandle[1], &TextureHandle[2], &FenceHandle, &ConsumedHandle})
		{
			if (*H)
			{
				CloseHandle(*H);
				*H = nullptr;
			}
		}
	}

	// The game frames the game has presented (its texture is free again), on any thread.
	uint64 ConsumedValue() const
	{
		return Consumed ? Consumed->GetCompletedValue() : ~0ull;
	}

	std::atomic<bool> bEnabled = false;

private:
	GTAVBridge::Shared* Shared;
	ID3D12Resource* Resource[GTAVBridge::kTextures] = {};
	ID3D12Fence* Fence = nullptr;
	ID3D12Fence* Consumed = nullptr;
	HANDLE TextureHandle[GTAVBridge::kTextures] = {}, FenceHandle = nullptr, ConsumedHandle = nullptr;
	FTextureRHIRef Texture[GTAVBridge::kTextures];
	TRefCountPtr<IPooledRenderTarget> Target[GTAVBridge::kTextures];
	// Game thread: the next frame's game frame and camera.
	uint32 ViewSequence = 0;
	FVector ViewLocation = FVector::ZeroVector;
	FRotator ViewRotation = FRotator::ZeroRotator;
	float ViewFov = 90.0f, ViewAspect = 16.0f / 9.0f;
	// Render thread: the game frame of the frame being rendered, and of the last one given to the game.
	uint32 RenderSequence = 0, LastWritten = 0;

	FScreenPassTexture AfterMotionBlur(FRDGBuilder& GraphBuilder, const FSceneView& View, const FPostProcessMaterialInputs& Inputs)
	{
		const FScreenPassTexture SceneColor = FScreenPassTexture::CopyFromSlice(GraphBuilder, Inputs.GetInput(EPostProcessMaterialInput::SceneColor));
		const uint32 FrameSequence = RenderSequence;
		// (once per game frame: a frame drawn again while no new view came is not the game's)
		if (Target[GTAVBridge::kTextures - 1] && Fence && bEnabled && FrameSequence && FrameSequence != LastWritten && SceneColor.IsValid() &&
			SceneColor.ViewRect.Width() == int32(Shared->textureWidth) && SceneColor.ViewRect.Height() == int32(Shared->textureHeight) &&
			Inputs.SceneTextures.SceneTextures)
		{
			LastWritten = FrameSequence;
			const uint32 Slot = FrameSequence % GTAVBridge::kTextures;
			FRDGTextureRef Dest = GraphBuilder.RegisterExternalTexture(Target[Slot], TEXT("GTAVBridgeFrame"));
			// The colour, and in alpha the depth (metres) for the game's peds and vehicles to be hidden behind.
			// (the depth's rectangle is the view's own, at the render resolution: the shader has it from the view)
			FRDGTextureRef Depth = Inputs.SceneTextures.SceneTextures->GetParameters()->SceneDepthTexture;
			FGTAVBridgePS::FParameters* P = GraphBuilder.AllocParameters<FGTAVBridgePS::FParameters>();
			P->View = View.ViewUniformBuffer;
			P->SceneColor = SceneColor.Texture;
			P->SceneDepth = Depth;
			P->ColorMin = SceneColor.ViewRect.Min;
			P->ColorSize = SceneColor.ViewRect.Size();
			P->RenderTargets[0] = FRenderTargetBinding(Dest, ERenderTargetLoadAction::ENoAction);
			FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(View.GetFeatureLevel());
			TShaderMapRef<FGTAVBridgePS> Shader(ShaderMap);
			FPixelShaderUtils::AddFullscreenPass(GraphBuilder, ShaderMap, RDG_EVENT_NAME("GTAVBridgeFrame"), Shader, P,
				FIntRect(0, 0, SceneColor.ViewRect.Width(), SceneColor.ViewRect.Height()));
			static bool bLogged = false;
			if (!bLogged)
			{
				bLogged = true;
				UE_LOG(LogGTAVBridge, Display, TEXT("scene colour %s %dx%d (view %dx%d at %d,%d); depth %dx%d"), GPixelFormats[SceneColor.Texture->Desc.Format].Name,
					SceneColor.Texture->Desc.Extent.X, SceneColor.Texture->Desc.Extent.Y, SceneColor.ViewRect.Width(), SceneColor.ViewRect.Height(),
					SceneColor.ViewRect.Min.X, SceneColor.ViewRect.Min.Y, Depth->Desc.Extent.X, Depth->Desc.Extent.Y);
			}
			GTAVBridge::Shared* S = Shared;
			ID3D12Fence* F = Fence;
			// (after the graph, on the immediate list: behind the drawing, on the same queue; the game waits for this value
			// before using the texture)
			GraphBuilder.AddPostExecuteCallback([S, F, FrameSequence, Slot] {
				// (as the engine does it: while the RHI thread translates the list, where a command context is active)
				FRHICommandListExecutor::GetImmediateCommandList().EnqueueLambda([S, F, FrameSequence, Slot](FRHICommandList& RHICmdList) {
					GetID3D12DynamicRHI()->RHISignalManualFence(RHICmdList, F, FrameSequence);
					S->frameSequences[Slot] = FrameSequence;
					FPlatformMisc::MemoryBarrier();
					S->frameValue = FrameSequence;
				});
			});
		}
		return Inputs.ReturnUntouchedSceneColorForPostProcessing(GraphBuilder);
	}
};

bool UGTAVBridgeSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	const UWorld* World = Cast<UWorld>(Outer);
	return World && World->IsGameWorld() && FString(FCommandLine::Get()).Contains(TEXT("-gtavbridge="));
}

void UGTAVBridgeSubsystem::OnWorldBeginPlay(UWorld& World)
{
	Super::OnWorldBeginPlay(World);
	FParse::Value(FCommandLine::Get(), TEXT("-gtavbridge="), GamePid);
	const FString Name = FString(GTAVBridge::kMemoryName) + FString::FromInt(GamePid);
	Mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, false, *Name);
	Shared = Mapping ? static_cast<GTAVBridge::Shared*>(MapViewOfFile(Mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(GTAVBridge::Shared))) : nullptr;
	if (!Shared || Shared->magic != GTAVBridge::kMagic || Shared->version != GTAVBridge::kVersion)
	{
		UE_LOG(LogGTAVBridge, Error, TEXT("no bridge %s (or another version)"), *Name);
		Shared = nullptr;
		return;
	}
	Camera = World.SpawnActor<ACameraActor>();
	Camera->GetCameraComponent()->bConstrainAspectRatio = false;
	Extension = FSceneViewExtensions::NewExtension<FGTAVBridgeViewExtension>(Shared);
	// (one picture per game frame; motion blur and depth of field are the game's)
	for (const TCHAR* Command : {TEXT("r.MotionBlurQuality 0"), TEXT("r.DepthOfFieldQuality 0"), TEXT("t.MaxFPS 0"), TEXT("r.VSync 0")})
	{
		GEngine->Exec(&World, Command);
	}
	UE_LOG(LogGTAVBridge, Display, TEXT("bridge to the game (process %u)"), GamePid);
}

void UGTAVBridgeSubsystem::Deinitialize()
{
	// (render commands still to run may refer to the extension)
	FlushRenderingCommands();
	Extension.Reset();
	if (Shared)
	{
		Shared->rendererState = 0;
		UnmapViewOfFile(Shared);
		Shared = nullptr;
	}
	if (Mapping)
	{
		CloseHandle(Mapping);
		Mapping = nullptr;
	}
	Super::Deinitialize();
}

void UGTAVBridgeSubsystem::Resize(uint32 Width, uint32 Height)
{
	GEngine->Exec(GetWorld(), *FString::Printf(TEXT("r.SetRes %ux%uw"), Width, Height));
	if (Extension->Make(GamePid, Width, Height))
	{
		Shared->rendererState = 1;
	}
	else
	{
		Shared->rendererState = 2;
	}
}

void UGTAVBridgeSubsystem::Tick(float DeltaTime)
{
	if (!Shared)
	{
		return;
	}
	// One frame per game frame, each with its own camera, in order (the newest when far behind): wait (briefly) for the
	// next one.
	const double Until = FPlatformTime::Seconds() + 0.1;
	while (int32(Shared->gameSequence - NextSequence) < 0 && FPlatformTime::Seconds() < Until)
	{
		FPlatformProcess::SleepNoStats(0.0005f);
	}
	const uint32 Newest = Shared->gameSequence;
	if (!Newest || int32(Newest - NextSequence) < 0)
	{
		return;
	}
	FPlatformMisc::MemoryBarrier();
	if (!NextSequence || Newest - NextSequence >= GTAVBridge::kViews - 1)
	{
		NextSequence = Newest;
	}
	GTAVBridge::View View = Shared->views[NextSequence % GTAVBridge::kViews];
	if (View.sequence != NextSequence)
	{
		NextSequence = Newest;
		View = Shared->views[Newest % GTAVBridge::kViews];
	}
	const uint32 Sequence = NextSequence++;
	if (int32(Sequence - LastSequence) > 1)
	{
		++Skipped;
	}
	LastSequence = Sequence;
	if (Shared->width && Shared->height && (Shared->width != Shared->textureWidth || Shared->height != Shared->textureHeight || !Shared->textureSequence))
	{
		Resize(Shared->width, Shared->height);
	}
	// Console commands the loader sent (tuning while the game runs).
	if (Shared->consoleSequence != LastConsole)
	{
		LastConsole = Shared->consoleSequence;
		TArray<FString> Lines;
		FString(UTF8_TO_TCHAR(Shared->console)).ParseIntoArrayLines(Lines);
		for (const FString& Line : Lines)
		{
			if (!Line.TrimStartAndEnd().IsEmpty() && !Line.StartsWith(TEXT("#")))
			{
				UE_LOG(LogGTAVBridge, Display, TEXT("console: %s"), *Line);
				GEngine->Exec(GetWorld(), *Line);
			}
		}
	}
	Extension->bEnabled = Shared->enabled != 0;
	// (the texture this frame goes into is the game's until it has presented the frame that was there: briefly)
	if (Shared->enabled && Sequence > GTAVBridge::kTextures)
	{
		const double Wait = FPlatformTime::Seconds() + 0.05;
		while (Extension->ConsumedValue() + GTAVBridge::kTextures < Sequence && FPlatformTime::Seconds() < Wait)
		{
			FPlatformProcess::SleepNoStats(0.0002f);
		}
	}

	// The camera the game rendered that frame with: its position (metres), forward and up (the game's space, y mirrored
	// here), its vertical field of view.
	const FVector Forward(View.forward[0], -View.forward[1], View.forward[2]), Up(View.up[0], -View.up[1], View.up[2]);
	const FRotator Rotation = FRotationMatrix::MakeFromXZ(Forward, Up).Rotator();
	const FVector Location(View.position[0] * 100, -View.position[1] * 100, View.position[2] * 100);
	const float Aspect = View.aspect > 0 ? View.aspect : Shared->height ? float(Shared->width) / Shared->height : 16.0f / 9.0f;
	const float Fov = FMath::RadiansToDegrees(2 * FMath::Atan(FMath::Tan(FMath::DegreesToRadians(View.fov) / 2) * Aspect));
	Extension->SetView(Sequence, Location, Rotation, Fov, Aspect);
	Camera->SetActorLocationAndRotation(Location, Rotation);
	Camera->GetCameraComponent()->SetFieldOfView(Fov);
	if (APlayerController* PC = GetWorld()->GetFirstPlayerController())
	{
		if (PC->GetViewTarget() != Camera)
		{
			PC->SetViewTarget(Camera);
		}
		// (the camera manager keeps its own 90 degrees otherwise)
		if (PC->PlayerCameraManager)
		{
			PC->PlayerCameraManager->SetFOV(Camera->GetCameraComponent()->FieldOfView);
		}
	}
	static double Told = 0;
	if (FPlatformTime::Seconds() - Told > 5)
	{
		Told = FPlatformTime::Seconds();
		UE_LOG(LogGTAVBridge, Display, TEXT("game camera %.2f %.2f %.2f forward %.3f %.3f %.3f fov %.1f; hours %.2f; weather %08x -> %08x %.2f; interior %d room %08x; frame %u, %u skipped so far"),
			View.position[0], View.position[1], View.position[2], View.forward[0], View.forward[1], View.forward[2], View.fov, Shared->hours,
			Shared->weatherFrom, Shared->weatherTo, Shared->weatherBlend, Shared->interior, Shared->room, Sequence, Skipped);
		{
			FString Vehicles;
			for (uint32 I = 0; I < FMath::Min(Shared->proxyCount, GTAVBridge::kMaxProxies); ++I)
			{
				if (Shared->proxies[I].kind == 2 && Vehicles.Len() < 200)
				{
					Vehicles += FString::Printf(TEXT(" (%.0f %.0f %.0f)"), Shared->proxies[I].position[0], Shared->proxies[I].position[1], Shared->proxies[I].position[2]);
				}
			}
			UE_LOG(LogGTAVBridge, Display, TEXT("shadow proxies: %u; vehicles%s"), Shared->proxyCount, *Vehicles);
		}
		if (const APlayerController* PC = GetWorld()->GetFirstPlayerController(); PC && PC->PlayerCameraManager)
		{
			UE_LOG(LogGTAVBridge, Display, TEXT("view fov %.2f (camera component %.2f), viewport %s"), PC->PlayerCameraManager->GetFOVAngle(),
				Camera->GetCameraComponent()->FieldOfView, GEngine->GameViewport ? *GEngine->GameViewport->Viewport->GetSizeXY().ToString() : TEXT("-"));
		}
	}

	// The sun by the hour, the weather, the wind.
	if (Shared->windSpeed > 0)
	{
		GTAVSky::SetWind(FVector2f(Shared->wind[0], -Shared->wind[1]), Shared->windSpeed);
	}
	if (Shared->year > 0 && Shared->month > 0 && Shared->day > 0)
	{
		GTAVSky::SetDate(int32(Shared->year), int32(Shared->month), int32(Shared->day));
	}
	GTAVSky::SetIndoors(Shared->interior != 0, true);
	GTAVSky::Apply(GetWorld(), Camera->GetActorLocation(), Shared->hours, Shared->weatherFrom, Shared->weatherTo, Shared->weatherBlend);
	GTAVShadows::Update(GetWorld(), Shared->proxies, Shared->proxyCount);

	// The world: the hour (timed archetypes, groups by the hour) and the map data groups the game has on.
	if (UGTAVWorldSubsystem* World = GetWorld()->GetSubsystem<UGTAVWorldSubsystem>())
	{
		World->SetHour(FMath::Clamp(int32(Shared->hours), 0, 23));
		if (Shared->groupsSequence != LastGroups)
		{
			LastGroups = Shared->groupsSequence;
			TSet<uint32> On;
			for (uint32 I = 0; I < FMath::Min<uint32>(Shared->groupCount, GTAVBridge::kMaxGroups); ++I)
			{
				On.Add(Shared->groupsOn[I]);
			}
			World->SetGroupsOn(On);
		}
	}
}
