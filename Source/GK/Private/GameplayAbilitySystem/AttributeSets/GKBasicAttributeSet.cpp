//  Rodney Torres, Erik Aguiar, and Michael Hernandez All Rights

#include "GameplayAbilitySystem/AttributeSets/GKBasicAttributeSet.h"

#include "AbilitySystemBlueprintLibrary.h"
#include "GameplayEffectExtension.h"

UGKBasicAttributeSet::UGKBasicAttributeSet()
{
	Health = 100.f;
	MaxHealth = 100.f;
	Damage = 0.f;
	HealCharges = 3;
	MaxHealCharges = 3;
	Souls = 0.f;
	MaxSouls = 99999.f;
}

// --- Clamp health to its maximum value before changes ---
void UGKBasicAttributeSet::PreAttributeBaseChange(const FGameplayAttribute& Attribute, float& NewValue) const
{
	Super::PreAttributeBaseChange(Attribute, NewValue);
	
	if (Attribute == GetHealthAttribute()) // Clamp health to its maximum value
	{
		NewValue = FMath::Clamp(NewValue, 0.f, GetMaxHealth());
	}
	if (Attribute == GetHealChargesAttribute()) // Clamp health charges to its max
	{
		NewValue = FMath::Clamp(NewValue, 0.f, GetMaxHealCharges());
	}
	if (Attribute == GetSoulsAttribute()) // Clamp currency to its max
	{
		NewValue = FMath::Clamp(NewValue, 0.f, GetMaxSouls());
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
		if (TotalDamage > 0.f) SetHealth(GetHealth() - TotalDamage);

		// AActor* SourceActor = Data.EffectSpec.GetContext().GetEffectCauser();
		// AActor* TargetActor = GetOwningActor();

		// TODO: Activate hit reaction ability if health was modified
		// if (Data.EffectSpec.Def->GetAssetTags().HasTag(FGameplayTag::RequestGameplayTag("Effects.HitReaction"))
		// 	&& Data.EvaluatedData.Magnitude != 0.f)
		// {
		// 	FGameplayTagContainer HitReactionTagContainer;
		// 	HitReactionTagContainer.AddTag(FGameplayTag::RequestGameplayTag("GameplayAbility.HitReaction"));
		// 	GetOwningAbilitySystemComponent()->TryActivateAbilitiesByTag(HitReactionTagContainer);
		// }
		
		// Get Damage Direction
		// if (SourceActor && TargetActor)
		// {
		// 	FGameplayEventData EventData;
		// 	EventData.EventTag = FGameplayTag::RequestGameplayTag("Event.UI.DamageIndicator");
		// 	EventData.Instigator = SourceActor;
		// 	EventData.Target = TargetActor;
		// 	EventData.EventMagnitude = TotalDamage;

		// 	UAbilitySystemBlueprintLibrary::SendGameplayEventToActor(TargetActor, EventData.EventTag, EventData);
		// }
	}

	// Update attributes after Gameplay Effect execution
	if (Data.EvaluatedData.Attribute == GetHealthAttribute())
	{
		SetHealth(GetHealth());
	}
	if (Data.EvaluatedData.Attribute == GetHealChargesAttribute())
	{
		SetHealCharges(GetHealCharges());
	}
	if (Data.EvaluatedData.Attribute == GetSoulsAttribute())
	{
		SetSouls(GetSouls());
	}
}

// --- Additional logic after attribute base changes ---
void UGKBasicAttributeSet::PostAttributeBaseChange(const FGameplayAttribute& Attribute, float OldValue, float NewValue) const
{
	Super::PostAttributeBaseChange(Attribute, OldValue, NewValue);

#pragma region DamageIndicator (Disabled)
	#if 0
	// Gameplay Cue for Damage Indicator
	// if (Attribute == GetHealthAttribute())
	// {
	// 	// Trigger cue if health was modified
	// 	if (NewValue < OldValue)
	// 	{
	// 		if (GetPlayerController()) // Only trigger if we have a valid player controller
	// 		{
	// 			FGameplayCueParameters Params;
	// 			Params.RawMagnitude = OldValue - NewValue;
	// 			Params.Instigator = GetOwningActor();
	// 			GetOwningAbilitySystemComponent()-> ExecuteGameplayCue(
	// 				FGameplayTag::RequestGameplayTag(FName("GameplayCue.DamageIndicator")),
	// 				Params
	// 			);
	// 		}
	// 	}
	// }
	#endif
#pragma endregion

	// Logic for attribute changes like handling Death Ability
}

// --- Helper function to get the player controller from the owning actor ---
APlayerController* UGKBasicAttributeSet::GetPlayerController() const
{
	AActor* OwningActor = GetOwningActor();
	if (APawn* OwnerPawn = Cast<APawn>(OwningActor))
	{
		return Cast<APlayerController>(OwnerPawn->GetController());
	}
	return nullptr;
}