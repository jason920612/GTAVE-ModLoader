#include "GTAVPack.h"

#include "Algo/BinarySearch.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/Paths.h"

FGTAVPackFile::~FGTAVPackFile()
{
	delete Handle;
}

bool FGTAVPackFile::Open(const FString& File, uint32 Kind, FString& OutError)
{
	Handle = FPlatformFileManager::Get().GetPlatformFile().OpenRead(*File);
	GTAVPack::FileHeader Header{};
	if (!Handle || !Handle->Read(reinterpret_cast<uint8*>(&Header), sizeof(Header)))
	{
		OutError = FString::Printf(TEXT("cannot read %s"), *File);
		return false;
	}
	if (FMemory::Memcmp(Header.magic, "GPK1", 4) != 0 || Header.kind != Kind || Header.version != GTAVPack::kVersion)
	{
		OutError = FString::Printf(TEXT("%s: not a pack of kind %u, version %u (ue5export pack writes them)"), *File, Kind, GTAVPack::kVersion);
		return false;
	}
	Table.SetNumUninitialized(Header.entryCount);
	if (!Handle->Seek(Header.tableOffset) || !Handle->Read(reinterpret_cast<uint8*>(Table.GetData()), int64(Table.Num()) * sizeof(GTAVPack::Entry)))
	{
		OutError = FString::Printf(TEXT("%s: no table"), *File);
		return false;
	}
	return true;
}

const GTAVPack::Entry* FGTAVPackFile::Find(uint64 Key) const
{
	const int32 Index = Algo::LowerBoundBy(Table, Key, &GTAVPack::Entry::key);
	return Table.IsValidIndex(Index) && Table[Index].key == Key ? &Table[Index] : nullptr;
}

bool FGTAVPackFile::Read(uint64 Key, TArray<uint8>& Out) const
{
	const GTAVPack::Entry* E = Find(Key);
	if (!E)
	{
		return false;
	}
	Out.SetNumUninitialized(E->size);
	FScopeLock Scope(&Lock);
	return Handle->Seek(E->offset) && Handle->Read(Out.GetData(), E->size);
}

bool FGTAVPack::Open(const FString& Folder, FString& OutError)
{
	return Models.Open(FPaths::Combine(Folder, TEXT("models.gpk")), GTAVPack::kModels, OutError) &&
		Textures.Open(FPaths::Combine(Folder, TEXT("textures.gpk")), GTAVPack::kTextures, OutError) &&
		World.Open(FPaths::Combine(Folder, TEXT("world.gpk")), GTAVPack::kWorld, OutError);
}
