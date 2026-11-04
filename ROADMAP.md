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
- **Room in the frames at 6 boards** (simulator: docs/REFERENCE.md, relaying):
  a frame holds 6 full records and brings 5 new ones of its own, so second
  copies mostly stop fitting (97.6% delivered instead of 99.9%) and relaying
  can carry one stranded board but not two at once (83% and 53%). Proposal to
  design and simulate first: a board sends a second frame in its own slot
  when records are waiting (relayed ones, second copies); a slot is 3 ms and a
  full frame takes under 1 ms, so the air has room (airtime would rise from
  26% to at most about 50%). Alternatives: compact records sooner (loses the
  HT-LTF part the views use), fewer bits per amplitude (coarser notches).
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
