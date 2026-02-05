//  Rodney Torres, Erik Aguiar, and Michael Hernandez All Rights

#include "GKGameplayAbility.h"

UGKGameplayAbility::UGKGameplayAbility()
{
    // Define default ability tags
    ActivationOwnedTags.AddTag(FGameplayTag::RequestGameplayTag(FName("GameplayAbility.Active")));
    ActivationBlockedTags.AddTag(FGameplayTag::RequestGameplayTag(FName("State.Dead")));
}