Copy these files in ~/.mixxx/controllers and load the mapping in Mixxx

The mapping drives four decks. The add-on's own USB MIDI sends MIDI channel 1: the left side is deck 1, the right
side deck 2. Decks 3 (left) and 4 (right) use the same Control Change numbers on MIDI channel 2: a Fri3d badge
running the Zephyr `dj_app` switches each side of the add-on between its two decks on its touchscreen, and shows the
LEDs of the selected deck. The crossfader always uses channel 1.

The potmeters and faders use soft takeover: after switching decks, a control only takes effect once it passes the
deck's current value.
