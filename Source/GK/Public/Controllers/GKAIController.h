//  Rodney Torres, Erik Aguiar, and Michael Hernandez All Rights

#pragma once

#include "CoreMinimal.h"
#include "AIController.h"
#include "GKAIController.generated.h"

class UAIPerceptionComponent;
class UAISenseConfig_Sight;
/**
 * 
 */
UCLASS()
class GK_API AGKAIController : public AAIController
{
	GENERATED_BODY()
	
public:
	//Constructor that allows us to override a component inside our parent class
	AGKAIController(const FObjectInitializer& ObjectInitializer);
	
	//~ Begin IGenericTeamAgentInterface Interface
	virtual ETeamAttitude::Type GetTeamAttitudeTowards(const AActor& Other) const override;
	//~ End IGenericTeamAgentInterface Interface
	
protected:
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly)
	UAIPerceptionComponent* EnemyPerceptionComponent;
	
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly)
	UAISenseConfig_Sight* AISenseConfig_Sight;
	
	UFUNCTION()
	virtual void OnEnemyPerceptionUpdated(AActor* Actor, FAIStimulus Stimulus);
};
