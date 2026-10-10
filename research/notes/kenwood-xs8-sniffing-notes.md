# Kenwood XS8 Sniffing Notes

**Hardware:** Kenwood KR-V6070 receiver + connected Kenwood CD player\
**Date:** 2026-10-07\
**Status:** Passive sniffing phase complete; ready to design/test a safe
transmitter.

## Physical interface

Observed SYSTEM CONTROL / XS connection:

-   **Sleeve:** GND
-   **Tip:** approximately `S.BUSY`
-   **Ring:** approximately `S.DATA`

These assignments are based on the working passive-sniffing setup and
should be rechecked before actively driving the bus.

## XS8 wire decoding

The captured XS8 byte can be read directly from the waveform:

-   framing/start precedes the eight data bits;
-   data is transmitted **MSB first**;
-   **short data interval = 1**;
-   **long data interval = 0**;
-   do **not** reverse the bit order;
-   do **not** invert the resulting byte.

Earlier reverse/invert interpretations were artifacts of including
framing in the data-bit analysis and should not be used.

## Receiver front-panel input selection

A clean control run was made with the CD stopped and powered off to
avoid automatic playback activity.

  Front-panel action     Observed XS8 byte
  -------------------- -------------------
  TV2                               `0x8E`
  TV1                               `0x85`
  TAPE2 state A                     `0xEB`
  TAPE2 state B                     `0xE7`
  TAPE1                             `0x86`
  TUNER                             `0x84`
  PHONO                             `0x8C`
  CD                                `0x82`

### TAPE2 validation

Four consecutive presses produced:

`0xEB → 0xE7 → 0xEB → 0xE7`

This is a useful internal validation of both the capture and decoder and
is consistent with the TAPE2 monitor toggle behavior.

## CD transport / system-control observations

### Playback start

Five independent transitions into playback produced the same two-byte
sequence:

`0xDE → 0x7D`

This occurred for:

-   PLAY from STOP;
-   PLAY from PAUSE;
-   NEXT from STOP when NEXT caused playback to begin.

When the receiver was on another input (for example TV1), starting
playback on the CD caused the receiver to switch automatically to CD.

Therefore the observed sequence is associated with **CD playback
activation / system coordination**, not with a NEXT command.

### Pause

Observed PAUSE byte:

`0xD2`

### Stop

Observed STOP byte:

`0xD6`

STOP was independently captured four times with the same result.

### NEXT / PREV

While CD playback was already active:

-   NEXT produced **no XS traffic**;
-   PREV produced **no XS traffic**.

PREV from STOP also produced no observed XS traffic.

NEXT from STOP can start playback; when that happens the bus carries the
normal playback-start sequence:

`0xDE → 0x7D`

Thus NEXT/PREV appear to be local CD-player operations when they do not
require a system-level state transition.

## Observed behavior model

``` text
CD stopped
    |
    +-- PLAY ----------> DE -> 7D ------> receiver selects CD
    |
    +-- NEXT ----------> DE -> 7D ------> receiver selects CD
                         (because playback starts)

CD playing
    |
    +-- NEXT ----------> no XS traffic
    +-- PREV ----------> no XS traffic
    +-- PAUSE ---------> D2
    +-- STOP ----------> D6

CD paused
    |
    +-- PLAY ----------> DE -> 7D ------> playback resumes / CD coordinated
```

## Bidirectional system coordination

Experiments demonstrated behavior in both directions:

1.  **CD → receiver:** Starting playback on the CD while the receiver
    was on TV1 caused the receiver to switch automatically to CD.
2.  **Receiver → CD:** Selecting CD on the receiver while the CD player
    was powered and available could cause CD playback to start.
3.  With the CD powered off, selecting CD on the receiver produced the
    clean receiver front-panel input event (`0x82`) without the
    playback-related activity.

This strongly suggests that XS is a system-coordination bus rather than
a simple mirror of front-panel transport button presses.

## Important distinction: observed bytes vs library mappings

These values are **observed wire-level results from this actual
KR-V6070/CD setup**.

Some existing KenwoodXS library constants use different values for
similarly named operations (notably PLAY/PAUSE or STOP). Do not replace
the observed values with library mappings without validating the
behavior on this hardware.

In particular, keep separate:

-   receiver front-panel **CD input selection:** `0x82`
-   playback-start/system sequence: `0xDE → 0x7D`

Their different values are not assumed to be an error; they were
observed in different system-control contexts.

## Current verified command map

  Function / event                                XS8 byte(s)
  ---------------------------------- ------------------------
  Receiver TV2 input                                   `0x8E`
  Receiver TV1 input                                   `0x85`
  Receiver TAPE2 toggle A                              `0xEB`
  Receiver TAPE2 toggle B                              `0xE7`
  Receiver TAPE1 input                                 `0x86`
  Receiver TUNER input                                 `0x84`
  Receiver PHONO input                                 `0x8C`
  Receiver CD input                                    `0x82`
  CD playback start / coordination              `0xDE → 0x7D`
  CD PAUSE                                             `0xD2`
  CD STOP                                              `0xD6`
  CD NEXT while playing                no observed XS traffic
  CD PREV while playing                no observed XS traffic

## Next phase

Passive sniffing is considered complete.

Before transmitting:

1.  determine the electrical behavior of `S.DATA` / `S.BUSY`
    sufficiently to avoid driving against another device;
2.  design a safe interface (rather than directly using a push-pull MCU
    output if the bus requires open-collector/open-drain behavior);
3.  reproduce the observed timing/framing;
4.  begin with a low-risk, well-understood command;
5.  verify receiver/CD response and bus contention before expanding the
    command set.

The captured physical behavior documented above is the reference
baseline for transmitter development.
