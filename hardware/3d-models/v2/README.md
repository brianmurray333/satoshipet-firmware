# Satoshi Pet case v2

A parametric rebuild of the v1 Tinkercad case (`../case-v1.stl`), keeping its layout
(screen window, keyring corner, front button, Bitcoin logo, paw on the back) and fixing
what didn't work in v1.

![classic](previews/classic-sheet.png)

## What changed from v1

| | v1 | v2 |
|---|---|---|
| Closed thickness | ~27 mm | **20 mm** |
| Screen depth (front face → board) | 2 mm wall + 5.2 mm gap | 1.6 mm wall + 3.8 mm gap, window bevelled 45° |
| Lip fit | 0 mm clearance (lip 29.0 × 66.3 into a 29.0 × 66.2 opening) | 0.2 mm per side (`clr`) |
| Closing | friction only | snap ridges on both long sides (`snap`) |
| USB-C opening | 10 × 4 mm (most cable plugs don't seat) | 12.5 × 6.8 mm |
| Battery | loose in the back cavity | corner ribs hold an 802540 (8 × 25 × 40 mm) |
| Buzzer | glued | ring holder + 7-hole sound grille |
| Button | glued | socket for a 6 × 6 mm tactile switch |
| Board | 0.7 mm pin holes in rails | rails + side guides + end stops, board drops in |

## Files

| File | What |
|---|---|
| `stl/classic-front.stl`, `stl/classic-back.stl` | Box shape like v1 with small edge chamfers |
| `stl/pebble-front.stl`, `stl/pebble-back.stl` | Same internals, rounded front/back edges and corners |
| `stl/screen-test.stl` | Front wall + rails only (~10 min). Drop the board in to check screen depth and window position before printing a full case |
| `stl/fit-test.stl` | The USB-C end of both halves (~15 min). Checks the lip/snap fit and the USB-C opening |
| `satoshipet_case.scad`, `artwork.scad` | OpenSCAD source; every dimension is a named parameter at the top |

All parts are already oriented for printing (outside face down, no supports needed).

## Printing

- PLA or PETG, 0.2 mm layers, 3 walls, 15–20 % infill, no supports.
- Print the two test pieces first:
  1. **screen-test**: the board's screen should sit just behind the front wall with the
     display centred in the window. If the board doesn't sit flat on the rails, raise
     `oled_stack`; if the screen sits far back, lower it (measure PCB → top of the screen glass + 0.3 mm).
  2. **fit-test**: the halves should press together with a click and pull apart by hand.
     Too tight → raise `clr` to 0.25–0.3 or lower `snap`; too loose → lower `clr` to 0.15 or raise `snap` to 0.45.
- Then print the front and back in the style you like.

## Assembly

1. Press the 6 × 6 mm tactile switch into its socket (actuator through the front hole), wire to GPIO 2 + GND.
2. Press the buzzer into its ring (face toward the grille), wires out through the slot toward the board.
3. Drop the board in screen-first; it rests on the rails with the USB-C socket at the opening.
4. Put the battery in the back half between the corner ribs (a thin foam pad on top keeps it from rattling), plug it in.
5. Snap the halves together and thread the keyring through the corner tube.

## Changing it

Install [OpenSCAD](https://openscad.org) (free), open `satoshipet_case.scad`, change a parameter,
press F6 then File → Export → STL. Or from a terminal:

```bash
openscad -D 'PART="front"' -D 'STYLE="pebble"' -o front.stl satoshipet_case.scad
```

`PART` is `front`, `back`, `screen_test`, `fit_test` or `assembly`; `STYLE` is `classic` or `pebble`.
