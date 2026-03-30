//  Rodney Torres, Erik Aguiar, and Michael Hernandez All Rights

#include "GameplayAbilitySystem/AttributeSets/GKBasicAttributeSet.h"

#include "AbilitySystemBlueprintLibrary.h"
#include "GameplayCueNotifyTypes.h"
#include "GameplayEffectExtension.h"

UGKBasicAttributeSet::UGKBasicAttributeSet()
{
	Health = 100.f;
	MaxHealth = 100.f;
	Damage = 0.f;
	HealCharges = 3;
	MaxHealCharges = 3;
}

// --- Clamp health to its maximum value before changes ---
void UGKBasicAttributeSet::PreAttributeBaseChange(const FGameplayAttribute& Attribute, float& NewValue) const
{
	Super::PreAttributeBaseChange(Attribute, NewValue);

	// Clamp health to its maximum value
	if (Attribute == GetHealthAttribute())
	{
		NewValue = FMath::Clamp(NewValue, 0.f, GetMaxHealth());
	}
	if (Attribute == GetHealChargesAttribute())
	{
		NewValue = FMath::Clamp(NewValue, 0.f, GetMaxHealCharges());
	}
}

// --- Handle damage application and health updates ---
void UGKBasicAttributeSet::PostGameplayEffectExecute(const struct FGameplayEffectModCallbackData& Data)
{
	Super::PostGameplayEffectExecute(Data);

	// Apply damage
	if (Data.EvaluatedData.Attribute == GetDamageAttribute())
	{
		float TotalDamage = GetDamage();
		SetDamage(0.f);

		// Subtract damage from health
		SetHealth(GetHealth() - TotalDamage);

		AActor* SourceActor = Data.EffectSpec.GetContext().GetEffectCauser();
		AActor* TargetActor = GetOwningActor();

		// TODO: Activate hit reaction ability if health was modified
		// if (Data.EffectSpec.Def->GetAssetTags().HasTag(FGameplayTag::RequestGameplayTag("Effects.HitReaction"))
		// 	&& Data.EvaluatedData.Magnitude != 0.f)
		// {
		// 	FGameplayTagContainer HitReactionTagContainer;
		// 	HitReactionTagContainer.AddTag(FGameplayTag::RequestGameplayTag("GameplayAbility.HitReaction"));
		// 	GetOwningAbilitySystemComponent()->TryActivateAbilitiesByTag(HitReactionTagContainer);
		// }

		// Get Damage Direction
		if (SourceActor && TargetActor)
		{
			FGameplayEventData EventData;
			EventData.EventTag = FGameplayTag::RequestGameplayTag("Event.UI.DamageIndicator");
			EventData.Instigator = SourceActor;
			EventData.Target = TargetActor;
			EventData.EventMagnitude = TotalDamage;

			UAbilitySystemBlueprintLibrary::SendGameplayEventToActor(TargetActor, EventData.EventTag, EventData);
		}
	}

	// Update health after gameplay effect execution
	if (Data.EvaluatedData.Attribute == GetHealthAttribute())
	{
		SetHealth(GetHealth());
	}
	// Update healing charges after gameplay effect execution
	if (Data.EvaluatedData.Attribute == GetHealChargesAttribute())
	{
		SetHealCharges(GetHealCharges());
	}
}

// --- Additional logic after attribute base changes ---
void UGKBasicAttributeSet::PostAttributeBaseChange(const FGameplayAttribute& Attribute, float OldValue, float NewValue) const
{
	Super::PostAttributeBaseChange(Attribute, OldValue, NewValue);

	// Logic for attribute changes like handling Death Ability
}