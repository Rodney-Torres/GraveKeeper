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
	virtual void BeginPlay() override;
	
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly)
	UAIPerceptionComponent* EnemyPerceptionComponent;
	
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly)
	UAISenseConfig_Sight* AISenseConfig_Sight;
	
	UFUNCTION()
	virtual void OnEnemyPerceptionUpdated(AActor* Actor, FAIStimulus Stimulus);
	
private:
	UPROPERTY(EditDefaultsOnly, Category = "Detour Crowd Avoidance Config")
	bool bEnableDetourCrowdAvoidance = true; //With this variable we can toggle our detour crowd avoidance on and off. This is mainly for comparison
	
	UPROPERTY(EditDefaultsOnly, Category = "Detour Crowd Avoidance Config", meta = (EditCondition = "bEnableDetourCrowdAvoidance", UIMin = "1", UIMax = "4")) // the edit condition meta specifier means that this variable can only be adjusted when this boolean is true, the UIMin/Max meta specifiers set the range of values in the editor for a slider
	int32 DetourCrowdAvoidanceQuality = 4; //Quality of our detour crowd avoidance from 1 (lowest) to 4 (highest)
	
	UPROPERTY(EditDefaultsOnly, Category = "Detour Crowd Avoidance Config", meta = (EditCondition = "bEnableDetourCrowdAvoidance"))
	float CollisionQueryRange = 600.f; //How far ahead the AI will look for potential collisions
};
