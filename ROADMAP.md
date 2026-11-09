# Roadmap

What is planned and what is being looked into. Firmware changes are batched,
so boards are reflashed as rarely as possible.

## Firmware (one reflash for all of it)
- **Self-numbering**: a new board listens for a few seconds, takes the lowest
  free number, settles clashes by its factory address and keeps the number;
  `tw_setid.py` still overrides.
- **USB output only while a computer is reading**, with a hello when a reader
  appears and once a minute.
- **The channel** (designed 2026-11-09; tw_node 3.1 and 3.2 built):
  3.1, the channel survey: `tw_setid.py PORT survey`, every board at once
  listens on channels 1-11 and reports who else is on the air; the table
  ranks the 40 MHz pairs by their worst board. 3.2: set the channel
  (`channel N`, 1-11, US) as a network key, every board switching 20 s after
  it takes it, and a board that hears nobody for 30 s scanning for the
  others (a board that was off, or a new one). To weigh: 20 MHz instead of
  40 (the first survey: 40 MHz in 2.4 GHz always overlaps a neighbor; 20 MHz
  on 11 was 5.5% busy at worst, against 19% for 7 + 11), for fewer
  subcarriers. Next, 3.3: groups (a group
  number in header byte 19, a default in the firmware, set over the cable),
  so two arrays near each other keep to themselves, each on its own channel.
- **Survey mode (FTM)**: on request, the boards pause sensing and measure
  their distances to each other by round-trip timing (about 1-2 m indoors),
  so the layout file can be checked and a moved board noticed; a nightly
  survey compared with a stored one. Needs a test first: that switching a
  board's radio to answer and back leaves the readings as they were.
- **Room in the frames at 6 boards**: built in tw_node 2.1, a second frame in
  a board's own slot when records wait (docs/SECOND-FRAME.md: design and
  simulator, 6 boards 97.6% -> 99.9%, two stranded boards 83% / 53% -> 98.6%).
  Still to do: the desk test and the crowded-night test (`cap 2`) on real
  boards; then decide whether to keep it on by default.
- **The followers' clock lag**: partly fixed in tw_node 2.2 (each frame's own
  air time counts: up to 700 us). On the real boards (`lag_check.py`) the
  followers are still 1.1-1.5 ms behind: a frame is stamped when built, about
  1 ms before the air, and the receive callback the clock uses comes about
  0.5 ms after the air.
- **Protocol 3** (docs/PROTOCOL-3.md; tw_node 3.0 built): one transport for settings and
  commands (records with keys: board settings, standing network settings,
  one-time actions; for board N or all; confirmed by each board's reports),
  and a header that keeps only what each frame needs (5 bytes reserved for
  the unknown). In steps: 2.5.1 the LED's actions over the air, 2.6 network
  settings both ways, 2.7 reports both ways, 3.0 the header. Any new setting
  or command is a key on the transport.
- **Frame timing (tw_node 2.3)**: docs/TIMING-2.3.md. Simulator, fitted to
  the boards: no cost up to 6 boards; above 6, even in normal running, the
  last board's frames run into the timekeeper's slot (8 boards: 150
  collisions in 120 s). The fix (build ahead, the clock from the radio's
  receive time and the sender's delay, the second frame from the send-done)
  is simulated: followers within 15 us, 0 collisions at 8 boards. Next: check
  the receive timestamp's clock, then build.
- Irregular send timing, so fast rhythms (motors, fans) show at their true
  rates instead of folded down; keep the phase as well as the strength.

## Analysis
- A catalog of known habits (motors, parked cars, a board's own quirks),
  subtracted from every run, so only the new stands out.
- Minutes sorted into "kinds" without labels, to flag states never seen.
- Maps with more boards: with 4+ boards the lines cross, and a moment
  becomes a place.

## Ideas
- A listening mode: a board that measures any 2.4 GHz access point's beacons
  and traffic without joining it, using the access point as the light source.
  Anywhere but your own home, only with the owner's and the occupants'
  agreement.
- Outdoors: a small solar enclosure beside beehives (activity, fanning, the
  colony's daily rhythm), where one-hop range and weather will matter.
