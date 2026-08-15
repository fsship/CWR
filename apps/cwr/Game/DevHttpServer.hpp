#pragma once

namespace Poseidon::Dev
{

// A deliberately lightweight, unauthenticated LAN development endpoint.  Its
// socket thread never reads game state: PumpHttpServer is called by the game
// loop and performs all world/UI/SQF work on the game thread.
void StartHttpServer();
void PumpHttpServer();
void StopHttpServer();

} // namespace Poseidon::Dev
