//  Rodney Torres, Erik Aguiar, and Michael Hernandez All Rights


#include "AI/BTT_RotateToFaceTarget.h"

UBTT_RotateToFaceTarget::UBTT_RotateToFaceTarget()
{
	NodeName = TEXT("Native Rotate to Face Target Actor");
	AnglePrecision = 10.0f;
	RotationInterpSpeed = 5.0f;

	bNotifyTick = true;
	bNotifyTaskFinished = true;
	bCreateNodeInstance = false;
	
	INIT_TASK_NODE_NOTIFY_FLAGS();

	InTargetToFaceKey.AddObjectFilter(this, GET_MEMBER_NAME_CHECKED(ThisClass, InTargetToFaceKey), AActor::StaticClass());
}

void UBTT_RotateToFaceTarget::InitializeFromAsset(UBehaviorTree& Asset)
{
	Super::InitializeFromAsset(Asset);

	if (UBlackboardData* BBAsset = GetBlackboardAsset())
	{
		InTargetToFaceKey.ResolveSelectedKey(*BBAsset);
	}
}

uint16 UBTT_RotateToFaceTarget::GetInstanceMemorySize() const
{
	return sizeof(FRotateToFaceTargetTaskMemory);
}

FString UBTT_RotateToFaceTarget::GetStaticDescription() const
{
	const FString KeyDescription = InTargetToFaceKey.SelectedKeyName.ToString();

	return FString::Printf(TEXT("Smoothly rotates to face %s Key until the angle precision: %s is reached"), *KeyDescription, *FString::SanitizeFloat(AnglePrecision));
}
