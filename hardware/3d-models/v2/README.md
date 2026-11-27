# Satoshi Pet case v2

A parametric rebuild of the v1 Tinkercad case (`../case-v1.stl`), keeping its layout
(screen window, keyring corner, front button, Bitcoin logo, paw on the back) and fixing
what didn't work in v1.

![classic](previews/classic-sheet.png)

## What changed from v1

| | v1 | v2 |
|---|---|---|
| Closed thickness | ~27 mm | **21.5 mm** |
| Screen window | 15.5 × 26.5 mm straight-walled, 2 mm wall | 13.5 × 25 mm over the OLED's dark area, 1.6 mm wall bevelled 45° |
| Lip fit | 0 mm clearance (lip 29.0 × 66.3 into a 29.0 × 66.2 opening) | 0.2 mm per side (`clr`) |
| Closing | friction only | snap ridges on both long sides (`snap`) |
| USB-C opening | 10 × 4 mm (most cable plugs don't seat) | 9.8 × 4.2 mm around the socket, which reaches to 0.9 mm from the outside |
| Battery | loose in the back cavity | 5.5 mm ribs hold an 802540 (8 × 25 × 40 mm); the wire end has open corners so the leads can't jam |
| Buzzer | glued | press-fit ring (crush ribs) + 7-hole sound grille; solder wells under its two pin holes |
| Button | glued | socket for a 6 × 6 mm tactile switch |
| Board | 0.7 mm pin holes in rails | rails + side guides; USB-C end tucks into its opening, the other end clicks under two angled snap hooks along the board's cut corners (0.1 mm play) |

## Files

| File | What |
|---|---|
| `stl/classic-front.stl`, `stl/classic-back.stl` | Box shape like v1 with small edge chamfers |
| `stl/pebble-front.stl`, `stl/pebble-back.stl` | Same internals, rounded front/back edges and corners |
| `stl/pebble-front-prg-key.stl` | Pebble front with a flexing key over the Heltec's own PRG button instead of a wired button (use with `stl/pebble-back.stl`). **Recommended.** |
| `stl/classic-front-prg-key.stl` | Same, in the classic box shape (use with `stl/classic-back.stl`) |
| `stl/prg-test-A.stl`, `-B`, `-C` | The USB-C end of that front (~12 min each), with the key's nub 0.3 / 0.6 / 0.9 mm above the button. Pick the one that clicks when pressed and not at rest |
| `stl/pebble-back-button-stopper.stl` | Pebble back with a post that holds the wired button's switch against the front wall, so it can't sink (use with `stl/pebble-front.stl`) |
| `stl/screen-test.stl` | Front wall + rails + snap hooks (~10 min). Click the board in to check the hooks, screen depth and window position before printing a full case |
| `stl/fit-test.stl` | The USB-C end of both halves (~15 min). Checks the lip/snap fit and the USB-C opening |
| `satoshipet_case.scad`, `artwork.scad` | OpenSCAD source; every dimension is a named parameter at the top |

All parts are already oriented for printing (outside face down, no supports needed).

## Printing

- PLA or PETG, 0.2 mm layers, 3 walls, 15–20 % infill, no supports.
- Print the two test pieces first:
  1. **screen-test**: the board should sit flat on the rails (screen glass just touching the
     front wall) with only the dark screen area showing in the window. If the board rocks on
     the glass, raise `oled_stack` (measured glass height + 0.2 mm). Adjust `win_cx`/`win_cy`
     if the window is off-centre.
  2. **fit-test**: the halves should press together with a click and pull apart by hand.
     Too tight → raise `clr` to 0.25–0.3 or lower `snap`; too loose → lower `clr` to 0.15 or raise `snap` to 0.45.
- Then print the front and back in the style you like.

## Assembly (PRG-key front)

1. Press the buzzer into its ring (face toward the grille, leads out through the slot toward the
   board). Three crush ribs hold it snug.
2. Put the board in screen-first, **USB-C end first**: tip it so the socket goes into the end-wall
   opening, then press the other end down until it clicks under the two hooks. The hooks sit along
   the board's angled corner edges at the keyring end (measured on a V3.1: 10 mm middle flat,
   corners set back 4 mm), so they hold it down, stop it sliding toward the keyring and centre it.
3. Wire the buzzer to the first two holes at the keyring end, on the row whose end holes are:

   | Board | Red (+), end hole, tone | White (−), next hole, held low |
   |---|---|---|
   | WiFi Kit 32 V3.1 | GPIO 46 | GPIO 45 |
   | WiFi LoRa 32 V3.2 (HTIT-WB32LAF) | GPIO 7 | GPIO 6 |

   (Not the other row: on the LoRa board its end holes are 19/20, the USB data pins.) Solder the
   wires to the buzzer's prongs first, run them flat over the top of the board, push each end
   **down** through its hole and solder on top. The rails are cut away under those holes (and under
   the matching holes on the other row), so the wire ends drop into the well instead of hitting the
   rail; trim them to ~2 mm. Keep the wires flat: the battery sits ~3 mm above the board just past
   these holes. Nothing runs to the GND pin at the far end.
4. Put the battery in the back half with its **wire end toward the keyring** (the end with the single
   centre stop); the leads leave through either open corner. A thin foam pad on top keeps it from
   rattling. Plug it in.
5. Snap the halves together and thread the keyring through the corner tube.

To take the board out, push the hooks outward with a fingernail and lift that end.

![snap hooks](previews/board-hooks.png) ![snap hooks, top](previews/board-hooks-top.png) ![solder wells and buzzer ribs](previews/solder-wells.png) ![battery pocket](previews/battery-pocket.png)

Earlier wiring (buzzer on GPIO 48 + GND, wired button on GPIO 2) still works with the same firmware.

## Button stopper (wired button)

![side cut](previews/pebble-button-stopper-side.png)

`pebble-back-button-stopper.stl` adds a tapered post (3.2 mm tip) that comes
down from the back wall to 0.3 mm above the switch body when the case is
closed. Pressing the button pushes against the post instead of glue. The
post sits at the keyring end, past the end of the board and about 6 mm
short of the battery pocket. Bend the switch's two legs flat (sideways),
not straight back, so they lie beside the post.

## PRG key variant (no wired button)

![pebble front with PRG key](previews/pebble-prg-key-front.png)

The Heltec already has a PRG button beside its USB-C port, and the firmware
reads it as well as the wired button. In `pebble-front-prg-key.stl` a round pad
is cut free from the front wall on three sides and hangs off a thinned arm; a
nub under it presses PRG. No button to solder or glue. The Bitcoin logo moves
right and shrinks a little to make room, and the v1 pinhole is dropped.

1. Print `prg-test-B` first. Set it over the USB-C end of the board (screen
   side down, USB-C against the end wall) and press the pad.
   - Clicks when pressed, nothing at rest: print `pebble-front-prg-key.stl`.
   - Needs a hard press or doesn't click: try `prg-test-A` (longer nub).
   - Clicks without pressing: try `prg-test-C` (shorter nub).
2. If A or C was the right one, rebuild the front with that clearance:
   `openscad -D 'PART="front"' -D 'STYLE="pebble"' -D 'PRG_KEY=true' -D 'prg_gap=0.3' -o front.stl satoshipet_case.scad`
   (`0.3` for A, `0.9` for C).

Holding PRG while plugging in USB starts the ESP32's flashing mode, so don't
press the key while connecting the cable.

## Changing it

Install [OpenSCAD](https://openscad.org) (free), open `satoshipet_case.scad`, change a parameter,
press F6 then File → Export → STL. Or from a terminal:

```bash
openscad -D 'PART="front"' -D 'STYLE="pebble"' -o front.stl satoshipet_case.scad
```

`PART` is `front`, `back`, `screen_test`, `fit_test`, `prg_test` or `assembly`; `STYLE` is `classic` or `pebble`; `PRG_KEY` is `true` or `false`.
