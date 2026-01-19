//  Rodney Torres, Erik Aguiar, and Michael Hernandez All Rights

#pragma once

#include "CoreMinimal.h"
#include "Abilities/GameplayAbility.h"
#include "GKGameplayAbility.generated.h"

// --- Ability Input ID Enumeration ---
UENUM(BlueprintType)
enum class EAbilityInputID: uint8
{
	None UMETA(DisplayName = "None"),
	PrimaryAbility UMETA(DisplayName = "PrimaryAbility"),
	SecondaryAbility UMETA(DisplayName = "SecondaryAbility"),
	DefensiveAbility UMETA(DisplayName = "DefensiveAbility"),
	MovementAbility UMETA(DisplayName = "MovementAbility"),
	SpecialAbility UMETA(DisplayName = "SpecialAbility"),
};

UCLASS()
class GK_API UGKGameplayAbility : public UGameplayAbility
{
	GENERATED_BODY()

	UGKGameplayAbility();

public:
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Input")
	EAbilityInputID AbilityInputID = EAbilityInputID::None;
};
