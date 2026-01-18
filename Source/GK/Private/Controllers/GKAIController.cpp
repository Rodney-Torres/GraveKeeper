//  Rodney Torres, Erik Aguiar, and Michael Hernandez All Rights


#include "Controllers/GKAIController.h"
#include "Navigation/CrowdFollowingComponent.h"
#include "Perception/AIPerceptionComponent.h"
#include "Perception/AISenseConfig_Sight.h"
#include "BehaviorTree/BlackboardComponent.h"

#include "GKDebugHelper.h"

AGKAIController::AGKAIController(const FObjectInitializer& ObjectInitializer)
	//Inside this initializer list we override the path following component with our crowd following component.
	: Super(ObjectInitializer.SetDefaultSubobjectClass<UCrowdFollowingComponent>("PathFollowingComponent"))
{
	//We retrieve our path following component with the overridden type and save it in a local variable then if this is valid we print a message to test
	if (UCrowdFollowingComponent* CrowdComp = Cast<UCrowdFollowingComponent>(GetPathFollowingComponent()))
	{
		Debug::Print(TEXT("CrowdFollowingComponent valid"), FColor::Green);
	}
	
	//How to construct our component and save it as our AISenseConfig_Sight. Then we set its properties.
	AISenseConfig_Sight = CreateDefaultSubobject<UAISenseConfig_Sight>("EnemySenseConfig_Sight");
	AISenseConfig_Sight->DetectionByAffiliation.bDetectEnemies = true;
	AISenseConfig_Sight->DetectionByAffiliation.bDetectFriendlies = false;
	AISenseConfig_Sight->DetectionByAffiliation.bDetectNeutrals = false;
	AISenseConfig_Sight->SightRadius = 5000.f;
	AISenseConfig_Sight->LoseSightRadius = 0.f; //Once the player is seen we don't want to lose sight of them. We should change this later probably
	AISenseConfig_Sight->PeripheralVisionAngleDegrees = 360.f; //Full vision, a stealth game would change this
	
	EnemyPerceptionComponent = CreateDefaultSubobject<UAIPerceptionComponent>("EnemyPerceptionComponent");
	EnemyPerceptionComponent->ConfigureSense(*AISenseConfig_Sight); //telling our perception component to use the sight config we just created
	EnemyPerceptionComponent->SetDominantSense(UAISenseConfig_Sight::StaticClass()); //Priority when multiple senses are used
	EnemyPerceptionComponent->OnTargetPerceptionUpdated.AddUniqueDynamic(this, &ThisClass::OnEnemyPerceptionUpdated); //Binding our function to the perception updated delegate
	
	SetGenericTeamId(FGenericTeamId(1)); //team id our enemy AI will use
}

ETeamAttitude::Type AGKAIController::GetTeamAttitudeTowards(const AActor& Other) const
{
	const APawn* PawnToCheck = Cast<const APawn>(&Other);
	
	const IGenericTeamAgentInterface* OtherTeamAgent = Cast<const IGenericTeamAgentInterface>(PawnToCheck->GetController()); //casting from the controller and the type were casting to is IGenericTeamAgentInterface
	
	if (OtherTeamAgent && OtherTeamAgent->GetGenericTeamId() != GetGenericTeamId()) //If the other actor does not have the same team ID, we consider them hostile
	{
		return ETeamAttitude::Hostile;
	}
	return ETeamAttitude::Friendly;
}


void AGKAIController::OnEnemyPerceptionUpdated(AActor* Actor, FAIStimulus Stimulus)
{
	if (Stimulus.WasSuccessfullySensed() && Actor) // if inside an actor was sensed by our AI
	{
		if (UBlackboardComponent* BlackboardComponent = GetBlackboardComponent()) //if we have a get a valid blackboard component
		{
			BlackboardComponent->SetValueAsObject(FName("TargetActor"), Actor); //we set the value of our blackboard key TargetActor to the actor that was sensed
		}
	}
}
