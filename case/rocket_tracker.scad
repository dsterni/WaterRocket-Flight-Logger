// Wasserraketen-Tracker — Gehäuse + Deckel
// Waveshare ESP32-S3-Touch-AMOLED-1.64" · BMP388 · MPU6050 · LiPo 500mAh
// Layout: SENSOREN (Spitze) → AKKU → PCB (Heck, USB-C raus)
// Drucken: 0.15mm Schichthöhe, 3 Perimeter, 15% Gyroid-Infill, PLA/PETG

// ── Parameter ────────────────────────────────────────────────────────────────
wall      = 1.4;
tol       = 0.3;
bottle_r  = 45.0;

board_w   = 28.6;
board_l   = 43.5;
board_h   = 9.6;
sensor_l  = 34.0;   // BMP388 16.5mm + 3mm + MPU6050 14.5mm (je -1.5mm)
gap       = 2.0;
bat_w     = 30.0;
bat_l     = 44.0;
bat_h     = 8.0;

disp_w    = 30.0;   // Glas 29mm + 0.5mm Toleranz je Seite
disp_h    = 45.0;   // Glas 44mm + 0.5mm Toleranz je Seite
disp_off_y = 2.3;   // von der USB-C-Kante des PCB

inner_w = max(board_w, bat_w) + tol * 2 + 2;                      // +2mm breiter → 32.6 mm
pcb_x0  = wall + (inner_w - board_w) / 2;                         // PCB zentriert → 3.4mm
inner_l = sensor_l + gap + bat_l + gap + board_l + tol * 2;       // 129.1 mm
inner_h = board_h + 3.0;                                           // 12.6 mm

outer_w = inner_w + wall * 2;
outer_l = inner_l + wall * 2;
outer_h = inner_h + wall;

fairing_nose = 14.0;   // spitze Nase
fairing = fairing_nose;
total_l = outer_l + fairing_nose;   // Heck = flacher Abschluss

// ── Zonen-Positionen (von Spitze aus gemessen) ───────────────────────────────
// Layout: [fairing_nose][wall][tol] SENSOR | gap | AKKU | gap | PCB [tol][wall][fairing_tail]
sen_y0 = fairing_nose + wall + tol;                     // 15.7 mm
bat_y0 = sen_y0 + sensor_l + gap;                      // 48.7 mm
brd_y0 = bat_y0 + bat_l + gap;                         // 94.7 mm  ← PCB-Vorderkante (Akku-Seite)
// PCB mit USB-C zum Heck: USB-C bei brd_y0 + board_l = 138.2 mm

// Display: PCB-USB-C zeigt zum Heck, Display-Offset von USB-C-Kante aus
disp_ox = pcb_x0 + (board_w - disp_w) / 2;            // 2.7mm — Display über PCB zentriert
disp_oy = brd_y0 + board_l - disp_off_y - disp_h;

lid_h    = wall;
tongue_h = 2.5;
tongue_t = 0.1;

// Deckel-Bossen: 1× Spitze (zentriert) + 2× Heck-Ecken
nose_boss_r = 3.0;  nose_boss_d = 2.4;
tail_boss_r = 2.5;  tail_boss_d = 2.0;
nose_boss_y  = sen_y0 + 4;                             // 19.7mm — in Sensor-Zone nahe Spitze
tail_boss_y  = total_l - 3;                            // 139.9mm — hinter Display-Ende
tail_boss_lx = wall + tail_boss_r + 0.5;               // 4.4mm — linke Heck-Ecke
tail_boss_rx = outer_w - wall - tail_boss_r - 0.5;     // 31.0mm — rechte Heck-Ecke

// ── Hilfsfunktionen ──────────────────────────────────────────────────────────
module rounded_box(w, l, h, r = 2) {
    hull()
        for (x = [r, w - r], y = [r, l - r])
            translate([x, y, 0]) cylinder(r = r, h = h, $fn = 24);
}

module bottle_cutout() {
    translate([outer_w / 2, total_l / 2, -bottle_r + 0.6])
        rotate([90, 0, 0])
        cylinder(r = bottle_r, h = total_l + 2, center = true, $fn = 80);
}

module fairing_shell(h) {
    hw = outer_w / 2;
    hull() {
        translate([0, fairing_nose, 0])
            rounded_box(outer_w, outer_l, h, 3);
        // Spitze Nase
        translate([hw - 3, 0, 0])
            cylinder(r = 3, h = h * 0.25, $fn = 24);
        translate([hw - 3, 0, h * 0.25 - 0.1])
            cylinder(r1 = 3, r2 = 0.3, h = h * 0.75, $fn = 24);
    }
}

// ── Gehäuse ──────────────────────────────────────────────────────────────────
module case_body() {
    difference() {
        fairing_shell(outer_h);

        // Innenraum
        translate([wall, fairing + wall, wall])
            cube([inner_w, inner_l, outer_h]);

        // Display-Fenster (PCB am Heck, Display nahe Heck)
        translate([disp_ox, disp_oy, outer_h - wall - 0.1])
            rounded_box(disp_w, disp_h, wall + 0.2, 2);

        // USB-C Schlitz durch Heckwand
        pcb_cx = pcb_x0 + board_w / 2;
        translate([pcb_cx - 5, total_l - wall - 1, wall + 2.5])
            cube([10.0, wall + 2, 5.0]);

        // Kabelbinder-Schlitze (3 Paare: Sensor-, Akku-, PCB-Zone)
        ziptie_w = 5.0;  ziptie_h = 3.5;
        for (zy = [sen_y0 + sensor_l * 0.5,
                   bat_y0 + bat_l * 0.5,
                   brd_y0 + board_l * 0.5]) {
            translate([-1, zy - ziptie_w / 2, wall])
                cube([wall + 2, ziptie_w, ziptie_h]);
            translate([outer_w - wall - 1, zy - ziptie_w / 2, wall])
                cube([wall + 2, ziptie_w, ziptie_h]);
        }

        // Druckausgleich BMP388 (Boden Sensor-Zone, nahe Spitze)
        translate([wall + tol + 9, sen_y0 + 6.5, -1])
            cylinder(d = 2.5, h = wall + 2, $fn = 20);

        // Batterie-Fach
        translate([wall + tol, bat_y0, wall])
            cube([bat_w + tol * 2, bat_l + tol * 2, bat_h + 1.0]);

        bottle_cutout();
    }

    // PCB M2-Bossen (4×) — PCB in der Heck-Zone
    m2_boss_od = 4.6;  m2_boss_h = 3.0;
    m2_drill = 2.2;    m2_head_d = 4.5;  m2_head_h = 1.2;
    for (px = [pcb_x0 + 2.5, pcb_x0 + board_w - 2.5])
        for (py = [brd_y0 + 2.5, brd_y0 + board_l - 2.5])
            difference() {
                translate([px, py, wall])
                    cylinder(d = m2_boss_od, h = m2_boss_h, $fn = 20);
                translate([px, py, -0.1])
                    cylinder(d = m2_drill, h = wall + m2_boss_h + 0.2, $fn = 16);
                translate([px, py, -0.1])
                    cylinder(d = m2_head_d, h = m2_head_h, $fn = 20);
            }

    // Deckel-Bosse: Spitze (1×) + Heck-Ecken (2×)
    difference() {
        translate([outer_w / 2, nose_boss_y, wall])
            cylinder(r = nose_boss_r, h = outer_h - wall, $fn = 20);
        translate([outer_w / 2, nose_boss_y, outer_h - 5.1])
            cylinder(d = nose_boss_d, h = 5.2, $fn = 16);
    }
    for (bx = [tail_boss_lx, tail_boss_rx])
        difference() {
            translate([bx, tail_boss_y, wall])
                cylinder(r = tail_boss_r, h = outer_h - wall, $fn = 20);
            translate([bx, tail_boss_y, outer_h - 4.1])
                cylinder(d = tail_boss_d, h = 4.2, $fn = 16);
        }

    // Sensor-Trennwände (3×) mit Kabeldurchführungen
    // Wand 1: BMP388 | MPU6050 (innerhalb Sensor-Zone)
    // Wand 2: Sensor-Zone | Akku
    // Wand 3: Akku | PCB
    bmp_l = 16.5;
    mpu_l = 14.5;
    div_t = 1.4;
    div_h = inner_h * 0.75;
    cbl_w = 7.0;
    cbl_h = 5.0;

    for (dy = [
        sen_y0 + bmp_l + (sensor_l - bmp_l - mpu_l) / 2 - div_t / 2,
        sen_y0 + sensor_l + gap / 2 - div_t / 2,
        bat_y0 + bat_l + gap / 2 - div_t / 2
    ]) {
        difference() {
            translate([wall + 1, dy, wall])
                cube([inner_w - 2, div_t, div_h]);
            // Offener Schlitz von oben: Kabel einlegen ohne durchfädeln
            translate([wall + (inner_w - cbl_w) / 2, dy - 0.1, wall + div_h - cbl_h])
                cube([cbl_w, div_t + 0.2, cbl_h + 0.1]);
        }
    }
}

// ── Deckel ────────────────────────────────────────────────────────────────────
module lid_body() {
    difference() {
        union() {
            fairing_shell(lid_h);
            translate([wall + tongue_t, fairing + wall + tongue_t, -tongue_h])
                cube([inner_w - tongue_t * 2, inner_l - tongue_t * 2, tongue_h]);
        }
        // Display-Fenster
        translate([disp_ox, disp_oy, -0.1])
            rounded_box(disp_w, disp_h, lid_h + 0.2, 2);
        // Deckel-Durchgangslöcher: Spitze + Heck-Ecken
        translate([outer_w / 2, nose_boss_y, -tongue_h - 0.1])
            cylinder(d = nose_boss_d, h = lid_h + tongue_h + 0.2, $fn = 16);
        translate([outer_w / 2, nose_boss_y, -tongue_h - 0.1])
            cylinder(r = nose_boss_r + 0.2, h = tongue_h + 0.2, $fn = 20);
        for (bx = [tail_boss_lx, tail_boss_rx]) {
            translate([bx, tail_boss_y, -tongue_h - 0.1])
                cylinder(d = tail_boss_d, h = lid_h + tongue_h + 0.2, $fn = 16);
            translate([bx, tail_boss_y, -tongue_h - 0.1])
                cylinder(r = tail_boss_r + 0.2, h = tongue_h + 0.2, $fn = 20);
        }
    }
}

// ── Ausgabe: Gehäuse + Deckel nebeneinander für Druck ────────────────────────
color("DodgerBlue", 0.9)
    case_body();

color("SteelBlue", 0.85)
    translate([outer_w + 8, total_l, lid_h])
        rotate([180, 0, 0])
            lid_body();

echo(str("Gehäuse: ", round(outer_w*10)/10, " × ", round(total_l*10)/10,
         " × ", round(outer_h*10)/10, " mm"));
echo(str("Deckel:  ", round(outer_w*10)/10, " × ", round(total_l*10)/10,
         " × ", round((lid_h+tongue_h)*10)/10, " mm"));
echo(str("Druckbett: ", round((outer_w*2+8)*10)/10, " × ",
         round(total_l*10)/10, " mm"));
echo(str("Zonen: Sensor y=", round(sen_y0*10)/10, " Akku y=",
         round(bat_y0*10)/10, " PCB y=", round(brd_y0*10)/10));
