#include "game.hpp"

namespace pusu {
void Game::use_entities(Vec3 position) {
    // 00422d40(position, "_on_use") queries static PL brush groups containing
    // the player's frame-0 origin, not a camera ray or dynamic pickup sphere.
    use_triggers(position, "_on_use");
}
} // namespace pusu
