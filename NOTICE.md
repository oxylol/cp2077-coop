# Notice: what comes from CyberpunkMP

Cp2077Coop is a modification of **CyberpunkMP** by **Tilted Phoques SRL**
(<https://github.com/tiltedphoques/CyberpunkMP>, license: [LICENSE.md](LICENSE.md)). Since version 0.6 it is
developed as an enhancement of CyberpunkMP: where CyberpunkMP already solved a problem, its solution is used and
built on, and the result stays under CyberpunkMP's license terms.

Taken from CyberpunkMP (commit `0ccb0cf`, December 2024, for game patch 2.2) so far:

| Here | From CyberpunkMP | What |
|---|---|---|
| `src/plugin/Looks.cpp`, `src/plugin/Looks.hpp` | `code/client/App/World/AppearanceSystem.cpp`, `code/client/App/Network/NetworkService.cpp` (`SendSpawnRequest`), `code/client/Game/Utils.h` (`CMPWriter`/`CMPReader`), `code/client/Game/CharacterCustomizationSystem.h`, and the `ICharacterCustomizationState` declaration from Tilted Phoques' RED4ext.SDK fork | A player's look on another player's game: the character customization state serialized from the local V and applied to a spawned body through the world's entity appearance changer (head, face, hair, beard, body, arms groups), the body's third-person flag in its puppet state, and the player's items put on it |
| `tweaks/Cp2077Coop/bodies.tweak` | `code/assets/Tweaks/CyberpunkMP.tweak` (`Character.Muppet`) | Spawning the player's third-person body through a Character record based on `Character.TPP_Player`, with the template picked by `genders` |
| `src/core/LookItems.hpp` (idea) | `AppearanceSystem::AddItems` | The item list that travels with a player's look (here as TweakDBIDs with their slots) |

Changed on the way (details at the top of `src/plugin/Looks.hpp`): the body is the player's own third-person template
with V's animation graph instead of CyberpunkMP's NPC body and walk/sprint controller (this project's animation
capture drives it); items are read from the player's attachment slots; the transaction system is called through
its script natives; engine addresses are looked up without ending the game when one is missing.

Not taken (yet): CyberpunkMP's dedicated server, .NET plugin SDK, RPC system, vehicle sync, chat, emotes, jobs, and
its movement controller (`MultiMovementController`, which only knows idle, walk and sprint). This project keeps its
own host-based story co-op architecture ([docs/01-architecture.md](docs/01-architecture.md)).
