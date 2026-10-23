#pragma once

#include "CoreMinimal.h"
#include "GTAVPackFormat.h"

class IFileHandle;

// One of the pack files tools/ue5export writes (GTAVPackFormat.h): its table, and its entries read on request.
class FGTAVPackFile
{
public:
	~FGTAVPackFile();
	bool Open(const FString& File, uint32 Kind, FString& OutError);
	bool Contains(uint64 Key) const { return Find(Key) != nullptr; }
	// An entry's bytes (thread-safe).
	bool Read(uint64 Key, TArray<uint8>& Out) const;
	int32 Num() const { return Table.Num(); }

private:
	const GTAVPack::Entry* Find(uint64 Key) const;
	TArray<GTAVPack::Entry> Table;
	IFileHandle* Handle = nullptr;
	mutable FCriticalSection Lock;
};

// The three pack files of a folder.
struct FGTAVPack
{
	FGTAVPackFile Models, Textures, World;
	bool Open(const FString& Folder, FString& OutError);
};
