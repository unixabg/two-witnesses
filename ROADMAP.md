# Roadmap

What is planned and what is being looked into. Firmware changes are batched,
so boards are reflashed as rarely as possible.

## Firmware (one reflash for all of it)
- **Self-numbering**: a new board listens for a few seconds, takes the lowest
  free number, settles clashes by its factory address and keeps the number;
  `tw_setid.py` still overrides.
- **USB output only while a computer is reading**, with a hello when a reader
  appears and once a minute.
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
  air time counts: up to 700 us). With the delays the boards measured, the
  simulator still puts followers about 850 us behind: a frame is stamped when
  built and reaches the air about 1 ms later.
- **Frame timing (tw_node 2.3?)**: docs/TIMING-2.3.md. Simulator: the 2.2
  delays cost nothing up to 6 boards; at 7-8 boards with many second frames,
  thousands of waits and some collisions. A fix (build ahead, a clock
  correction, the second frame sent from the send-done) is simulated, not
  built. First two cheap measurements on the boards: where followers' frames
  arrive in the timekeeper's clock, and how long the build takes.
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
