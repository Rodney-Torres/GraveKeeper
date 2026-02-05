//  Rodney Torres, Erik Aguiar, and Michael Hernandez All Rights

#include "GameplayAbilitySystem/AttributeSets/GKCombatAttributeSet.h"
#include "GameplayEffectExtension.h"

UGKCombatAttributeSet::UGKCombatAttributeSet()
{
	Strength = 0.f;
	MaxStrength = 100.f;
	Defense = 0.f;
	MaxDefense = 100.f;
}

void UGKCombatAttributeSet::PreAttributeBaseChange(const FGameplayAttribute& Attribute, float& NewValue) const
{
	Super::PreAttributeBaseChange(Attribute, NewValue);

	// Clamp Defense and Strength to their maximum values
	if (Attribute == GetDefenseAttribute())
	{
		NewValue = FMath::Clamp(NewValue, 0.f, GetMaxDefense());
	}
	else if (Attribute == GetStrengthAttribute())
	{
		NewValue = FMath::Clamp(NewValue, 0.f, GetMaxStrength());
	}
}

void UGKCombatAttributeSet::PostGameplayEffectExecute(const struct FGameplayEffectModCallbackData& Data)
{
	Super::PostGameplayEffectExecute(Data);

	// Update Defense and Strength after gameplay effect execution
	if (Data.EvaluatedData.Attribute == GetDefenseAttribute())
	{
		SetDefense(GetDefense());
	}
	else if (Data.EvaluatedData.Attribute == GetStrengthAttribute())
	{
		SetStrength(GetStrength());
	}
}