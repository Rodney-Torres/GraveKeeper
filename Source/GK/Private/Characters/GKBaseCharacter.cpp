//  Rodney Torres, Erik Aguiar, and Michael Hernandez All Rights

#include "Characters/GKBaseCharacter.h"
#include "GameplayAbilitySystem/AttributeSets/GKBasicAttributeSet.h"
#include "GameplayAbilitySystem/AttributeSets/GKCombatAttributeSet.h"

// Sets default values
AGKBaseCharacter::AGKBaseCharacter()
{
	// Set this character to call Tick() every frame.  You can turn this off to improve performance if you don't need it.
	PrimaryActorTick.bCanEverTick = true;

	// Create the Ability System Component and Attribute Sets
	GKAbilitySystemComponent = CreateDefaultSubobject<UAbilitySystemComponent>(TEXT("GKAbilitySystemComponent"));
	GKBasicAttributeSet = CreateDefaultSubobject<UGKBasicAttributeSet>(TEXT("GKBasicAttributeSet"));
	GKCombatAttributeSet = CreateDefaultSubobject<UGKCombatAttributeSet>(TEXT("GKCombatAttributeSet"));

}

UAbilitySystemComponent* AGKBaseCharacter::GetAbilitySystemComponent() const
{
	return GKAbilitySystemComponent;
}

// Grant abilities to the character
TArray<FGameplayAbilitySpecHandle> AGKBaseCharacter::GrantAbilities(
	TArray<TSubclassOf<UGameplayAbility>> AbilitiesToGrant)
{
	if (!GKAbilitySystemComponent)
	{
		return TArray<FGameplayAbilitySpecHandle>();
	}

	// Grant each ability and store the handles 
	TArray<FGameplayAbilitySpecHandle> AbilityHandles;
	for (TSubclassOf<UGameplayAbility> Ability : AbilitiesToGrant)
	{
		FGameplayAbilitySpecHandle SpecHandle = GKAbilitySystemComponent->GiveAbility(FGameplayAbilitySpec(
			Ability, 1, -1, this)
		);
		AbilityHandles.Add(SpecHandle);
	}
	
	return AbilityHandles;
}

void AGKBaseCharacter::RemoveAbilities(TArray<FGameplayAbilitySpecHandle> AbilityHandlesToRemove)
{
	if (!GKAbilitySystemComponent)
	{
		return;
	}

	for (FGameplayAbilitySpecHandle& SpecHandle : AbilityHandlesToRemove)
	{
		GKAbilitySystemComponent->ClearAbility(SpecHandle);
	}
}

// Called when the game starts or when spawned
void AGKBaseCharacter::BeginPlay()
{
	Super::BeginPlay();
}

// Called when the character is possessed by a controller 
void AGKBaseCharacter::PossessedBy(AController* NewController)
{
	Super::PossessedBy(NewController);

	// Initialize and grant abilities
	if (GKAbilitySystemComponent)
	{
		GKAbilitySystemComponent->InitAbilityActorInfo(this, this);
		GrantAbilities(StartingAbilities);
	}
}

// Called every frame
void AGKBaseCharacter::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
}

// Called to bind functionality to input
void AGKBaseCharacter::SetupPlayerInputComponent(UInputComponent* PlayerInputComponent)
{
	Super::SetupPlayerInputComponent(PlayerInputComponent);
}