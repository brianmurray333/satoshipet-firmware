// Satoshi Pet case v2 — parametric OpenSCAD source
// Heltec WiFi Kit 32 V3 + 802540 LiPo (8 x 25 x 40 mm) + 12 mm buzzer + 6x6 tactile button
//
// Coordinates (assembled): x across the width, y along the length
// (y = 0 is the keyring/buzzer end, y = L is the USB-C end), z = 0 is the
// front face (screen side). Each part is rotated flat for printing on export.
//
// Render one part at a time:
//   openscad -D 'PART="front"'  -D 'STYLE="classic"' -o front.stl satoshipet_case.scad
//   PART: front | back | screen_test | fit_test | prg_test | assembly
//   STYLE: classic | pebble
//   PRG_KEY: false | true   (openscad -D 'PRG_KEY=true' ...)
//   BTN_BACKSTOP: false | true  (back half: post that holds the wired button)

include <artwork.scad>

PART  = "assembly";
STYLE = "classic";
BTN_BACKSTOP = false;  // true: a post on the back half presses the wired button's
                       // switch against the front wall so it can't sink
PRG_KEY = true;    // true: a flexing key in the front wall presses the Heltec's own
                   // PRG button (no wired button, no button hole or socket)

/* ---------- main dimensions ---------- */
W      = 33;     // outer width  (same as v1)
L      = 70.2;   // outer length (same as v1)
R      = 3;      // corner radius (top view)
T      = 21.5;   // total closed thickness (v1 was ~27)
split  = 9.5;    // front-half height; back half = T - split
wall   = 1.8;    // side walls
floor_f= 1.6;    // front wall (thinner = screen closer to the surface)
floor_b= 1.6;    // back wall
edge_r = 3;      // pebble style: rounding of the front/back edges
R_peb  = 6;      // pebble style: corner radius seen from the front

/* ---------- fit ---------- */
clr    = 0.2;    // clearance per side between lip and front half
lip_h  = 3;      // lip height (back half slides into front half)
lip_t  = 1.4;    // lip thickness
snap   = 0.35;   // snap ridge height on lip (0 = friction only)
snap_len = 14;   // snap ridge length on each long side

/* ---------- board ---------- */
board_w   = 25.5;  // Heltec V3 PCB width
board_l   = 50.2;  // Heltec V3 PCB length (datasheet), measured through the middle of the ends.
                   // The USB-C end of the PCB rests against the inside of the end wall; the
                   // socket sticks out 0.9 mm past the PCB into the USB opening.
pcb_notch = 1.1;   // the PCB's corners are cut back 1.1 mm at each end (long edges are 48.0)
stop_play = 0.2;   // lengthwise play between the board and the end stops
pcb_t     = 1.6;
oled_stack= 5.2;   // gap from the front wall to the PCB: the OLED glass and
                   // the USB-C socket sit on this side. Measured glass on a
                   // V3: 5 mm tall x 33 x 19 mm, plus 0.2 mm clearance.
rail_w    = 2.0;   // PCB rests on these along its long edges
// Snap hooks at the keyring end of the board: the USB-C end goes in first (its
// socket sits in the end-wall opening, which holds that end down), then the
// other end is pressed down and clicks under the hooks. Their faces also stop
// the board lengthwise. They sit 2.2 mm in from the long edges, clear of the
// PCB's 1.1 mm cut-back corners.
hook_w    = 4.0;   // across
hook_t    = 1.2;   // post thickness along the board (thinner = softer click)
hook_lip  = 0.6;   // how far the hook reaches over the PCB end
hook_play = 0.1;   // lengthwise play: hook face to PCB end
hook_zgap = 0.1;   // vertical play under the hook
hook_in   = 2.2;   // hook position in from the PCB's long edges
// Solder wells: the rails are cut away under the last two pin holes at the
// keyring end (GPIO 46/45 on one row, 1/2 on the other), so the buzzer leads
// can come up through the holes from the screen side and be soldered on top.
// Heltec V3 headers: 18 holes at 2.54 mm, first hole centre ~3.5 mm from the
// PCB end, hole centres ~1.3 mm in from the long edges.
pin_pitch  = 2.54;
pin_first  = 3.5;   // first hole centre from the PCB's keyring end
well_holes = 2;     // holes cleared per row
well_pad   = 1.6;   // extra length past the outer hole centres

/* ---------- screen window ---------- */
win_cx = 13.3;                 // across: ~3 mm toward one pin row (fit confirmed on the v2 print)
win_cy = L - wall - 31.0 - stop_play/2;  // along: OLED centre is 31.0 mm from the PCB's USB-C end
win_x  = 13.5; win_y  = 25;    // the OLED's dark area measured 25 mm long on the board
win_chamfer = true;            // 45 deg bevel out to the front face

/* ---------- other parts ---------- */
key_c   = [6, 6.6];  key_d = 5.5; key_od = 9;    // keyring tube through both halves
buz_c   = [16.3, 9.6]; buz_d = 12.4; buz_h = 5;  // buzzer ring, sound grille in front
buz_grip = 11.9;     // three crush ribs inside the ring: a 12 mm buzzer presses in snug
btn_c   = [27, 13];  btn_hole = 3.8; btn_sock = 6.4; btn_sock_h = 3.0; // 6x6 tactile
btn_body_h = 3.5;    // switch body height (6x6 switches: 3.5 mm whatever the plunger length)
stop_gap   = 0.3;    // backstop clearance above the switch body when closed
stop_tip   = 3.2;    // backstop tip (square); fits between the 2 legs at the body's edges
pin_c   = [25, 63.6]; pin_d = 2.0;               // pinhole (kept from v1)
usb_w   = 9.8;  usb_h = 4.2;                      // USB-C opening: the socket's metal shell (8.9 x 3.2) + ~0.45 mm a side
usb_z0  = floor_f + oled_stack - 1.6 - usb_h/2;   // centred on the socket (3.2 mm tall, under the PCB)
bat     = [25.6, 40.6, 8.6];                     // battery pocket (802540 + clearance)
bat_cy  = 43;                                    // pocket centre along y
bat_rib_h  = 5.5;   // pocket ribs (the battery is 8 mm thick)
bat_stop_w = 8;     // wire end (keyring end): one centre stop, both corners open
                    // so the leads can leave from either corner
engrave = 0.7;                                   // artwork depth

/* ---------- PRG key (PRG_KEY = true) ---------- */
// The Heltec V3's PRG button sits on the screen side, 3.7 mm from the PCB's
// USB-C end and 4.7 mm in from the long edge the screen is offset toward
// (Heltec HTIT-WB32LA V3.2 drawing). A round pad cut free from the front wall
// on three sides hangs off a thinned arm; a nub under the pad reaches down to
// the button.
prg_btn_h = 1.5;    // PRG button height above the PCB (measure yours; see prg_test)
prg_gap   = 0.6;    // nub clearance above the button at rest (prg-test-B)
key_pad_d = 6.5;    // pad you press
key_slot  = 0.6;    // cut around pad and arm
key_arm_w = 3.5;
key_arm_l = 6;      // arm length from the pad edge to where it joins the wall
key_arm_t = 0.9;    // arm thickness (front wall is floor_f); thinner = softer
key_nub_d = 2.4;
logo_shift_prg = 3.6;   // logo moves right by this much with the key...
logo_scale_prg = 0.85;  // ...and shrinks to fit (the v1 pinhole is dropped)

$fn = 48;
eps = 0.01;

board_x0 = (W - board_w) / 2;
board_y0 = L - wall - board_l;
pcb_z0   = floor_f + oled_stack;

prg_c   = [board_x0 + 4.7, L - wall - 3.7];      // PRG button centre
pad_c   = [prg_c[0] + 0.15, prg_c[1] - 1.7];     // pad moved off the rounded corner; nub stays over PRG
arm_y0  = pad_c[1] - key_pad_d/2 - key_arm_l;    // where the arm joins the wall
nub_end = pcb_z0 - prg_btn_h - prg_gap;          // nub tip (z)

/* ---------- helpers ---------- */
module rrect(w, l, r, h) {
  r2 = max(min(r, w/2 - eps, l/2 - eps), eps);
  translate([r2, r2, 0]) linear_extrude(h) offset(r = r2) square([w - 2*r2, l - 2*r2]);
}

// outer body of the whole closed case
module outer_body() {
  if (STYLE == "pebble") {
    hull() for (x = [R_peb, W - R_peb], y = [R_peb, L - R_peb], z = [edge_r, T - edge_r])
      translate([x, y, z]) {
        // corner piece: front/back edges rounded by edge_r, corner radius R_peb
        rotate_extrude($fn = 48) translate([R_peb - edge_r, 0]) circle(r = edge_r - 0.001, $fn = 32);
      }
  } else {
    // classic: v1 box with a 0.8 mm chamfer on the front/back edges
    c = 0.8;
    hull() {
      translate([c, c, 0]) rrect(W - 2*c, L - 2*c, R - c, eps);
      translate([0, 0, c]) rrect(W, L, R, T - 2*c);
      translate([c, c, T - eps]) rrect(W - 2*c, L - 2*c, R - c, eps);
    }
  }
}

module cavity() {  // inside of the closed case
  translate([wall, wall, floor_f]) rrect(W - 2*wall, L - 2*wall, R - wall, T - floor_f - floor_b);
}

module lip_ring(grow = 0) {
  ow = W - 2*wall - 2*clr + 2*grow;
  ol = L - 2*wall - 2*clr + 2*grow;
  o  = wall + clr - grow;
  difference() {
    translate([o, o, 0]) rrect(ow, ol, R - wall, lip_h);
    translate([o + lip_t, o + lip_t, -eps]) rrect(ow - 2*lip_t, ol - 2*lip_t, R - wall - lip_t, lip_h + 2*eps);
  }
}

module snap_ridges(extra = 0) {  // on the outside of the lip's long sides
  if (snap > 0)
    for (side = [0, 1]) {
      x = side == 0 ? wall + clr : W - wall - clr;
      translate([x, L/2 - snap_len/2, split - lip_h/2])
        rotate([-90, 0, 0]) cylinder(r = snap + extra, h = snap_len, $fn = 16);
    }
}

/* ---------- PRG key pieces ---------- */
module key_2d() {  // pad + arm outline
  translate(pad_c) circle(d = key_pad_d);
  translate([pad_c[0] - key_arm_w/2, arm_y0]) square([key_arm_w, pad_c[1] - arm_y0]);
}
module key_slot_cut() {  // U-shaped cut through the front wall, open at the arm root
  translate([0, 0, -1]) linear_extrude(floor_f + 2) difference() {
    offset(delta = key_slot) key_2d();
    key_2d();
    translate([pad_c[0] - key_arm_w/2 - key_slot - 1, arm_y0 - key_slot - 1]) square([key_arm_w + 2*key_slot + 2, key_slot + 1.05]);
  }
  // thin the arm from the inside so it flexes
  translate([pad_c[0] - key_arm_w/2, arm_y0 - 0.05, key_arm_t]) cube([key_arm_w, pad_c[1] - arm_y0 - key_pad_d/2 + 0.6, floor_f]);
  // keep the board rail off the pad and arm (the side guide outboard of it stays)
  translate([board_x0 - 0.1, arm_y0 - key_slot, floor_f + eps])
    cube([pad_c[0] + key_pad_d/2 + key_slot - board_x0 + 0.1, pad_c[1] + key_pad_d/2 + key_slot - arm_y0 + key_slot, oled_stack]);
}
module key_nub() {
  translate([prg_c[0], prg_c[1], floor_f - eps]) cylinder(d = key_nub_d, h = nub_end - floor_f + eps, $fn = 24);
}

/* ---------- board snap hooks ---------- */
module board_hooks() {
  z_top = pcb_z0 + pcb_t + hook_zgap;    // underside of the hook lip
  y1 = board_y0 - hook_play;             // hook face (meets the PCB end)
  // with the wired button its socket sits where the second hook goes
  for (x = PRG_KEY ? [board_x0 + hook_in, board_x0 + board_w - hook_in - hook_w]
                   : [board_x0 + hook_in])
    translate([x, 0, 0]) {
      translate([0, y1 - hook_t, floor_f - eps]) cube([hook_w, hook_t, z_top - floor_f + eps]);
      // lip over the PCB, ramped on top so the board pushes the hook back
      hull() {
        translate([0, y1 - hook_t, z_top]) cube([hook_w, hook_t + hook_lip, 0.4]);
        translate([0, y1 - hook_t, z_top + 1.2]) cube([hook_w, hook_t, eps]);
      }
    }
}

/* ---------- front half (screen side) ---------- */
module front_half() {
  union() {
  difference() {
    union() {
      difference() {
        intersection() { outer_body(); translate([-1, -1, -1]) cube([W + 2, L + 2, split + 1]); }
        cavity();
      }
      // board rails + side guides
      for (s = [0, 1]) {
        xr = s == 0 ? board_x0 : board_x0 + board_w - rail_w;
        translate([xr, board_y0, floor_f - eps]) cube([rail_w, board_l, oled_stack + eps]);
        xg = s == 0 ? wall - eps : board_x0 + board_w + 0.15;
        gw = s == 0 ? board_x0 - 0.15 - wall + eps : W - wall - xg + eps;
        translate([xg, board_y0, floor_f - eps]) cube([gw, board_l, min(oled_stack + pcb_t, split - lip_h - 0.3 - floor_f) + eps]);
      }
      board_hooks();
      // keyring tube
      translate([key_c[0], key_c[1], 0]) cylinder(d = key_od, h = split);
      // buzzer ring
      translate([buz_c[0], buz_c[1], floor_f - eps]) difference() {
        union() {
          difference() {
            cylinder(d = buz_d + 2, h = buz_h);
            translate([0, 0, -eps]) cylinder(d = buz_d, h = buz_h + 1);
          }
          // crush ribs, chamfered at the top so the buzzer starts in easily
          for (a = [30, 150, 270]) rotate(a) translate([buz_grip/2 + 0.6, 0, 0])
            hull() { cylinder(r = 0.6, h = buz_h - 1, $fn = 12); translate([0.5, 0, 0]) cylinder(r = 0.6, h = buz_h, $fn = 12); }
        }
        translate([-1.5, 0, 1]) cube([3, 10, buz_h]);  // wire exit, toward the board
      }
      // tactile button socket (not needed with the PRG key)
      if (!PRG_KEY) translate([btn_c[0] - btn_sock/2 - 1, btn_c[1] - btn_sock/2 - 1, floor_f - eps])
        difference() {
          cube([btn_sock + 2, btn_sock + 2, btn_sock_h]);
          translate([1, 1, -eps]) cube([btn_sock, btn_sock, btn_sock_h + 1]);
          translate([btn_sock/2 - 1, -eps, 1]) cube([2, 1.2, btn_sock_h]);  // leg/wire slot
        }
    }
    // solder wells in both rails under the first holes at the keyring end
    for (xr = [board_x0, board_x0 + board_w - rail_w])
      translate([xr - eps, board_y0 + pin_first - well_pad, floor_f])
        cube([rail_w + 2*eps, (well_holes - 1) * pin_pitch + 2*well_pad, oled_stack + 1]);
    // screen window (+ bevel)
    translate([win_cx, win_cy, 0]) {
      translate([-win_x/2, -win_y/2, -1]) cube([win_x, win_y, floor_f + 2]);
      if (win_chamfer)
        hull() {
          translate([-win_x/2, -win_y/2, floor_f - eps]) cube([win_x, win_y, eps]);
          translate([-win_x/2 - floor_f, -win_y/2 - floor_f, -eps]) cube([win_x + 2*floor_f, win_y + 2*floor_f, eps]);
        }
    }
    // keyring hole
    translate([key_c[0], key_c[1], -1]) cylinder(d = key_d, h = T + 2);
    // buzzer sound grille
    translate([buz_c[0], buz_c[1], -1])
      for (a = [0:60:359]) rotate(a) translate([3, 0, 0]) cylinder(d = 1.4, h = floor_f + 2, $fn = 12);
    translate([buz_c[0], buz_c[1], -1]) cylinder(d = 1.4, h = floor_f + 2, $fn = 12);
    // button hole + pinhole
    if (!PRG_KEY) translate([btn_c[0], btn_c[1], -1]) cylinder(d = btn_hole, h = floor_f + 2);
    if (PRG_KEY) key_slot_cut();
    if (!PRG_KEY) translate([pin_c[0], pin_c[1], -1]) cylinder(d = pin_d, h = floor_f + 2, $fn = 16);
    // USB-C opening in the end wall
    translate([W/2 - usb_w/2, L - wall - 1, usb_z0]) cube([usb_w, wall + 2, usb_h]);
    // groove for the snap ridges
    snap_ridges(extra = 0.1);
    // Bitcoin logo, engraved in the front face
    // (with the PRG key the logo moves right and shrinks a little to clear the key)
    translate([W/2 + (PRG_KEY ? logo_shift_prg : 0), L/2, -eps])
      linear_extrude(engrave) translate([0, 25.2]) scale(PRG_KEY ? logo_scale_prg : 1) translate([0, -25.2]) btc_logo();  // scale about the logo's centre
  }
  if (PRG_KEY) key_nub();
  }
}

/* ---------- back half (battery side) ---------- */
module back_half() {
  difference() {
    union() {
      difference() {
        intersection() { outer_body(); translate([-1, -1, split]) cube([W + 2, L + 2, T]); }
        cavity();
      }
      // lip that slides into the front half
      difference() {
        union() {
          translate([0, 0, split - lip_h]) lip_ring();
          // bridge the lip to the back half's walls (lip sits clr inside them)
          translate([0, 0, split - eps]) difference() {
            translate([wall - eps, wall - eps, 0]) rrect(W - 2*wall + 2*eps, L - 2*wall + 2*eps, R - wall, 2);
            translate([wall + clr + lip_t, wall + clr + lip_t, -1]) rrect(W - 2*(wall + clr + lip_t), L - 2*(wall + clr + lip_t), R - wall - lip_t, 4);
          }
        }
        translate([key_c[0], key_c[1], split - lip_h - 1]) cylinder(d = key_od + 2*clr + 0.4, h = lip_h + 2);
        // clear the PCB end at the USB-C wall
        translate([board_x0 - 0.3, L - wall - lip_t - clr - 1, split - lip_h - 1]) cube([board_w + 0.6, lip_t + clr + 1.5, pcb_z0 + pcb_t + 0.4 - (split - lip_h) + 1]);
      }
      snap_ridges();
      // keyring tube
      translate([key_c[0], key_c[1], split]) cylinder(d = key_od, h = T - split);
      // button backstop: tapered post from the back wall down to just above the
      // switch body, between its legs (the battery pocket starts ~6 mm further in)
      if (BTN_BACKSTOP) hull() {
        translate([btn_c[0] - stop_tip/2, btn_c[1] - stop_tip/2, floor_f + btn_body_h + stop_gap]) cube([stop_tip, stop_tip, 1]);
        translate([btn_c[0] - 2.5, btn_c[1] - 2.5, T - floor_b - 1]) cube([5, 5, 1 + eps]);
      }
      // battery pocket: corner ribs at the USB-C end; at the wire end (keyring
      // end) a single centre stop, so the leads leave freely from either corner
      translate([W/2 - bat[0]/2, bat_cy - bat[1]/2, T - floor_b - bat_rib_h]) {
        for (cx = [0, 1])
          translate([cx * bat[0], bat[1], 0]) mirror([cx, 0, 0]) mirror([0, 1, 0])
            difference() {
              translate([-1.2, -1.2, 0]) cube([7, 7, bat_rib_h + eps]);
              translate([0, 0, -1]) cube([8, 8, bat_rib_h + 2]);
            }
        // side ribs along the long edges, starting clear of the wire corners
        for (cx = [0, 1])
          translate([cx * bat[0] - (cx ? 0 : 1.2), 8, 0]) cube([1.2, 14, bat_rib_h + eps]);
        // centre stop at the wire end
        translate([bat[0]/2 - bat_stop_w/2, -1.2, 0]) cube([bat_stop_w, 1.2, bat_rib_h + eps]);
      }
    }
    translate([key_c[0], key_c[1], -1]) cylinder(d = key_d, h = T + 2);
    // paw + heart, engraved in the back face
    translate([W/2, L/2, T - engrave + eps]) linear_extrude(engrave) paw_heart();
  }
}

/* ---------- print orientation ---------- */
module print_front() front_half();
module print_back()  translate([0, L, T]) rotate([180, 0, 0]) back_half();

// quick check of screen depth / window position: front wall + rails only
module screen_test() {
  intersection() {
    front_half();
    translate([0, board_y0 - 2, -1]) cube([W, board_l + 4, floor_f + oled_stack + pcb_t + 1]);
  }
}
// quick check of lip fit, snap and USB-C opening: the last 20 mm of both halves
module fit_test() {
  translate([0, -(L - 20), 0]) intersection() { front_half(); translate([-1, L - 20, -1]) cube([W + 2, 21, T + 2]); }
  translate([W + 6, 0, 0])
    intersection() { print_back(); translate([-1, -1, -1]) cube([W + 2, 21, T + 2]); }
}

// quick check of the PRG key: the USB-C end of the front half (~12 min).
// Lay it over the board's USB-C end and press: one click, and no click at rest.
module prg_test() {
  intersection() { front_half(); translate([-1, arm_y0 - 4, -1]) cube([W + 2, L, split + 2]); }
}

if (PART == "front") print_front();
else if (PART == "back") print_back();
else if (PART == "screen_test") screen_test();
else if (PART == "fit_test") fit_test();
else if (PART == "prg_test") prg_test();
else { color("hotpink") front_half(); color("lightsteelblue") translate([0, 0, 6]) back_half(); }
