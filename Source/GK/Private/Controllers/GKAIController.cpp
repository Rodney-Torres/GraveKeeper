//  Rodney Torres, Erik Aguiar, and Michael Hernandez All Rights


#include "Controllers/GKAIController.h"
#include "Navigation/CrowdFollowingComponent.h"
#include "Perception/AIPerceptionComponent.h"
#include "Perception/AISenseConfig_Sight.h"
#include "BehaviorTree/BlackboardComponent.h"
#include "Perception/AISenseConfig_Hearing.h"

#include "GKDebugHelper.h"

AGKAIController::AGKAIController(const FObjectInitializer& ObjectInitializer)
	//Inside this initializer list we override the path following component with our crowd following component.
	: Super(ObjectInitializer.SetDefaultSubobjectClass<UCrowdFollowingComponent>("PathFollowingComponent"))
{
	
	//How to construct our component and save it as our AISenseConfig_Sight. Then we set its properties.
	AISenseConfig_Sight = CreateDefaultSubobject<UAISenseConfig_Sight>("EnemySenseConfig_Sight");
	AISenseConfig_Sight->DetectionByAffiliation.bDetectEnemies = true;
	AISenseConfig_Sight->DetectionByAffiliation.bDetectFriendlies = false;
	AISenseConfig_Sight->DetectionByAffiliation.bDetectNeutrals = false;
	AISenseConfig_Sight->SightRadius = 5000.f;
	AISenseConfig_Sight->LoseSightRadius = 0.f; //Once the player is seen we don't want to lose sight of them. We should change this later probably
	AISenseConfig_Sight->PeripheralVisionAngleDegrees = 360.f; //Full vision, a stealth game would change this
	AISenseConfig_Sight->SetMaxAge(5.f);
	
	AISenseConfig_Hearing = CreateDefaultSubobject<UAISenseConfig_Hearing>("EnemySenseConfig_Hearing");
	AISenseConfig_Hearing->DetectionByAffiliation.bDetectEnemies = true;
	AISenseConfig_Hearing->DetectionByAffiliation.bDetectFriendlies = false;
	AISenseConfig_Hearing->DetectionByAffiliation.bDetectNeutrals = false;
	AISenseConfig_Hearing->HearingRange = 500.f;
	AISenseConfig_Hearing->SetMaxAge(5.f);
	AISenseConfig_Hearing->SetStartsEnabled(true);
	
	EnemyPerceptionComponent = CreateDefaultSubobject<UAIPerceptionComponent>("EnemyPerceptionComponent");
	EnemyPerceptionComponent->ConfigureSense(*AISenseConfig_Sight); //telling our perception component to use the sight config we just created
	EnemyPerceptionComponent->ConfigureSense(*AISenseConfig_Hearing);
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

void AGKAIController::BeginPlay()
{
	Super::BeginPlay();
	
	if (UCrowdFollowingComponent* CrowdComp = Cast<UCrowdFollowingComponent>(GetPathFollowingComponent())) //We cast from our path following component to our crowd following component to access detour crowd avoidance features
	{
		CrowdComp->SetCrowdSimulationState(bEnableDetourCrowdAvoidance? ECrowdSimulationState::Enabled : ECrowdSimulationState::Disabled); //Enabling or disabling detour crowd avoidance based on our variable

		switch (DetourCrowdAvoidanceQuality) // Setting the quality of our crowd avoidance depending on our variable
		{
		case 1: CrowdComp->SetCrowdAvoidanceQuality(ECrowdAvoidanceQuality::Low);    break;
		case 2: CrowdComp->SetCrowdAvoidanceQuality(ECrowdAvoidanceQuality::Medium); break;
		case 3: CrowdComp->SetCrowdAvoidanceQuality(ECrowdAvoidanceQuality::Good);   break;
		case 4: CrowdComp->SetCrowdAvoidanceQuality(ECrowdAvoidanceQuality::High);   break;
		default:
			break;
		}
		
		CrowdComp->SetAvoidanceGroup(1); //Setting our AI to be in avoidance group 1
		CrowdComp->SetGroupsToAvoid(1); //Setting our AI to avoid other agents in group 1
		CrowdComp->SetCrowdCollisionQueryRange(CollisionQueryRange); //Setting how far ahead our AI will look for potential collisions
	}
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
