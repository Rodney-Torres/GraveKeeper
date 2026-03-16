//  Rodney Torres, Erik Aguiar, and Michael Hernandez All Rights

#pragma once

#include "CoreMinimal.h"
#include "AttributeSet.h"
#include "AbilitySystemComponent.h"
#include "GKBasicAttributeSet.generated.h"

UCLASS()
class GK_API UGKBasicAttributeSet : public UAttributeSet
{
	GENERATED_BODY()

public:
	UGKBasicAttributeSet();

	// Health Attributes
	UPROPERTY(BlueprintReadOnly, Category="Attributes")
	FGameplayAttributeData Health;
	ATTRIBUTE_ACCESSORS_BASIC(UGKBasicAttributeSet, Health)

	UPROPERTY(BlueprintReadOnly, Category="Attributes")
	FGameplayAttributeData MaxHealth;
	ATTRIBUTE_ACCESSORS_BASIC(UGKBasicAttributeSet, MaxHealth)

	// Healing Charges
	UPROPERTY(BlueprintReadOnly, Category="Attributes")
	FGameplayAttributeData HealCharges;
	ATTRIBUTE_ACCESSORS_BASIC(UGKBasicAttributeSet, HealCharges)

	UPROPERTY(BlueprintReadOnly, Category="Attributes")
	FGameplayAttributeData MaxHealCharges;
	ATTRIBUTE_ACCESSORS_BASIC(UGKBasicAttributeSet, MaxHealCharges)

	// Damage Attribute
	UPROPERTY(BlueprintReadOnly, Category="Attributes")
	FGameplayAttributeData Damage;
	ATTRIBUTE_ACCESSORS_BASIC(UGKBasicAttributeSet, Damage)

public:
	// Attribute Change Hooks
	virtual void PreAttributeBaseChange(const FGameplayAttribute& Attribute, float& NewValue) const override;

	virtual void PostGameplayEffectExecute(const struct FGameplayEffectModCallbackData& Data) override;
	
	virtual void PostAttributeBaseChange(const FGameplayAttribute& Attribute, float OldValue, float NewValue) const override;
};