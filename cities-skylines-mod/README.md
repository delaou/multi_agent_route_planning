# Vehicle Telemetry Sender

Cities: Skylines 1 mod that captures service vehicle, railway line, and train
snapshots and uploads the latest snapshots to the configured server:

`POST /api/game/vehicles/snapshot`
`POST /api/game/lines/snapshot`
`POST /api/game/trains/snapshot`

It also polls dispatch commands from:

`POST /api/game/dispatch/commands/query`

and closes the execution loop with:

`POST /api/game/dispatch/commands/ack`
`POST /api/game/dispatch/resources/cleared`

The default server URL is `http://127.0.0.1:8080`. Set the
`CS_TELEMETRY_SERVER_URL` environment variable before starting Steam to use a
different host.

## Build

1. Install and enable `Harmony 2.2.2-0 (Mod Dependency)` for Cities: Skylines 1.
   The project deploys `CitiesHarmony.API.dll`, while the shared dependency Mod
   provides the patched Harmony runtime used by all CS1 Mods.
2. Install Visual Studio with .NET desktop development tools.
3. If Cities: Skylines is not in the configured Steam location, set the MSBuild
   property `CitiesSkylinesPath` to the game directory.
4. Build `VehicleTelemetryMod.csproj`.

The target is .NET Framework 3.5, using the game's Managed directory for framework
references. Do not retarget to .NET 4.x or remove FrameworkPathOverride: the game's
legacy Mono cannot load those framework types. Commands use a locked Queue;
responses use Unity 5.6 JsonUtility with serializable field-based DTOs, avoiding
ConcurrentQueue and System.Web.Extensions dependencies. To build without deploying,
pass `/p:SkipModDeployment=true`.

The post-build target copies `VehicleTelemetryMod.dll` to:

`%LOCALAPPDATA%\Colossal Order\Cities_Skylines\Addons\Mods\VehicleTelemetryMod`

## Runtime notes

- Start the C++ vehicle server before loading a city.
- Vehicle state is collected by scanning the game's vehicle buffer. Harmony is
  used only for the train speed gate, not for telemetry collection.
- Railway line geometry is collected from the game's rendered transport line
  curves when available, with stop positions as a fallback.
- Train state is collected from rail-like transport lines and mapped onto the
  latest uploaded route polyline to estimate `route_index`.
- The HTTP worker keeps only the newest snapshot per endpoint, so a slow server
  cannot build an unbounded backlog.
- The C++ server accepts snapshots at `/api/game/vehicles/snapshot` and exposes
  the current snapshot at `GET /api/game/vehicles`.
- The poller parses commands into a thread-safe queue. Game objects are touched
  only from `ThreadingExtension.OnUpdate`, never from an HTTP worker thread.
- A train approaching a shared-corridor entrance without a matching RELEASE is
  held within 300 metres. RELEASE stops applying the local hold and leaves
  movement to the vanilla TrainAI.
- Entry is ACKed only after the train is observed inside the route-index bounds.
  Resource clearance is posted after it leaves and a two-second tail allowance
  has elapsed.
- The HOLD implementation publishes an immutable set containing the whole train
  consist. A last-priority Harmony Postfix clamps TrainAI's calculated
  `maxSpeed` to zero for those vehicle IDs; RELEASE removes the extra limit and
  never forces the vanilla AI to move.
- `CitiesHarmony.Harmony.dll` in `lib/` is a compile-time reference only and is
  deliberately not copied into this Mod's output directory.
