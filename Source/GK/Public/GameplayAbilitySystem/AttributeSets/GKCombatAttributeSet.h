//  Rodney Torres, Erik Aguiar, and Michael Hernandez All Rights

#pragma once

#include "CoreMinimal.h"
#include "AbilitySystemComponent.h"
#include "AttributeSet.h"
#include "GKCombatAttributeSet.generated.h"

UCLASS()
class GK_API UGKCombatAttributeSet : public UAttributeSet
{
	GENERATED_BODY()

public:
	UGKCombatAttributeSet();

	// Defense Attributes
	UPROPERTY(BlueprintReadOnly, Category="Attributes", meta=(ToolTip="Reduces incoming damage"))
	FGameplayAttributeData Defense;
	ATTRIBUTE_ACCESSORS_BASIC(UGKCombatAttributeSet, Defense)

	UPROPERTY(BlueprintReadOnly, Category="Attributes")
	FGameplayAttributeData MaxDefense;
	ATTRIBUTE_ACCESSORS_BASIC(UGKCombatAttributeSet, MaxDefense)

	// Strength Attributes
	UPROPERTY(BlueprintReadOnly, Category="Attributes", meta=(ToolTip="Increases damage dealt"))
	FGameplayAttributeData Strength;
	ATTRIBUTE_ACCESSORS_BASIC(UGKCombatAttributeSet, Strength)

	UPROPERTY(BlueprintReadOnly, Category="Attributes")
	FGameplayAttributeData MaxStrength;
	ATTRIBUTE_ACCESSORS_BASIC(UGKCombatAttributeSet, MaxStrength)

	// Attribute change hooks
	virtual void PreAttributeBaseChange(const FGameplayAttribute& Attribute, float& NewValue) const override;
	virtual void PostGameplayEffectExecute(const struct FGameplayEffectModCallbackData& Data) override;
};