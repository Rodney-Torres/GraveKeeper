//  Rodney Torres, Erik Aguiar, and Michael Hernandez All Rights


#include "Controllers/GKPlayerController.h"

AGKPlayerController::AGKPlayerController()
{
	PlayerTeamId = FGenericTeamId(0); // Team ID for the player
}

FGenericTeamId AGKPlayerController::GetGenericTeamId() const
{
	return PlayerTeamId;
}
