//  Rodney Torres, Erik Aguiar, and Michael Hernandez All Rights

#include "GKGameplayAbility.h"

UGKGameplayAbility::UGKGameplayAbility()
{
    // Define default ability tags
    ActivationOwnedTags.AddTag(FGameplayTag::RequestGameplayTag(FName("GameplayAbility.Active")));
    ActivationBlockedTags.AddTag(FGameplayTag::RequestGameplayTag(FName("State.Dead")));
}

// Check if the avatar actor has a player controller
bool UGKGameplayAbility::HasPC() const
{
    const APawn* PawnObject = Cast<APawn>(GetAvatarActorFromActorInfo());
    if (!PawnObject) return false;

    const AController* Controller = PawnObject->GetController();
    return Controller && Controller->IsA<APlayerController>();
}