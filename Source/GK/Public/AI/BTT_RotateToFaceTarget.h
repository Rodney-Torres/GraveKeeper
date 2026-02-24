//  Rodney Torres, Erik Aguiar, and Michael Hernandez All Rights

#pragma once

#include "CoreMinimal.h"
#include "BehaviorTree/BTTaskNode.h"
#include "BTT_RotateToFaceTarget.generated.h"

struct FRotateToFaceTargetTaskMemory
{
	TWeakObjectPtr<APawn> OwningPawn;
	TWeakObjectPtr<AActor> AttackTarget;

	bool IsValid() const
	{
		return OwningPawn.IsValid() && AttackTarget.IsValid();
	}

	void Reset()
	{
		OwningPawn.Reset();
		AttackTarget.Reset();
	}
};

/**
 * 
 */
UCLASS()
class GK_API UBTT_RotateToFaceTarget : public UBTTaskNode
{
	GENERATED_BODY()
		
	UBTT_RotateToFaceTarget();

	//~ Begin UBTNode interface
	virtual void InitializeFromAsset(UBehaviorTree& Asset) override;
	virtual uint16 GetInstanceMemorySize() const override;
	virtual FString GetStaticDescription() const override;
	//~ End UBTNode interface

	UPROPERTY(EditAnywhere, Category = "Face Target")
	float AnglePrecision;

	UPROPERTY(EditAnywhere, Category = "Face Target")
	float RotationInterpSpeed;

	UPROPERTY(EditAnywhere, Category = "Face Target")
	FBlackboardKeySelector InTargetToFaceKey;
};
