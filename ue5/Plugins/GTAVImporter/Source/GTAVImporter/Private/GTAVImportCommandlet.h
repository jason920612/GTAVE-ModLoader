#pragma once

#include "Commandlets/Commandlet.h"

#include "GTAVImportCommandlet.generated.h"

// UnrealEditor-Cmd.exe GTAVRenderer.uproject -run=GTAVImport -export=<ue5export's folder>
UCLASS()
class UGTAVImportCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UGTAVImportCommandlet();
	virtual int32 Main(const FString& Params) override;
};
