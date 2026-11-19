# Multiplayer

This document covers networked multiplayer: a dedicated server binary that
streams a text screen to each of its thin clients over TCP, and the client that
connects to it. This is a technology preview -- see
[Limitations](#limitations-of-this-first-version) before you plan a session.

## Overview

The server is a special build of the game (`MP_SERVER=1`). It links an
in-memory curses backend and renders everything -- menus, the map, IMGUI
screens -- into a text grid per player. Each client connects over TCP, sends its
terminal size, and the server streams that player's grid to it. The client is the normal
game binary in a special mode: it forwards your keystrokes to the server and
paints whatever screen the server sends. The server runs the game logic; the
client is a thin remote viewport.

Because the server is a full game build, everything you can do in single-player
(move, fight, craft, read, sleep) is available, with every player in their own
menus at the same time -- but only the server's own keybindings are in effect.

The server also owns the world and the party. It waits for its players, opens
the world named on its command line or in its server config file, and gives
each of them their own join menu, in which the player creates a survivor or
continues the one they played last time. Nothing has to be prepared in the
world beforehand.

## Building

### Server

```sh
make -j$(nproc) CCACHE=1 MP_SERVER=1 BUILD_PREFIX=mpserver-
```

This produces `mpserver-cataclysm-server`. `MP_SERVER` implies `MULTIPLAYER` and
sets a server-specific target name (Makefile, `TARGET_NAME = cataclysm-server`),
so the build never collides with a normal client build. The server links the
in-memory curses backend and IMGUI's text backend; it does not link SDL or
ncurses.

To also build the server test binary (the `[mp]` tests, used by CI), add
`TESTS=1`:

```sh
make -j$(nproc) CCACHE=1 TESTS=1 MP_SERVER=1 BUILD_PREFIX=mpserver-
# -> tests/mpserver-cata_test
```

### Client

The client is the normal game built with `MULTIPLAYER=1` (the curses build is
sufficient):

```sh
make -j$(nproc) CCACHE=1 MULTIPLAYER=1
# -> cataclysm
```

`MULTIPLAYER` is **not** on by default, and it is what compiles `--connect` in.
A plain `make` produces a binary that still accepts `--connect` on the command
line but refuses to run with it, because the client code is not in the build.
If you already have a plain build sitting at `./cataclysm`, this rebuilds it in
place -- quit any running copy first, or the link fails with "Text file busy".

## Running a session

### 1. Start the server

```sh
./mpserver-cataclysm-server --server [<port>] --world <name> --players <count> --userdir <path>
```

- `--server <port>` -- run this instance as a multiplayer server that streams a
  screen to each connected client (main.cpp, `--server` handler). The port may
  be left out (`--server` alone); it then has to come from the server config
  file.
- `--world <name>` -- the world to open. It may be left out if the server
  config file names `world`; if neither does, the server refuses to start and
  says `refusing to start: no world: give --world <name> or set "world" in the
  server config`. A name that is not in the save folder yet is created with the
  default mods, so `--world coop` on a machine with no such world gives you a
  fresh empty one.
- `--players <count>` -- how many seats the server creates (default 1). It
  waits at the listener for that many clients, opens the world and waits for
  every one of them to choose a survivor (see
  [Joining a session](#joining-a-session)). The number of seats is fixed: a
  seat can be vacated and taken again later (see [When a client leaves or
  dies](#when-a-client-leaves-or-dies)), but the roster never grows beyond
  this count.
- `--userdir <path>` -- keep the server's config, saves, and log under this
  directory instead of your normal single-player profile. Recommended, so the
  server does not share options or savegames with your client.
- `--hurry-ms <ms>` -- how long, in milliseconds, a countdown started by hand
  lasts before the players who are still reading stop holding the world
  (default 15000). The accepted range is `[1000, 600000]`; anything else is
  refused and logged, and the session runs on the defaults -- a bad value is
  never quietly clamped.
- `--hurry-combat-ms <ms>` -- the shorter countdown that paces a turn nobody is
  playing by hand while a hostile can see a player (default 4000). Same range
  `[1000, 600000]` and the same refusal rule.

#### The server config file

Every one of those options except `--server` and `--userdir` can also be put in
a JSON file at `<userdir>/config/mp/server.json` (the profile's
`config/mp/server.json` when no `--userdir` is given):

```json
{
  "port": 4321,
  "world": "coop",
  "players": 2,
  "hurry-ms": 15000,
  "hurry-combat-ms": 4000
}
```

- The five keys are `port` and `players` (integers), `world` (a string), and
  `hurry-ms` and `hurry-combat-ms` (integers, the same two countdowns as the
  flags).
- The command line wins over the file for each key it names; a key neither
  names falls back to the shipped default. A missing file is not an error.
- `port` and `world` have no built-in default: one of the two sources must
  name each, or the server refuses to start and says which one is missing.
- A malformed file, an unknown key or a value of the wrong type is refused, not
  ignored: the server prints `refusing to start: cannot read server config
  "<path>": <reason>` and exits with status 1. A typo in an option name is
  therefore visible to the operator instead of silently doing nothing.
- An out-of-range `hurry-*` value does not stop the server: both countdowns run
  on the defaults for the session, with `hurry lengths refused: manual <n> ms,
  combat <m> ms; running on the defaults` on the console -- the same
  all-or-nothing rule the flags follow.

With the file in place, the server can be started with nothing but the mode
flag:

```sh
./mpserver-cataclysm-server --server --userdir mp-coop
```

The server binds a TCP listener on `<port>`, waits for `<count>` clients, and
performs a connection handshake with each (protocol version, terminal size,
256-colour support, the client's name). Only after every client is accepted does
the world open and the join phase begin; you can take as long as you like to
connect. The listener is not closed once play starts: that is how a lost client
reconnects and how a newcomer can take a freed seat.

The server has no screen of its own -- its curses backend draws into a buffer
that goes out over the wire -- so the console it was launched from carries a
session log on stderr instead:

```
[21:44:03] mp: listening on port 4321 (world: "Hanston", protocol 2)
[21:44:03] mp: waiting for 2 clients
[21:44:31] mp: connection from 127.0.0.1:55226; shaking hands
[21:44:31] mp: client "Bo" connected from 127.0.0.1:55226 (terminal 180x50, 256 colours, protocol 2)
[21:44:31] mp: 1 of 2 connected
[21:44:47] mp: connection from 127.0.0.1:55230; shaking hands
[21:44:47] mp: client "Ada" connected from 127.0.0.1:55230 (terminal 80x24, 256 colours, protocol 2)
[21:44:47] mp: 2 of 2 connected
[21:44:47] mp: world "Hanston" is open; waiting for 2 players to choose a survivor
[21:45:12] mp: loading "Norman Blair"
[21:45:12] mp: "Bo" plays "Norman Blair"
[21:45:19] mp: "Ada" plays "Ada King" (new)
[21:45:19] mp: game started in world "Hanston"
[21:52:10] mp: "Norman Blair" disconnected after 7m39s; the session continues
[21:53:02] mp: "Norman Blair" reconnected
[22:04:11] mp: "Ada King" died; the seat is choosing a replacement
[22:05:40] mp: "Ada King II" replaced "Ada King"
[22:21:07] mp: "Norman Blair" disconnected after 35m55s; no players left; ending the session
[22:21:07] mp: saving "Norman Blair"
[22:21:07] mp: saving "Ada King II"
[22:21:09] mp: saved
[22:21:09] mp: game ended
[22:21:09] mp: server stopped
```

The disconnect, reconnect and death lines name the character, not the
`--username` label; the handshake and newcomer lines name the label the client
announced. The in-game message log adds the seat number, as in
`Player 1 (Norman Blair) disconnected` and `Player 1 (Norman Blair) reconnected`.

Every failure that stops the server says so on the same stream: a port already
in use, a failed accept, a refused server config, a rejected handshake with the
reason the client was turned away, and a named world whose creation failed.

### 2. Connect the client

```sh
./cataclysm --connect <host:port> [--username <name>]
```

- `--connect <host:port>` -- connect to a multiplayer server and play as its
  client (main.cpp, `--connect` handler).
- `--username <name>` -- optional, and it does not choose your character. It is
  the label this client announces in its handshake: the server puts it in its
  console log and on the other players' waiting screen while they wait for you
  to choose. Without it the client announces your system user name (`$USER`),
  or `client` if the environment does not provide one. Which survivor you play
  is decided inside the join menu, not on the command line.

The client needs a terminal between 80x24 and 1000x1000. On connect it
announces its terminal size; the server streams at that size and refuses a
client outside that range at the handshake. Resizing the client's terminal
renegotiates the grid.

The client also opens whichever join menu the server puts in front of it, and
remembers the survivor the server tells it it is playing (see [The character
card](#the-character-card)). Nothing else about the session lives on the client.

### When a client leaves or dies

A client that drops after the party has started no longer ends the session. The
server marks its seat `disconnected`: the shared message log shows `Player 1
(Norman Blair) disconnected` and its console shows `"Norman Blair" disconnected
after 7m39s; the session continues`. The character stays in the world as an
ordinary living body without a player -- monsters can still kill it and the
tether still counts it -- but the barrier stops waiting for that seat and its
moves are forfeited each turn. Everyone still at a terminal keeps playing.
Quitting the game mid-session (Save and quit, or Quit without saving) is
treated exactly like a lost wire: no death mark, no corpse, no replacement
menu. Abandoning the character is a death and takes the replacement path below.

A returning client that presents the same character card (same `--userdir`,
same `host:port`; see [The character card](#the-character-card)) gets the same
seat and the same living character back. The server clears the disconnected
mark, discards keys the old terminal had queued, and prints `Player 1 (Norman
Blair) reconnected` in the message log and `"Norman Blair" reconnected` on its
console. If the character died while its client was away, the seat is still
returned, but it is dead and goes straight to the replacement menu below. A
client without a matching card cannot take a living character's seat: with no
free seat it is refused with `no free seat`, and a live disconnected seat stays
reserved for its own card.

When a character dies, the session goes on too. The corpse stays where the
character fell, the message log says `Player 2 (Ada King) died`, and that
player's own screen opens the character-creation menu (the same menu as at
connect, without Continue). The replacement is placed on a free tile next to a
player who is still at a terminal -- or near where the character fell if nobody
else is available -- and takes over the same seat. The console records both
halves: `"Ada King" died; the seat is choosing a replacement`, then `"Ada King
II" replaced "Ada King"`. Leaving that menu without creating a character marks
the seat `disconnected`. Whether the seat is free for a newcomer then depends
on the character it still holds: an hp-dead character leaves a `dead &&
disconnected` seat that a newcomer may take, while a character abandoned with
"Abandon this character" is still a living body (no hit points were lost), so
the seat stays reserved for its card and a newcomer is refused with `no free
seat`.

The listener stays open after the party starts. A new client can fill only a
seat that is both dead and disconnected -- the previous occupant died and its
client left without choosing a replacement. That client creates a character in
the usual join menu, announced in the message log as `Player 2 (Cid) joined`
with the label the client gave at the handshake, and the replacement is placed
next to the party. Every other case is refused with `no free seat` (console:
`refused <peer>: no free seat`): all seats are occupied, or the vacated seat
still holds a living character waiting for its own card. No seats are ever
added beyond `--players`.

The session ends only when no client is connected at all. The last disconnect
prints `"Norman Blair" disconnected after 35m55s; no players left; ending the
session`, the server saves the world and exits; the session-end reason is `no
players connected`. While at least one client is connected the session stays
up even if every character is dead, because replacements can still be made.
The join phase keeps its old rule: a client that quits or drops there costs
only its own seat, and the server listens for a replacement (see [Joining a
session](#joining-a-session)).

## Two or more players

### Preparing a world

A co-op world is an ordinary world with more than one character in it. The
server opens the world, and the characters are made in the join menu, one save
file each -- so there is nothing to prepare beyond picking the world and its
mods:

1. Decide on a world name. If you want specific mods or a particular
   world-generation preset, create the world first in the normal game
   (World -> Create World) and quit without making a character.
2. Start the server on it. A name that does not exist yet is created with the
   default mods, which is fine for a first session.

```sh
./mpserver-cataclysm-server --server 4321 --world "coop" --players 2 --userdir mp-coop
```

The world need not contain any characters when the server opens it. If it has
been played before, its map, clock and NPCs are there and the party starts from
them; if it is new, the first party's characters are placed next to each other
in a starting location of their own.

### Joining a session

Each connected client gets its own join menu in its own window, streamed like
any other screen. It offers:

- **Continue as <name>** -- present only when this client's card names a
  character that is still in this world and nobody else in this session has
  taken it. Loads that character where it was left.
- **New character: custom / preset / random** -- the ordinary character
  creator. `preset` picks from the server's character templates. A new
  character's name must be free: not a save in this world and not a name
  another player in this session has just confirmed -- the game asks for
  another name otherwise.
- **Settings** -- the server's options. Anyone in the party may change them;
  co-op is a trusted circle.
- **Help**, **Quit** -- the usual. Quit during the join phase: your client
  exits, your seat is offered to the next connection, and everyone else stays
  where they were. After the party has started, a seat owed a replacement shows
  the same menu; quitting that leaves the seat `disconnected`, with the
  character still in the world. If the character was hp-dead, the seat is
  `dead && disconnected` and a newcomer may take it; if it was abandoned
  ("Abandon this character" leaves a living body), the seat stays reserved for
  the old character's card, and a newcomer is refused with `no free seat`.

A new character is placed on a free tile next to the rest of the party: the
bubble is shared, and the tether has to hold from the first turn. All co-op
characters also start under one scenario, **Co-op Survivors**
(`data/json/scenarios_coop.json`, id `coop_survivors`); on a server the
creator's scenario list is filtered down to it, so the party gets one safe,
roomy place to wake up in.

Whoever chooses first sees a waiting screen that lists the players who have not
chosen yet, by the name their client announced (`--username`, or your system
user name if you gave none -- the menu does not know their survivor yet). When
the last one confirms, the party starts.

### The character card

The server tells each client which survivor it plays, and the client writes a
small card under its own user directory:

```
<userdir>/config/mp/<base64 of host:port>.mpcard.json
```

For example `config/mp/#MTI3LjAuMC4xOjQzMjE=.mpcard.json` after connecting to
`127.0.0.1:4321`. It records the server address, the world name, the character's
`save_id` and its name -- enough to say "this is me" next time, and nothing
more. It is not a save: a character without its world is unplayable, so the card
cannot be played and cannot restore anything. The server stays the only source
of truth; a card naming a character that no longer exists simply means the
Continue entry is absent, and a client that has never been here sees the same
menu minus that entry.

There is one card per server address, in that client's user directory: connect
with the same `host:port` to be offered the same survivor. A replacement
character gets a fresh card when it is created, so the seat's claim follows the
character now being played. Two clients sharing one user directory and one
address would share the card, so give each its own `--userdir` if you run more
than one client on one machine.

### Starting the next session

Stop the server, start it again on the same `--world`, and have each client
connect with the same `--connect` address. Everyone who has played there before
finds "Continue as <name>" at the top of their menu and picks up where they
left off; anyone new creates a character instead. The world's map, clock and
NPCs are shared -- each character's own save file holds that character.

### How turns work

Everyone acts at once: each player has their own screen, their own menus and
their own input, and opening a menu never blocks anyone else's screen. The world
-- monsters, fire, the clock -- only moves on once every player has acted. A
player who still has time to spend this turn **holds** the world; a player who
has spent it, or who is running a long activity (crafting, reading, a long walk
to a destination), has **acted** and holds nothing.

Leafing through a menu costs no time. So a player reading the crafting list
holds the world until they pick something -- and their partner, who has already
acted, waits.

### Hurrying a partner

Press **`` ` ``** (backquote, the `coop_hurry` action) to start a countdown for
every player who is still holding the world -- you too, if you press it while
you still have time to spend. When it runs out, those players stop holding the
world for as long as they stay at the screen they are on: the world moves on,
and their characters stand still and lose those turns. Nothing is closed -- the
menu stays open, their keys still work, and they finish what they were doing
when they are ready. Once they act, the world waits for them again like anyone
else. If what they chose has gone stale meanwhile (somebody took the item), the
game refuses it with its usual message.

When a hostile creature can see any player, the countdown starts by itself and
is shorter (4 seconds rather than 15).

If nobody is left playing -- everyone has been dropped this way or is busy with
a long activity -- the world does not race ahead: it moves one turn per
countdown length until somebody acts.

### Stepping away

Press **`:`** (the `coop_away` action, "Go without me") when you are leaving
the keyboard on purpose. Your partner no longer waits for you: the barrier
treats you like someone who has already acted, and your moves are forfeited
each turn instead of piling up, so you cannot come back with a minute of banked
time and take sixty actions in one go. The sidebar shows `away: <name>` next to
the `waiting:` list, so everyone can see who is not at their screen. With no
party formed -- a single server slot -- the key does nothing: there is nobody
to step away from.

You come back by taking an action that costs a turn, or by pressing `:` again.
Free actions do not return you -- opening your inventory, the map, the look
cursor, the message log and the rest of the free actions leave you away; if
they did, a partner reading a menu would freeze the world, which is the whole
reason this mode exists.

When every living player is away or disconnected, the turn stays open and the
world stands still: the clock does not move around empty chairs or a dropped
player. The key is the exit -- somebody returns, and the world moves on again.

Away is not a fight fast-forward. While a hostile creature can see a player and
nobody is playing by hand, the turn never closes faster than the combat
countdown (4 seconds by default), so a partner crafting across the room cannot
let a monster eat the character who stepped out.

The server never plays for an away player, never closes their menu and never
brings them back on its own; that is the player's decision alone.

## Limitations of this first version

- **A fixed roster, and no spectators.** The server waits for exactly
  `--players` clients before it opens the world, and that many seats exist for
  the session. A client that leaves the join menu frees its seat for a new
  connection; after the party has started, a dropped or quitting client can
  reclaim its seat with its character card, and a seat whose dead character's
  client is gone can be taken by a newcomer. The roster never grows, nobody can
  join as a spectator, and a client without the right card cannot take a
  living character's seat.
- **Some world state is still shared.** The game itself warns that many features
  "will not work correctly with multiple characters in the same world", and
  co-op now lives on exactly that path. Missions are per character for
  completion, failure and rewards, but kill-count and recruit-class goals and
  nemesis missions still count on the world map; vehicle seats are per
  character. Base camps and zones, NPC followers, faction reputation, and
  achievements are not, and are deferred, with estimates in
  `docs/superpowers/reports/2026-08-08-multi-character-world-audit.md`.
- **Hurry is pressed by the wrong player.** A player who has already acted is
  parked until the next turn and reads no keys, so they cannot press hurry; only
  a player who is still holding the world can. In practice the waiting player
  asks their partner to press it, or waits; danger starts the countdown on its
  own. A player who knows they will be a while can step away with `:` before
  they hold the party up (see [Stepping away](#stepping-away)), but the game
  cannot press that for them either.
- **The world cannot ask anyone anything.** An NPC who walks up to talk, a
  monster that asks a question, a popup from an effect: these come from the
  world's half of the turn, which has no player to answer, so they get the
  answer of Escape -- the conversation does not open, the question is answered
  "no" -- and the server's console says one was skipped.
- **A death means a new character.** There are no death screens and no
  deathcam in a session: the dead character's corpse stays in the world, and
  its player creates a replacement next to the party. The old character is not
  resumed; the world keeps its corpse.
- **One terminal size for everyone.** All players draw into one shared root
  window, which is sound only while every client runs the same terminal size.
  Use the same size on every machine.
- **The server's keyboard layout is the one that counts.** The client sends
  raw input events (key presses, mouse position, modifiers); the server feeds
  them through its own keybinding configuration. If you want a different layout,
  change the server's keybindings. The client's keymap is not consulted.
- **No sound.** The server renders to a text grid and streams that grid; audio is
  not transmitted and the client does not synthesise it.
- **No tileset map.** The streamed picture is the server's text grid. Tile
  graphics are never sent; the client renders text cells and colours.
- **Text only.** The picture is a grid of characters with per-cell colour
  indices. Mouse events are forwarded, but there is no pixel-perfect or
  hardware-accelerated rendering.

## Debugging

The wire traffic is useful for debugging a session.

- **Control messages are JSON.** The hello, resize, input, cursor, and bye
  messages are serialised as readable JSON (`mp_control_to_json`, mp_protocol).
  Screen frames are the compact binary of `mp_screen_codec`, because that is
  the hot path.
- **Peer JSON is parsed leniently.** Incoming control messages call
  `obj.allow_omitted_members()` before reading fields (mp_protocol.cpp), so a
  peer that adds a field does not trip the "unvisited member" debug message.
  The whole parse is wrapped in a `try`/`catch( const JsonError & )`, so a
  malformed or unknown message is dropped rather than raised. The `proto` field
  in the client's hello exists so a later protocol version can add fields and
  an older peer can skip what it does not recognise.
- **Frames.** Every payload is framed with one kind byte (`mp_msg_kind`: 1 for
  control, 2 for screen) followed by the body (`mp_wrap`/`mp_unwrap`). The TCP
  transport sets `TCP_NODELAY` so key presses are not batched behind screen
  traffic.
- **The console.** The server's session log (above) is the first place to look;
  it is written to stderr, so `./mpserver-cataclysm-server ... 2>server.log`
  keeps it, and a service manager captures it with no extra configuration.
- **The debug log.** The game writes `debug.log` under the user directory's
  config dir (`<userdir>/config/debug.log`). Every console line is mirrored
  there through the `D_MAIN` class, but at `D_INFO` level, which the default log
  filter drops -- so an unconfigured run leaves no `debug.log` at all and the
  console is what you have. Errors (`mp client: cannot connect ...`, the
  oversized-terminal refusal) are logged at `D_ERROR`/`D_WARNING` and do reach
  the file. Run both sides with the same `--userdir` or with none at all; the
  `[mp]` server tests use a fresh `--userdir` so a session never writes into a
  profile you care about.
