# Mid-session joins fill only dead, disconnected seats

The roster size stays fixed by `--players`. A client arriving after the party
starts may take a seat only when that seat has no connected player and no
living avatar — that is, its character is dead and its player disconnected. A
dead but connected player keeps their seat for a replacement; a disconnected
but living avatar keeps its seat for reconnection. This preserves the
scheduler's bounded roster and avoids hot-adding seats mid-session.
