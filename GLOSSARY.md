# Cataclysm-DDA multiplayer

Co-op multiplayer terminology: a server-arbitrated party whose players share one
reality bubble and take turns in lockstep.

## Language

**Seat**:
A roster position opened when the server starts, one per `--players`.
_Avoid_: slot, player index

**Away**:
A player-declared state: the barrier stops waiting for them, their moves are
forfeited, and their avatar stays under their own control. Ends when they take a
turn-costing action or press the away key again.
_Avoid_: AFK, idle, disconnected

**Disconnected**:
A seat whose client has lost transport or left voluntarily. The avatar stays in
the world as a parked body with no AI, and the session continues.
_Avoid_: detached, offline, dropped

**Dead**:
A character whose body is a corpse. Their player stays connected and may create
a replacement.
_Avoid_: killed player, lost seat

**Replacement**:
A new character created by a connected player after their previous character
died; it appears next to the party, and the corpse stays where it fell.
_Avoid_: respawn, clone

**Session**:
One server-run party, from the moment the party starts until no clients remain
connected.
_Avoid_: game, match, lobby
