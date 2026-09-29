// Wasserraketen-Tracker — Deckel
// Passt auf rocket_case.scad v4
// Nur Display-Fenster offen · M2-Schrauben an den 4 PCB-Bossen

// ── Parameter (identisch mit rocket_case.scad) ────────────────────────────────
wall      = 1.4;
tol       = 0.3;

board_w   = 28.6;
board_l   = 43.5;
board_h   = 9.6;
sensor_l  = 37.0;
gap       = 2.0;
bat_w     = 30.0;
bat_l     = 44.0;

disp_w    = 22.34;
disp_h    = 36.06;
disp_off_x =  3.1;
disp_off_y =  2.3;

inner_w = max(board_w, bat_w) + tol * 2;
inner_l = board_l + gap + sensor_l + gap + bat_l + tol * 2;
inner_h = board_h + 3.0;

outer_w = inner_w + wall * 2;
outer_l = inner_l + wall * 2;
outer_h = inner_h + wall;

fairing  = 8.0;   // identisch mit rocket_tracker.scad
total_l  = outer_l + fairing * 2;

// Deckeldicke + Zunge
lid_h    = wall;        // obere Platte
tongue_h = 2.5;         // Zunge greift in Gehäuse
tongue_t = 0.1;         // Spiel Zunge → Gehäuse (Presspassung)

// Display-Fenster (gleiche Offsets wie Gehäuse)
disp_ox = disp_off_x + tol;
disp_oy = disp_off_y + tol + fairing;

// Deckel-Schraubenlöcher (gleiche Positionen wie Lid-Bossen im Gehäuse)
lid_m2_d    = 2.4;
lid_boss_r  = 3.0;
lid_y1      = fairing + wall + board_l + gap + 8;
lid_y2      = fairing + wall + board_l + gap + sensor_l - 8;

// ── Hilfsfunktionen ───────────────────────────────────────────────────────────
module rounded_box(w, l, h, r = 2) {
    hull()
        for (x = [r, w - r], y = [r, l - r])
            translate([x, y, 0]) cylinder(r = r, h = h, $fn = 24);
}

module fairing_shell(h) {
    hw = outer_w / 2;
    hull() {
        translate([0, fairing, 0])
            rounded_box(outer_w, outer_l, h, 3);
        translate([hw - 3, 0, 0])
            cylinder(r = 3, h = h * 0.3, $fn = 24);
        translate([hw - 3, 0, h * 0.3 - 0.1])
            cylinder(r1 = 3, r2 = 0.5, h = h * 0.7, $fn = 24);
        translate([hw - 3, total_l, 0])
            cylinder(r = 3, h = h * 0.3, $fn = 24);
        translate([hw - 3, total_l, h * 0.3 - 0.1])
            cylinder(r1 = 3, r2 = 0.5, h = h * 0.7, $fn = 24);
    }
}

// ── Deckel ────────────────────────────────────────────────────────────────────
module lid() {
    difference() {
        union() {
            // Obere Platte (aerodynamisches Profil)
            fairing_shell(lid_h);

            // Zunge — greift in Gehäuse, hält Deckel in Position
            translate([wall + tongue_t, fairing + wall + tongue_t, -tongue_h])
                cube([inner_w - tongue_t * 2,
                      inner_l - tongue_t * 2,
                      tongue_h]);
        }

        // Nur Display-Fenster ausschneiden
        translate([disp_ox, disp_oy, -0.1])
            rounded_box(disp_w, disp_h, lid_h + 0.2, 2);

        // M2-Durchgangslöcher zu den Lid-Bossen im Gehäuse
        for (by = [lid_y1, lid_y2])
            for (bx = [wall / 2, outer_w - wall / 2])
                translate([bx, by, -tongue_h - 0.1])
                    cylinder(d = lid_m2_d, h = lid_h + tongue_h + 0.2, $fn = 16);
    }
}

// ── Ausgabe ───────────────────────────────────────────────────────────────────
color("SteelBlue", 0.85) lid();

echo(str("Deckel: ", round(outer_w*10)/10, " × ",
         round(total_l*10)/10, " × ",
         round((lid_h + tongue_h)*10)/10, " mm"));
