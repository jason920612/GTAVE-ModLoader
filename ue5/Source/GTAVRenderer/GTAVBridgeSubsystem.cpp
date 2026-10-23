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
#include "GTAVWorldSubsystem.h"

DEFINE_LOG_CATEGORY_STATIC(LogGTAVBridge, Log, All);

// The picture to the game: after motion blur (HDR, before bloom and tone mapping), copied into the shared texture,
// then the shared fence signalled with the frame's value (both on UE's graphics queue, in that order).
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

	virtual void SetupViewFamily(FSceneViewFamily& InViewFamily) override {}
	virtual void SetupView(FSceneViewFamily& InViewFamily, FSceneView& InView) override {}
	virtual void BeginRenderViewFamily(FSceneViewFamily& InViewFamily) override {}

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
		if (FAILED(Device->CreateCommittedResource(&Heap, D3D12_HEAP_FLAG_SHARED, &Desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&Resource))) ||
			FAILED(Device->CreateSharedHandle(Resource, nullptr, GENERIC_ALL, *(FString(GTAVBridge::kTextureName) + Pid), &TextureHandle)) ||
			FAILED(Device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&Fence))) ||
			FAILED(Device->CreateSharedHandle(Fence, nullptr, GENERIC_ALL, *(FString(GTAVBridge::kFenceName) + Pid), &FenceHandle)))
		{
			UE_LOG(LogGTAVBridge, Error, TEXT("could not make the shared texture / fence (%ux%u)"), Width, Height);
			Release();
			return false;
		}
		Texture = RHI->RHICreateTexture2DFromResource(PF_FloatRGBA, TexCreate_RenderTargetable | TexCreate_ShaderResource, FClearValueBinding::None, Resource);
		Target = CreateRenderTarget(Texture, TEXT("GTAVBridgeFrame"));
		Shared->textureWidth = Width;
		Shared->textureHeight = Height;
		Shared->frameValue = 0;
		FPlatformMisc::MemoryBarrier();
		++Shared->textureSequence;
		UE_LOG(LogGTAVBridge, Display, TEXT("shared texture %ux%u"), Width, Height);
		return true;
	}

	void Release()
	{
		Target.SafeRelease();
		Texture.SafeRelease();
		if (Resource)
		{
			Resource->Release();
			Resource = nullptr;
		}
		if (Fence)
		{
			Fence->Release();
			Fence = nullptr;
		}
		for (HANDLE* H : {&TextureHandle, &FenceHandle})
		{
			if (*H)
			{
				CloseHandle(*H);
				*H = nullptr;
			}
		}
	}

	// The game frame being drawn (game thread, read on the render thread).
	std::atomic<uint32> Sequence = 0;
	std::atomic<bool> bEnabled = false;

private:
	GTAVBridge::Shared* Shared;
	ID3D12Resource* Resource = nullptr;
	ID3D12Fence* Fence = nullptr;
	HANDLE TextureHandle = nullptr, FenceHandle = nullptr;
	FTextureRHIRef Texture;
	TRefCountPtr<IPooledRenderTarget> Target;
	uint64 Value = 0;

	FScreenPassTexture AfterMotionBlur(FRDGBuilder& GraphBuilder, const FSceneView& View, const FPostProcessMaterialInputs& Inputs)
	{
		const FScreenPassTexture SceneColor = FScreenPassTexture::CopyFromSlice(GraphBuilder, Inputs.GetInput(EPostProcessMaterialInput::SceneColor));
		if (Target && Fence && bEnabled && SceneColor.IsValid() && SceneColor.ViewRect.Width() == int32(Shared->textureWidth) &&
			SceneColor.ViewRect.Height() == int32(Shared->textureHeight))
		{
			FRDGTextureRef Dest = GraphBuilder.RegisterExternalTexture(Target, TEXT("GTAVBridgeFrame"));
			// (drawn, not copied: the scene colour's format need not be the shared texture's)
			FRDGDrawTextureInfo Draw;
			Draw.SourcePosition = SceneColor.ViewRect.Min;
			Draw.Size = SceneColor.ViewRect.Size();
			AddDrawTexturePass(GraphBuilder, GetGlobalShaderMap(View.GetFeatureLevel()), SceneColor.Texture, Dest, Draw);
			static bool bLogged = false;
			if (!bLogged)
			{
				bLogged = true;
				UE_LOG(LogGTAVBridge, Display, TEXT("scene colour %s %dx%d (view %dx%d at %d,%d)"), GPixelFormats[SceneColor.Texture->Desc.Format].Name,
					SceneColor.Texture->Desc.Extent.X, SceneColor.Texture->Desc.Extent.Y, SceneColor.ViewRect.Width(), SceneColor.ViewRect.Height(),
					SceneColor.ViewRect.Min.X, SceneColor.ViewRect.Min.Y);
			}
			const uint64 FrameValue = ++Value;
			const uint32 FrameSequence = Sequence;
			GTAVBridge::Shared* S = Shared;
			ID3D12Fence* F = Fence;
			// (after the graph, on the immediate list: behind the copy, on the same queue; the game waits for this value
			// before using the texture)
			GraphBuilder.AddPostExecuteCallback([S, F, FrameValue, FrameSequence] {
				// (as the engine does it: while the RHI thread translates the list, where a command context is active)
				FRHICommandListExecutor::GetImmediateCommandList().EnqueueLambda([S, F, FrameValue, FrameSequence](FRHICommandList& RHICmdList) {
					GetID3D12DynamicRHI()->RHISignalManualFence(RHICmdList, F, FrameValue);
					S->frameSequence = FrameSequence;
					FPlatformMisc::MemoryBarrier();
					S->frameValue = FrameValue;
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
	// One frame per game frame: wait (briefly) for the game's next view.
	const double Until = FPlatformTime::Seconds() + 0.1;
	while (Shared->gameSequence == LastSequence && FPlatformTime::Seconds() < Until)
	{
		FPlatformProcess::SleepNoStats(0.0005f);
	}
	const uint32 Sequence = Shared->gameSequence;
	if (Sequence == LastSequence)
	{
		return;
	}
	LastSequence = Sequence;
	FPlatformMisc::MemoryBarrier();
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
	Extension->Sequence = Sequence;
	Extension->bEnabled = Shared->enabled != 0;

	// The camera the game rendered with: its position (metres), forward and up (the game's space, y mirrored here), its
	// vertical field of view.
	const FVector Forward(Shared->forward[0], -Shared->forward[1], Shared->forward[2]), Up(Shared->up[0], -Shared->up[1], Shared->up[2]);
	const FRotator Rotation = FRotationMatrix::MakeFromXZ(Forward, Up).Rotator();
	Camera->SetActorLocationAndRotation(FVector(Shared->position[0] * 100, -Shared->position[1] * 100, Shared->position[2] * 100), Rotation);
	const float Aspect = Shared->aspect > 0 ? Shared->aspect : Shared->height ? float(Shared->width) / Shared->height : 16.0f / 9.0f;
	Camera->GetCameraComponent()->SetFieldOfView(FMath::RadiansToDegrees(2 * FMath::Atan(FMath::Tan(FMath::DegreesToRadians(Shared->fov) / 2) * Aspect)));
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
		UE_LOG(LogGTAVBridge, Display, TEXT("game camera %.2f %.2f %.2f forward %.3f %.3f %.3f fov %.1f; hours %.2f; weather %08x -> %08x %.2f; view %s %s"),
			Shared->position[0], Shared->position[1], Shared->position[2], Shared->forward[0], Shared->forward[1], Shared->forward[2], Shared->fov, Shared->hours,
			Shared->weatherFrom, Shared->weatherTo, Shared->weatherBlend, *Camera->GetActorLocation().ToString(), *Camera->GetActorRotation().ToString());
		if (const APlayerController* PC = GetWorld()->GetFirstPlayerController(); PC && PC->PlayerCameraManager)
		{
			UE_LOG(LogGTAVBridge, Display, TEXT("view fov %.2f (camera component %.2f), viewport %s"), PC->PlayerCameraManager->GetFOVAngle(),
				Camera->GetCameraComponent()->FieldOfView, GEngine->GameViewport ? *GEngine->GameViewport->Viewport->GetSizeXY().ToString() : TEXT("-"));
		}
	}

	// The sun from the hour: east at 6, highest at 12 (to the south), west at 18.
	if (!Sun)
	{
		for (TActorIterator<ADirectionalLight> It(GetWorld()); It; ++It)
		{
			Sun = *It;
		}
	}
	if (Sun)
	{
		const float Angle = (Shared->hours - 6.0f) / 12.0f * PI;
		const FVector Toward = FVector(FMath::Cos(Angle), FMath::Sin(Angle) * 0.5f, FMath::Sin(Angle) * 0.866f).GetSafeNormal(); // (y: south, mirrored)
		Sun->SetActorRotation((-Toward).Rotation());
		Sun->GetLightComponent()->SetIntensity(Toward.Z > -0.05f ? 10.0f : 0.0f);
	}

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
