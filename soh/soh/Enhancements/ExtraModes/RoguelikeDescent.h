#pragma once

// Roguelike Descent: a room-by-room roguelike mode.
// Link is dropped into a random room of a random dungeon, the doors are sealed, and the room must be
// cleared of enemies to descend to the next floor. Floors get harder, rewards are handed out along the way,
// and dying ends the run. The player's real save data is snapshotted before the run and restored after it.

#ifdef __cplusplus
extern "C" {
#endif

bool Roguelike_IsRunActive();

#ifdef __cplusplus
}

void Roguelike_RequestStartRun();
void Roguelike_RequestAbandonRun();
int Roguelike_GetCurrentFloor();
int Roguelike_GetBestFloor();
int Roguelike_GetLevel();
int Roguelike_GetXp();
int Roguelike_GetXpForNextLevel();
#endif
