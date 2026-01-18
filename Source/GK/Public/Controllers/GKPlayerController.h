//  Rodney Torres, Erik Aguiar, and Michael Hernandez All Rights

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerController.h"
#include "GenericTeamAgentInterface.h"
#include "GKPlayerController.generated.h"

/**
 * 
 */
UCLASS()
class GK_API AGKPlayerController : public APlayerController, public IGenericTeamAgentInterface
{
	GENERATED_BODY()
	
	public:
	AGKPlayerController();
	
	virtual FGenericTeamId GetGenericTeamId() const;
	
private:
	FGenericTeamId PlayerTeamId;
};
