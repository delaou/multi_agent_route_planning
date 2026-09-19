#include "DispatchManager.h"

#include <iostream>

int main() {
    DispatchManager manager;
    SharedCorridorState forward;
    forward.corridor_id = "C";
    forward.corridor_resource_id = "C:FORWARD";
    forward.direction = CorridorDirection::FORWARD;
    forward.received_at_wall_ms = 1000;
    SharedCorridorState reverse = forward;
    reverse.corridor_resource_id = "C:REVERSE";
    reverse.direction = CorridorDirection::REVERSE;
    manager.upsertCorridor(forward);
    manager.upsertCorridor(reverse);
    manager.upsertLineState("C:FORWARD", "A", LineDispatchState{});
    manager.upsertLineState("C:REVERSE", "B", LineDispatchState{});

    DispatchCandidate a;
    a.train_id = "A1"; a.line_id = "A";
    a.corridor_resource_id = "C:FORWARD";
    a.corridor_direction = CorridorDirection::FORWARD;
    a.arrived_at_simulation_ms = 1; a.received_at_wall_ms = 1000;
    manager.replaceCandidates("C:FORWARD", {a}, 1000);
    auto release = manager.planNext("C:FORWARD", 10000, 1000);
    if (release.action != DispatchAction::RELEASE ||
        !manager.confirmEntry("C:FORWARD", "A1", 10000)) return 1;

    DispatchCandidate b;
    b.train_id = "B1"; b.line_id = "B";
    b.corridor_resource_id = "C:REVERSE";
    b.corridor_direction = CorridorDirection::REVERSE;
    b.arrived_at_simulation_ms = 1; b.received_at_wall_ms = 1000;
    manager.replaceCandidates("C:REVERSE", {b}, 1000);
    auto blocked = manager.planNext("C:REVERSE", 20000, 1000);
    if (blocked.action != DispatchAction::HOLD_ALL ||
        blocked.reason != "opposing_direction_occupied") return 2;

    if (!manager.markCorridorExited("C:FORWARD")) return 3;
    auto next = manager.planNext("C:REVERSE", 20000, 1000);
    if (next.action != DispatchAction::RELEASE) return 4;
    std::cout << "DISPATCH_DIRECTION_INTERLOCK_OK\n";
    return 0;
}
