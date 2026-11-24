// Satoshi Pet case v2 — parametric OpenSCAD source
// Heltec WiFi Kit 32 V3 + 802540 LiPo (8 x 25 x 40 mm) + 12 mm buzzer + 6x6 tactile button
//
// Coordinates (assembled): x across the width, y along the length
// (y = 0 is the keyring/buzzer end, y = L is the USB-C end), z = 0 is the
// front face (screen side). Each part is rotated flat for printing on export.
//
// Render one part at a time:
//   openscad -D 'PART="front"'  -D 'STYLE="classic"' -o front.stl satoshipet_case.scad
//   PART: front | back | screen_test | fit_test | assembly
//   STYLE: classic | pebble

include <artwork.scad>

PART  = "assembly";
STYLE = "classic";

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
board_l   = 50.6;  // PCB length incl. USB-C + clearance
pcb_t     = 1.6;
oled_stack= 5.2;   // gap from the front wall to the PCB: the OLED glass and
                   // the USB-C socket sit on this side. Measured glass on a
                   // V3: 5 mm tall x 33 x 19 mm, plus 0.2 mm clearance.
rail_w    = 2.0;   // PCB rests on these along its long edges

/* ---------- screen window ---------- */
win_cx = 13.3; win_cy = 38.3;  // centre of the OLED's dark area (~20.5 mm from the board's pointed end, ~3 mm toward one pin row)
win_x  = 13.5; win_y  = 27;    // a little inside the dark area (~14 x 28 mm) so no glass edge shows
win_chamfer = true;            // 45 deg bevel out to the front face

/* ---------- other parts ---------- */
key_c   = [6, 6.6];  key_d = 5.5; key_od = 9;    // keyring tube through both halves
buz_c   = [16.3, 9.6]; buz_d = 12.4; buz_h = 4;  // buzzer ring, sound grille in front
btn_c   = [27, 13];  btn_hole = 3.8; btn_sock = 6.4; btn_sock_h = 3.0; // 6x6 tactile
pin_c   = [25, 63.6]; pin_d = 2.0;               // pinhole (kept from v1)
usb_w   = 12.5; usb_h = 6.6;                      // USB-C opening, fits most cable plugs
usb_z0  = floor_f + oled_stack - 1.6 - usb_h/2;   // centred on the socket (3.2 mm tall, under the PCB)
bat     = [25.6, 40.6, 8.6];                     // battery pocket (802540 + clearance)
bat_cy  = 43;                                    // pocket centre along y
engrave = 0.7;                                   // artwork depth

$fn = 48;
eps = 0.01;

board_x0 = (W - board_w) / 2;
board_y0 = L - wall - board_l;
pcb_z0   = floor_f + oled_stack;

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

/* ---------- front half (screen side) ---------- */
module front_half() {
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
        // end stops on the buzzer side of the board
        translate([xr - (s == 0 ? 0 : 0), board_y0 - 1.5, floor_f - eps]) cube([rail_w, 1.5, oled_stack + pcb_t + eps]);
      }
      // keyring tube
      translate([key_c[0], key_c[1], 0]) cylinder(d = key_od, h = split);
      // buzzer ring
      translate([buz_c[0], buz_c[1], floor_f - eps]) difference() {
        cylinder(d = buz_d + 2, h = buz_h);
        translate([0, 0, -eps]) cylinder(d = buz_d, h = buz_h + 1);
        translate([-1.5, 0, 1]) cube([3, 10, buz_h]);  // wire exit
      }
      // tactile button socket
      translate([btn_c[0] - btn_sock/2 - 1, btn_c[1] - btn_sock/2 - 1, floor_f - eps])
        difference() {
          cube([btn_sock + 2, btn_sock + 2, btn_sock_h]);
          translate([1, 1, -eps]) cube([btn_sock, btn_sock, btn_sock_h + 1]);
          translate([btn_sock/2 - 1, -eps, 1]) cube([2, 1.2, btn_sock_h]);  // leg/wire slot
        }
    }
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
    translate([btn_c[0], btn_c[1], -1]) cylinder(d = btn_hole, h = floor_f + 2);
    translate([pin_c[0], pin_c[1], -1]) cylinder(d = pin_d, h = floor_f + 2, $fn = 16);
    // USB-C opening in the end wall
    translate([W/2 - usb_w/2, L - wall - 1, usb_z0]) cube([usb_w, wall + 2, usb_h]);
    // groove for the snap ridges
    snap_ridges(extra = 0.1);
    // Bitcoin logo, engraved in the front face
    translate([W/2, L/2, -eps]) mirror([0, 0, 0]) linear_extrude(engrave) btc_logo();
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
      // battery pocket: four corner ribs (gaps let wires pass)
      translate([W/2 - bat[0]/2, bat_cy - bat[1]/2, T - floor_b - 3]) {
        for (cx = [0, 1], cy = [0, 1])
          translate([cx * bat[0], cy * bat[1], 0]) mirror([cx, 0, 0]) mirror([0, cy, 0])
            difference() {
              translate([-1.2, -1.2, 0]) cube([7, 7, 3 + eps]);
              translate([0, 0, -1]) cube([8, 8, 5]);
            }
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

if (PART == "front") print_front();
else if (PART == "back") print_back();
else if (PART == "screen_test") screen_test();
else if (PART == "fit_test") fit_test();
else { color("hotpink") front_half(); color("lightsteelblue") translate([0, 0, 6]) back_half(); }
