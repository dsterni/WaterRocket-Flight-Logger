#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <ESPAsyncWebServer.h>
#include <Adafruit_BMP3XX.h>
#include <Adafruit_MPU6050.h>
#include "display_config.h"
#include "web_page.h"
#include "esp_log.h"
#include "esp_sleep.h"

static constexpr int I2C_SDA = 47;
static constexpr int I2C_SCL = 48;

// SSID wird pro Board eindeutig gemacht (MAC-Suffix), damit beide Tracker
// gleichzeitig ihr eigenes WLAN aufspannen — selbe Firmware auf beiden Boards.
static char AP_SSID[24] = "WaterRocket";
static const char* AP_PASS = "launch123";

static LGFX_WaveshareAMOLED164 display;
static LGFX_Sprite canvas(&display);

static Adafruit_BMP3XX bmp;
static Adafruit_MPU6050 mpu;

static AsyncWebServer server(80);
static AsyncEventSource events("/events");
static DNSServer dnsServer;
static int armSession = 0;   // zählt bei jedem Scharfstellen hoch → Browser lädt neu
static unsigned long lastSSE = 0;

// --- Flight state ---
enum FlightPhase { IDLE, ARMED, BOOST, COAST, PARACHUTE, LANDED };
static const char* PHASE_NAMES[] = { "BEREIT", "SCHARF", "SCHUB", "STEIG", "FALLSCH", "LANDET" };

struct FlightData {
    FlightPhase phase = IDLE;
    float groundAltitude = 0.0f;

    float altitude    = 0.0f;
    float maxAltitude = 0.0f;
    float velocity    = 0.0f;
    float maxSpeed    = 0.0f;
    float speed10m    = 0.0f;   // Durchschnittsgeschw. über die ersten 10 Höhenmeter (m/s)
    float accelZ      = 0.0f;
    float maxAccel    = 0.0f;

    unsigned long launchTime   = 0;
    unsigned long landedTime   = 0;
    unsigned long lastUpdateUs = 0;

    int descendCount = 0;
    float prevAltitude = 0.0f;
    int stableCount = 0;

    // Parachute detection
    float descentRate = 0.0f;
    bool chuteDetected = false;

    // Extra stats
    unsigned long apogeeTime = 0;
    float descentAltSum = 0.0f;
    int descentSamples = 0;
    float avgDescentSpeed = 0.0f;

    // Rotation tracking (gyroscope)
    float rollAngle  = 0.0f;    // accumulated degrees around rocket axis (Z)
    float pitchAngle = 0.0f;    // accumulated degrees around X
    float yawAngle   = 0.0f;    // accumulated degrees around Y
    int rollCount    = 0;        // full 360° spins around rocket axis
    int tumbleCount  = 0;        // full 360° pitch/yaw tumbles
    float maxRollRate  = 0.0f;   // peak spin rate (deg/s)
    float maxTumbleRate = 0.0f;  // peak tumble rate (deg/s)
    float gyroDeviationSum = 0.0f;  // sum of off-axis angular velocity
    int gyroSamples = 0;
    float stabilityScore = 100.0f;  // 100 = perfectly stable, 0 = chaotic
    float instantStability = 100.0f;

    // Raw sensor readings (updated every cycle)
    float rawAltitude  = 0.0f;   // absolute altitude above sea level (m)
    float rawPressure  = 0.0f;   // hPa
    float rawTemp      = 0.0f;   // °C
};

static FlightData flight;

// --- Colors ---
static constexpr uint16_t COL_BG      = 0x0000;
static constexpr uint16_t COL_TEXT    = 0xFFFF;
static constexpr uint16_t COL_DIM     = 0x8410;
static constexpr uint16_t COL_GREEN   = 0x07E0;
static constexpr uint16_t COL_RED     = 0xF800;
static constexpr uint16_t COL_YELLOW  = 0xFFE0;
static constexpr uint16_t COL_CYAN    = 0x07FF;
static constexpr uint16_t COL_ORANGE  = 0xFD20;
static constexpr uint16_t COL_MAGENTA = 0xF81F;

// --- Battery: Systemspannung über ADC1_CH3 = GPIO 4 (laut Waveshare-Wiki, Demo 01_ADC_Test).
// Dieses Board hat KEIN Power-Management-IC über I2C — die frühere 0x6A/0x6B-Abfrage war falsch.
static constexpr int BAT_ADC_PIN = 4;
static constexpr float BAT_DIVIDER = 3.0f;   // 200k/100k Teiler — ggf. per BAT_CAL nachjustieren

static int batteryPct = -1;   // -1 = unknown
static unsigned long lastBatRead = 0;
static int   batRawLast  = 0;
static float batVoltLast = 0;

static void updateBattery() {
    // Mittelwert über mehrere Messungen glättet ADC-Rauschen
    uint32_t mvSum = 0;
    int raw = 0;
    for (int i = 0; i < 8; i++) {
        mvSum += analogReadMilliVolts(BAT_ADC_PIN);   // werkskalibriert
        raw   += analogRead(BAT_ADC_PIN);
    }
    batRawLast = raw / 8;
    float v = (mvSum / 8.0f) / 1000.0f * BAT_DIVIDER;
    batVoltLast = v;

    // Unter 2,5 V ist kein Akku angeschlossen / Messung unplausibel
    if (v < 2.5f) { batteryPct = -1; return; }

    batteryPct = (int)((v - 3.3f) / (4.2f - 3.3f) * 100.0f);   // LiPo-Kennlinie grob
    if (batteryPct < 0)   batteryPct = 0;
    if (batteryPct > 100) batteryPct = 100;
}

// Small battery icon at (x,y), width 22px, height 12px
static void drawBatteryIcon(int x, int y, int pct) {
    uint16_t col = pct > 50 ? COL_GREEN : (pct > 20 ? COL_YELLOW : COL_RED);
    canvas.drawRect(x, y, 20, 12, COL_DIM);        // body outline
    canvas.fillRect(x + 20, y + 3, 3, 6, COL_DIM); // positive terminal nub
    int fill = (int)(18 * pct / 100.0f);
    if (fill > 0) canvas.fillRect(x + 1, y + 1, fill, 10, col);
}

// --- Persistent records (saved to flash) ---
static Preferences prefs;
static constexpr int MAX_LEADERBOARD = 20;

struct FlightRecord {
    float maxAlt;
    float maxSpeed;
    float maxG;
    float stability;
    float flightTime;
    int flightNum;
};

static FlightRecord leaderboard[MAX_LEADERBOARD];
static int leaderboardCount = 0;
static int totalFlights = 0;
static float bestAltEver = 0;
static float bestSpeedEver = 0;
static int bestFlightNum = 0;

static void loadRecords() {
    // readOnly=false creates namespace on first boot; readOnly=true fails if missing
    prefs.begin("rocket", false);
    totalFlights = prefs.getInt("flights", 0);
    bestAltEver = prefs.getFloat("bestAlt", 0);
    bestSpeedEver = prefs.getFloat("bestSpd", 0);
    bestFlightNum = prefs.getInt("bestNum", 0);
    leaderboardCount = prefs.getInt("lbCount", 0);
    if (leaderboardCount > MAX_LEADERBOARD) leaderboardCount = MAX_LEADERBOARD;
    for (int i = 0; i < leaderboardCount; i++) {
        char key[12];
        snprintf(key, sizeof(key), "lb%d_alt", i); leaderboard[i].maxAlt = prefs.getFloat(key, 0);
        snprintf(key, sizeof(key), "lb%d_spd", i); leaderboard[i].maxSpeed = prefs.getFloat(key, 0);
        snprintf(key, sizeof(key), "lb%d_g", i);   leaderboard[i].maxG = prefs.getFloat(key, 0);
        snprintf(key, sizeof(key), "lb%d_stb", i); leaderboard[i].stability = prefs.getFloat(key, 0);
        snprintf(key, sizeof(key), "lb%d_t", i);   leaderboard[i].flightTime = prefs.getFloat(key, 0);
        snprintf(key, sizeof(key), "lb%d_n", i);   leaderboard[i].flightNum = prefs.getInt(key, 0);
    }
    prefs.end();
}

static void saveRecords() {
    prefs.begin("rocket", false);
    prefs.putInt("flights", totalFlights);
    prefs.putFloat("bestAlt", bestAltEver);
    prefs.putFloat("bestSpd", bestSpeedEver);
    prefs.putInt("bestNum", bestFlightNum);
    prefs.putInt("lbCount", leaderboardCount);
    for (int i = 0; i < leaderboardCount; i++) {
        char key[12];
        snprintf(key, sizeof(key), "lb%d_alt", i); prefs.putFloat(key, leaderboard[i].maxAlt);
        snprintf(key, sizeof(key), "lb%d_spd", i); prefs.putFloat(key, leaderboard[i].maxSpeed);
        snprintf(key, sizeof(key), "lb%d_g", i);   prefs.putFloat(key, leaderboard[i].maxG);
        snprintf(key, sizeof(key), "lb%d_stb", i); prefs.putFloat(key, leaderboard[i].stability);
        snprintf(key, sizeof(key), "lb%d_t", i);   prefs.putFloat(key, leaderboard[i].flightTime);
        snprintf(key, sizeof(key), "lb%d_n", i);   prefs.putInt(key, leaderboard[i].flightNum);
    }
    prefs.end();
}

static void resetAllRecords() {
    totalFlights = 0;
    bestAltEver = 0;
    bestSpeedEver = 0;
    bestFlightNum = 0;
    leaderboardCount = 0;
    prefs.begin("rocket", false);
    prefs.clear();
    prefs.end();
}

static void addToLeaderboard(const FlightRecord& rec) {
    // Prepend: most recent flight at index 0
    int last = min(leaderboardCount, MAX_LEADERBOARD - 1);
    for (int i = last; i > 0; i--) leaderboard[i] = leaderboard[i - 1];
    leaderboard[0] = rec;
    if (leaderboardCount < MAX_LEADERBOARD) leaderboardCount++;
}

// --- Flight rating ---
static const char* getFlightRating(float alt, float stab) {
    float score = alt * 0.6f + stab * 0.4f;
    if (score > 60) return "LEGENDAER!";
    if (score > 45) return "HAMMER!";
    if (score > 30) return "SUPER!";
    if (score > 15) return "GUT!";
    if (score > 8)  return "NOCHMAL!";
    return "WEITER SO!";
}

static uint16_t getRatingColor(float alt, float stab) {
    float score = alt * 0.6f + stab * 0.4f;
    if (score > 45) return COL_GREEN;
    if (score > 25) return COL_YELLOW;
    return COL_ORANGE;
}

// --- Debug screen ---
static bool debugMode = false;
static bool bmpOk   = false;
static bool mpuOk   = false;
static bool touchOk = false;

// --- Countdown state ---
static bool countdownActive = false;
static int currentPage = 0;
static constexpr int NUM_PAGES = 5;
static int historyIndex = 0;       // which flight shown on history detail
static bool historyShowDetail = false;  // false = overview, true = single-flight detail
static float resetHoldProgress = 0.0f; // 0..1 for long-press reset feedback
static float powerOffProgress = 0.0f;  // 0..1 for long-press power-off feedback
static unsigned long countdownStart = 0;
static constexpr int COUNTDOWN_SECS = 5;

// --- Altitude + time history for graph ---
static constexpr int GRAPH_POINTS = 1000;
struct Sample {
    float altitude;
    float timeSec;
    float stability;  // 0-100 instant stability at this sample
};
static Sample samples[GRAPH_POINTS];
static int sampleCount = 0;

static void recordSample(float instantStab, int decimation = 1) {
    static int skipCounter = 0;
    if (++skipCounter < decimation) return;
    skipCounter = 0;
    if (sampleCount < GRAPH_POINTS) {
        samples[sampleCount].altitude = flight.altitude;
        samples[sampleCount].timeSec = (millis() - flight.launchTime) / 1000.0f;
        samples[sampleCount].stability = instantStab;
        sampleCount++;
    }
}

// --- Button areas ---
struct Button { int x, y, w, h; const char* label; uint16_t color; };

static Button btnArm   = { 30, 400, 100, 44, "START", COL_YELLOW };
static Button btnReset = { 150, 400, 100, 44, "NEU", COL_RED };

static bool touchInButton(int tx, int ty, const Button& b) {
    return tx >= b.x && tx < b.x + b.w && ty >= b.y && ty < b.y + b.h;
}

static void calibrateGround() {
    if (!bmpOk) return;

    for (int i = 0; i < 3; i++) { bmp.performReading(); delay(50); }

    float sum = 0;
    int good = 0;
    for (int i = 0; i < 10; i++) {
        if (bmp.performReading()) {
            float alt = bmp.readAltitude(1013.25f);
            if (alt > -500.0f && alt < 9000.0f) { sum += alt; good++; }
        }
        delay(50);
    }

    if (good >= 3) {
        flight.groundAltitude = sum / good;
    }
    // If fewer than 5 good readings, leave groundAltitude at 0 rather than use garbage
}

static void readSensors(float& alt, float& ax, float& ay, float& az,
                        float& gx, float& gy, float& gz) {
    // Nur übernehmen, wenn der Sensor da ist UND die Messung plausibel ist.
    // Sonst liefert readAltitude() bei Druck=0 die Formelkonstante 44330 m.
    if (bmpOk && bmp.performReading()) {
        float p = bmp.pressure / 100.0f;
        if (p > 300.0f && p < 1100.0f) {          // plausibler Luftdruckbereich
            flight.rawPressure = p;
            flight.rawTemp     = bmp.temperature;
            flight.rawAltitude = bmp.readAltitude(1013.25f);
        }
        // unplausibel → letzte gültige Werte behalten
    }
    float rawRelAlt = flight.rawAltitude - flight.groundAltitude;

    // Median-of-3 entfernt einzelne Staudruck-/Böen-Spitzen ohne nennenswerte Verzögerung
    static float m0 = 0, m1 = 0, m2 = 0;
    m0 = m1; m1 = m2; m2 = rawRelAlt;
    float med = fmaxf(fminf(m0, m1), fminf(fmaxf(m0, m1), m2));

    // Leichter Tiefpass darüber glättet Restrauschen (α=0.5 ≈ minimale Latenz)
    static float altFilt = 0.0f;
    altFilt = 0.5f * med + 0.5f * altFilt;
    alt = altFilt;

    sensors_event_t a, g, temp;
    mpu.getEvent(&a, &g, &temp);
    ax = a.acceleration.x;
    ay = a.acceleration.y;
    az = a.acceleration.z;
    // Gyro returns rad/s, convert to deg/s
    gx = g.gyro.x * 57.2958f;
    gy = g.gyro.y * 57.2958f;
    gz = g.gyro.z * 57.2958f;
}

static void updateFlight() {
    float alt, ax, ay, az, gx, gy, gz;
    readSensors(alt, ax, ay, az, gx, gy, gz);

    unsigned long nowUs = micros();
    float dt = (nowUs - flight.lastUpdateUs) / 1e6f;
    if (dt > 0.5f) dt = 0.02f;
    flight.lastUpdateUs = nowUs;

    flight.altitude = alt;
    flight.accelZ = ax - 9.81f;   // Ax = Raketenachse; Ruhewert ~0, Start >> 0

    // Low-pass filtered acceleration for max-G tracking
    // α=0.35: fast enough for 0.2s water-rocket boost, still damps single-sample noise
    static float accelFiltered = 0.0f;
    accelFiltered = 0.35f * flight.accelZ + 0.65f * accelFiltered;

    // Geschwindigkeit aus der (gut funktionierenden) Höhenmessung ableiten:
    // v = ΔHöhe / Δt. Zuverlässiger als die aufintegrierte Beschleunigung.
    // Positiv = Steigen. Kräftig geglättet, da die Ableitung Rauschen verstärkt.
    float rawClimb = (flight.altitude - flight.prevAltitude) / dt;
    if (rawClimb > 200.0f)  rawClimb = 200.0f;   // Ausreißer begrenzen (>720 km/h unrealistisch)
    if (rawClimb < -200.0f) rawClimb = -200.0f;
    flight.velocity = 0.7f * flight.velocity + 0.3f * rawClimb;

    // Zusätzlich stärker geglättete Spitzenwert-Basis (nur Steigen zählt für maxSpeed)
    static float velFiltered = 0.0f;
    velFiltered = 0.5f * fmaxf(flight.velocity, 0.0f) + 0.5f * velFiltered;

    // Descent rate (smoothed) — Sinkrate = negative Steiggeschwindigkeit
    flight.descentRate = 0.8f * flight.descentRate + 0.2f * ((flight.prevAltitude - flight.altitude) / dt);

    // Gyro tracking during flight (BOOST through PARACHUTE)
    // Rocket mounted vertically: Z = roll (spin), X/Y = pitch/yaw (tumble)
    float instantStability = 100.0f;
    if (flight.phase >= BOOST && flight.phase <= PARACHUTE) {
        flight.rollAngle  += gx * dt;   // Spin um Raketenachse (X)
        flight.pitchAngle += gy * dt;
        flight.yawAngle   += gz * dt;

        flight.rollCount  = (int)(fabsf(flight.rollAngle) / 360.0f);
        int pitchFull = (int)(fabsf(flight.pitchAngle) / 360.0f);
        int yawFull   = (int)(fabsf(flight.yawAngle) / 360.0f);
        flight.tumbleCount = pitchFull + yawFull;

        float rollRate   = fabsf(gx);
        float tumbleRate = sqrtf(gy * gy + gz * gz);
        if (rollRate > flight.maxRollRate) flight.maxRollRate = rollRate;
        if (tumbleRate > flight.maxTumbleRate) flight.maxTumbleRate = tumbleRate;

        // Instant stability for this sample (used for graph coloring)
        instantStability = fmaxf(0.0f, 100.0f - tumbleRate * 0.5f);
        flight.instantStability = instantStability;

        // Overall average stability
        flight.gyroDeviationSum += tumbleRate;
        flight.gyroSamples++;
        float avgTumble = flight.gyroDeviationSum / flight.gyroSamples;
        flight.stabilityScore = fmaxf(0.0f, 100.0f - (avgTumble * 0.5f));
    }

    switch (flight.phase) {
    case IDLE:
        break;

    case ARMED: {
        static int launchCount = 0;
        if (flight.accelZ > 2.5f) {    // ~0.25G net above gravity — sustained water-rocket thrust
            launchCount++;
        } else {
            launchCount = 0;
        }
        if (launchCount >= 2) {         // 2 consecutive samples (~40ms) avoids false triggers
            launchCount = 0;
            flight.phase = BOOST;
            flight.launchTime = millis();
            flight.groundAltitude = flight.rawAltitude;  // re-zero at launch
            currentPage = 1;  // switch to flight path screen
            flight.altitude = 0;
            flight.velocity = 0;
            flight.maxAltitude = 0;
            flight.maxSpeed = 0;
            flight.speed10m = 0;
            flight.maxAccel = 0;
            flight.apogeeTime = 0;
            flight.descentAltSum = 0;
            flight.descentSamples = 0;
            flight.avgDescentSpeed = 0;
            accelFiltered = 0.0f;
            velFiltered = 0.0f;
            flight.rollAngle = 0; flight.pitchAngle = 0; flight.yawAngle = 0;
            flight.rollCount = 0; flight.tumbleCount = 0;
            flight.maxRollRate = 0; flight.maxTumbleRate = 0;
            flight.gyroDeviationSum = 0; flight.gyroSamples = 0;
            flight.stabilityScore = 100;
            sampleCount = 0;
        }
        break;
    }

    case BOOST:
        // velocity kommt jetzt zentral aus der Höhenableitung (siehe oben)
        // Durchschnittsgeschw. der ersten 10 m: v = 10 m / Zeit bis zum Erreichen von 10 m
        if (flight.speed10m == 0.0f && flight.altitude >= 10.0f) {
            float t10 = (millis() - flight.launchTime) / 1000.0f;
            if (t10 > 0.01f) flight.speed10m = 10.0f / t10;
        }
        if (flight.altitude > flight.maxAltitude) { flight.maxAltitude = flight.altitude; flight.apogeeTime = millis(); }
        if (velFiltered > flight.maxSpeed) flight.maxSpeed = velFiltered;
        if (accelFiltered > flight.maxAccel) flight.maxAccel = accelFiltered;

        if (flight.accelZ < 0.5f && (millis() - flight.launchTime) > 100) {
            flight.phase = COAST;
        }
        break;

    case COAST:
        if (flight.speed10m == 0.0f && flight.altitude >= 10.0f) {
            float t10 = (millis() - flight.launchTime) / 1000.0f;
            if (t10 > 0.01f) flight.speed10m = 10.0f / t10;
        }
        if (flight.altitude > flight.maxAltitude) { flight.maxAltitude = flight.altitude; flight.apogeeTime = millis(); }
        // Only track max speed while still ascending — descent velocity would inflate the value
        if (flight.velocity > 0 && velFiltered > flight.maxSpeed) flight.maxSpeed = velFiltered;

        if (flight.altitude < flight.prevAltitude - 0.15f) {
            flight.descendCount++;
        } else {
            flight.descendCount = 0;
        }
        // Apogee reached → parachute deploys
        if (flight.descendCount > 5) {
            flight.phase = PARACHUTE;
            flight.chuteDetected = true;
        }

        // Direct landing from COAST (no parachute / hard impact)
        if ((millis() - flight.launchTime) > 1500UL) {
            float totalG  = sqrtf(ax*ax + ay*ay + az*az);
            bool restingG = (totalG > 8.5f && totalG < 11.0f);   // ~1G regardless of orientation
            bool notMoving = fabsf(flight.descentRate) < 0.6f;   // vertikal in Ruhe (kein Sinken)
            bool timeout  = (millis() - flight.launchTime) > 120000UL;
            // Kette darf einzelne Ausreißer verkraften: hoch bei Ruhe, langsam runter sonst
            if ((restingG && notMoving) || timeout) flight.stableCount++;
            else flight.stableCount -= 2;
            if (flight.stableCount < 0) flight.stableCount = 0;
        }
        if (flight.stableCount > 25) {   // ~0.5 s echte Ruhe bevor "gelandet"
            flight.phase = LANDED;
            flight.landedTime = millis();
            totalFlights++;
            float flightSec = (flight.landedTime - flight.launchTime) / 1000.0f;
            FlightRecord rec = { flight.maxAltitude, flight.maxSpeed,
                flight.maxAccel / 9.81f, flight.stabilityScore, flightSec, totalFlights };
            addToLeaderboard(rec);
            if (flight.maxAltitude > bestAltEver) { bestAltEver = flight.maxAltitude; bestFlightNum = totalFlights; }
            if (flight.maxSpeed > bestSpeedEver) bestSpeedEver = flight.maxSpeed;
            saveRecords();
        }
        break;

    case PARACHUTE:
        if (flight.descentRate > 0) {
            flight.descentAltSum += flight.descentRate;
            flight.descentSamples++;
            flight.avgDescentSpeed = flight.descentAltSum / flight.descentSamples;
        }

        if ((millis() - flight.launchTime) > 1500UL) {
            float totalG  = sqrtf(ax*ax + ay*ay + az*az);
            bool restingG = (totalG > 8.5f && totalG < 11.0f);
            bool notMoving = fabsf(flight.descentRate) < 0.6f;   // vertikal in Ruhe (kein Sinken)
            bool timeout  = (millis() - flight.launchTime) > 120000UL;

            // Kette darf einzelne Ausreißer verkraften: hoch bei Ruhe, langsam runter sonst
            if ((restingG && notMoving) || timeout) {
                flight.stableCount++;
            } else {
                flight.stableCount -= 2;
                if (flight.stableCount < 0) flight.stableCount = 0;
            }
        }
        if (flight.stableCount > 25) {   // ~0.5 s echte Ruhe bevor "gelandet"
            flight.phase = LANDED;
            flight.landedTime = millis();

            // Save flight to leaderboard
            totalFlights++;
            float flightSec = (flight.landedTime - flight.launchTime) / 1000.0f;
            FlightRecord rec = { flight.maxAltitude, flight.maxSpeed,
                flight.maxAccel / 9.81f, flight.stabilityScore, flightSec, totalFlights };
            addToLeaderboard(rec);
            if (flight.maxAltitude > bestAltEver) {
                bestAltEver = flight.maxAltitude;
                bestFlightNum = totalFlights;
            }
            if (flight.maxSpeed > bestSpeedEver) bestSpeedEver = flight.maxSpeed;
            saveRecords();
        }
        break;

    case LANDED:
        break;
    }

    // Record every cycle while rocket is in motion (BOOST=dense, PARACHUTE=decimated)
    if (flight.phase >= BOOST && flight.phase <= LANDED) {
        int dec = (flight.phase == PARACHUTE) ? 10 : 1;
        recordSample(instantStability, dec);
    }

    flight.prevAltitude = flight.altitude;
}

// --- Graph drawing (main feature) ---
static void drawGraph(int x, int y, int w, int h) {
    // Border
    canvas.drawRect(x, y, w, h, COL_DIM);

    // Label
    canvas.setTextColor(COL_DIM);
    canvas.setTextSize(1);
    canvas.setCursor(x + 4, y + 3);
    canvas.print("HOEHE (m)");

    if (sampleCount < 2) {
        canvas.setTextColor(COL_DIM);
        canvas.setTextSize(1);
        canvas.setCursor(x + w / 2 - 40, y + h / 2 - 4);
        canvas.print("Keine Daten");
        return;
    }

    // Find ranges
    float maxAlt = 1.0f;
    float maxTime = 1.0f;
    for (int i = 0; i < sampleCount; i++) {
        if (samples[i].altitude > maxAlt) maxAlt = samples[i].altitude;
        if (samples[i].timeSec > maxTime) maxTime = samples[i].timeSec;
    }

    // Add 10% headroom
    maxAlt *= 1.1f;
    maxTime *= 1.05f;

    int gx = x + 28;  // left margin for Y labels
    int gy = y + 14;   // top margin
    int gw = w - 32;
    int gh = h - 28;   // bottom margin for X labels

    // Grid lines + Y-axis labels
    for (int i = 0; i <= 4; i++) {
        int ly = gy + gh - (i * gh / 4);
        canvas.drawFastHLine(gx, ly, gw, canvas.color565(30, 30, 40));
        char buf[8];
        snprintf(buf, sizeof(buf), "%.0f", maxAlt * i / 4.0f);
        canvas.setTextColor(COL_DIM);
        canvas.setTextSize(1);
        canvas.setCursor(x + 2, ly - 3);
        canvas.print(buf);
    }

    // X-axis labels
    for (int i = 0; i <= 4; i++) {
        int lx = gx + (i * gw / 4);
        canvas.drawFastVLine(lx, gy, gh, canvas.color565(30, 30, 40));
        char buf[8];
        snprintf(buf, sizeof(buf), "%.1fs", maxTime * i / 4.0f);
        canvas.setTextColor(COL_DIM);
        canvas.setTextSize(1);
        canvas.setCursor(lx - 8, gy + gh + 3);
        canvas.print(buf);
    }

    // Find phase transition indices for coloring
    int prevPx = -1, prevPy = -1;
    for (int i = 0; i < sampleCount; i++) {
        int px = gx + (int)((samples[i].timeSec / maxTime) * gw);
        int py = gy + gh - (int)((samples[i].altitude / maxAlt) * gh);
        if (py < gy) py = gy;
        if (py > gy + gh) py = gy + gh;

        // Color by stability: green=stable, yellow=wobble, red=tumbling
        float s = samples[i].stability;
        uint16_t lineCol;
        if (s > 70.0f) {
            // Green to yellow gradient: interpolate
            int g2y = (int)((100.0f - s) / 30.0f * 255.0f);
            lineCol = canvas.color565(g2y, 255, 0);
        } else if (s > 40.0f) {
            int y2r = (int)((70.0f - s) / 30.0f * 255.0f);
            lineCol = canvas.color565(255, 255 - y2r, 0);
        } else {
            lineCol = COL_RED;
        }

        if (prevPx >= 0) {
            canvas.drawLine(prevPx, prevPy, px, py, lineCol);
            // Thicker line
            canvas.drawLine(prevPx, prevPy + 1, px, py + 1, lineCol);
        }
        prevPx = px;
        prevPy = py;
    }

    // Mark max altitude with a dot and label
    float peakTime = 0;
    for (int i = 0; i < sampleCount; i++) {
        if (fabsf(samples[i].altitude - flight.maxAltitude) < 0.1f) {
            peakTime = samples[i].timeSec;
            break;
        }
    }
    int peakPx = gx + (int)((peakTime / maxTime) * gw);
    int peakPy = gy + gh - (int)((flight.maxAltitude / maxAlt) * gh);
    canvas.fillCircle(peakPx, peakPy, 4, COL_RED);
    canvas.setTextColor(COL_RED);
    canvas.setTextSize(1);
    char peakBuf[16];
    snprintf(peakBuf, sizeof(peakBuf), "%.1fm", flight.maxAltitude);
    canvas.setCursor(peakPx + 6, peakPy - 4);
    canvas.print(peakBuf);

    // Parachute icon near descent start
    if (flight.chuteDetected) {
        for (int i = 1; i < sampleCount; i++) {
            if (samples[i].altitude < samples[i - 1].altitude - 0.1f) {
                int cx = gx + (int)((samples[i].timeSec / maxTime) * gw);
                int cy = gy + gh - (int)((samples[i].altitude / maxAlt) * gh);
                // Simple parachute symbol: arc + line
                canvas.drawCircle(cx, cy - 8, 6, COL_MAGENTA);
                canvas.drawLine(cx - 6, cy - 8, cx, cy, COL_MAGENTA);
                canvas.drawLine(cx + 6, cy - 8, cx, cy, COL_MAGENTA);
                break;
            }
        }
    }
}

// --- Compact value display ---
static void drawValueCompact(int x, int y, const char* label, float value, const char* unit, uint16_t color) {
    canvas.setTextColor(COL_DIM);
    canvas.setTextSize(1);
    canvas.setCursor(x, y);
    canvas.print(label);

    char buf[16];
    snprintf(buf, sizeof(buf), "%.1f", value);
    canvas.setTextColor(color);
    canvas.setTextSize(1);
    canvas.setCursor(x, y + 10);
    canvas.print(buf);
    canvas.print(" ");
    canvas.setTextColor(COL_DIM);
    canvas.print(unit);
}

static void drawButton(const Button& b, bool active) {
    uint16_t col = active ? b.color : COL_DIM;
    canvas.drawRoundRect(b.x, b.y, b.w, b.h, 8, col);
    canvas.setTextColor(col);
    canvas.setTextSize(2);
    int tw = strlen(b.label) * 12;
    canvas.setCursor(b.x + (b.w - tw) / 2, b.y + 14);
    canvas.print(b.label);
}

static void drawPhaseIndicator(int y) {
    uint16_t col;
    switch (flight.phase) {
        case IDLE:      col = COL_DIM;     break;
        case ARMED:     col = COL_YELLOW;  break;
        case BOOST:     col = COL_RED;     break;
        case COAST:     col = COL_ORANGE;  break;
        case PARACHUTE: col = COL_MAGENTA; break;
        case LANDED:    col = COL_GREEN;   break;
    }
    canvas.fillRoundRect(10, y, 260, 24, 6, col);
    canvas.setTextColor(COL_BG);
    canvas.setTextSize(2);
    int tw = strlen(PHASE_NAMES[flight.phase]) * 12;
    canvas.setCursor(140 - tw / 2, y + 4);
    canvas.print(PHASE_NAMES[flight.phase]);
}

static void drawCountdown() {
    int remaining = COUNTDOWN_SECS - (int)((millis() - countdownStart) / 1000);
    if (remaining <= 0) {
        countdownActive = false;
        calibrateGround();
        flight.phase = ARMED;
        return;
    }

    canvas.fillScreen(COL_BG);

    // Big countdown number
    canvas.setTextColor(remaining <= 2 ? COL_RED : COL_YELLOW);
    canvas.setTextSize(8);
    char num[4];
    snprintf(num, sizeof(num), "%d", remaining);
    int tw = strlen(num) * 48;
    canvas.setCursor(140 - tw / 2, 150);
    canvas.print(num);

    // Label
    canvas.setTextColor(COL_TEXT);
    canvas.setTextSize(2);
    canvas.setCursor(62, 260);
    canvas.print("ACHTUNG!");

    // Progress bar
    float progress = (float)(millis() - countdownStart) / (COUNTDOWN_SECS * 1000.0f);
    int barW = (int)(260 * progress);
    canvas.drawRect(10, 310, 260, 12, COL_DIM);
    canvas.fillRect(10, 310, barW, 12, COL_YELLOW);

    // Flight number
    canvas.setTextColor(COL_DIM);
    canvas.setTextSize(1);
    char flightBuf[32];
    snprintf(flightBuf, sizeof(flightBuf), "Flug #%d", totalFlights + 1);
    canvas.setCursor(100, 340);
    canvas.print(flightBuf);

    canvas.pushSprite(0, 0);
}

static void drawDebug() {
    canvas.fillScreen(TFT_BLACK);

    // Title bar
    canvas.fillRect(0, 0, 280, 22, canvas.color565(30, 30, 80));
    canvas.setTextColor(TFT_CYAN);
    canvas.setTextSize(1);
    canvas.setCursor(4, 7);
    canvas.print("DEBUG  [Tippen zum Beenden]");

    // Read live sensor values
    float alt = 0, temp = 0, pres = 0;
    float ax = 0, ay = 0, az = 0, gx = 0, gy = 0, gz = 0;
    if (bmpOk && bmp.performReading()) {
        pres = bmp.pressure / 100.0f;
        temp = bmp.temperature;
        alt  = bmp.readAltitude(1013.25f);
    }
    if (mpuOk) {
        sensors_event_t ea, eg, et;
        mpu.getEvent(&ea, &eg, &et);
        ax = ea.acceleration.x;
        ay = ea.acceleration.y;
        az = ea.acceleration.z;
        gx = eg.gyro.x * 57.2958f;
        gy = eg.gyro.y * 57.2958f;
        gz = eg.gyro.z * 57.2958f;
    }

    auto row = [&](int y, uint16_t col, const char* fmt, ...) {
        char buf[48];
        va_list args; va_start(args, fmt); vsnprintf(buf, sizeof(buf), fmt, args); va_end(args);
        canvas.setTextColor(col);
        canvas.setCursor(6, y);
        canvas.print(buf);
    };

    canvas.setTextSize(2);
    int y = 30;

    // BMP388
    canvas.setTextColor(bmpOk ? TFT_GREEN : TFT_RED);
    canvas.setCursor(6, y); canvas.print(bmpOk ? "BMP388  OK" : "BMP388  FEHLT"); y += 22;
    if (bmpOk) {
        row(y, TFT_WHITE,  "  Druck:  %7.2f hPa", pres);  y += 20;
        row(y, TFT_WHITE,  "  Temp:   %7.2f C",   temp);  y += 20;
        row(y, TFT_YELLOW, "  Hoehe:  %7.2f m",   alt);   y += 20;
    } else { y += 4; }

    y += 4;
    canvas.setTextColor(mpuOk ? TFT_GREEN : TFT_RED);
    canvas.setCursor(6, y); canvas.print(mpuOk ? "MPU6050 OK" : "MPU6050 FEHLT"); y += 22;
    if (mpuOk) {
        row(y, TFT_WHITE,  "  Ax:%6.2f Ay:%6.2f", ax, ay); y += 20;
        row(y, TFT_WHITE,  "  Az:%6.2f m/s2",     az);      y += 20;
        row(y, TFT_CYAN,   "  Gx:%6.1f deg/s",    gx);      y += 20;
        row(y, TFT_CYAN,   "  Gy:%6.1f Gz:%5.1f", gy, gz);  y += 20;
        float totalG = sqrtf(ax*ax + ay*ay + az*az) / 9.81f;
        row(y, TFT_YELLOW, "  |G|:  %5.2f G",     totalG);  y += 20;
    } else { y += 4; }

    y += 6;
    // WiFi + display status
    canvas.setTextSize(1);
    canvas.setTextColor(TFT_DARKGREY);
    canvas.setCursor(6, y);
    canvas.printf("WiFi: %s  IP: %s", AP_SSID, WiFi.softAPIP().toString().c_str()); y += 14;
    canvas.setCursor(6, y);
    canvas.printf("Clients: %d  Flüge: %d", WiFi.softAPgetStationNum(), totalFlights); y += 14;
    canvas.setCursor(6, y);
    canvas.printf("Uptime: %lus  Free heap: %uKB",
                  millis() / 1000, ESP.getFreeHeap() / 1024); y += 14;
    canvas.setCursor(6, y);
    if (batteryPct >= 0)
        canvas.printf("BAT GPIO%d raw=%d v=%.3fV %d%%",
                      BAT_ADC_PIN, batRawLast, batVoltLast, batteryPct);
    else
        canvas.printf("BAT: kein Akku (v=%.2fV, GPIO%d)", batVoltLast, BAT_ADC_PIN);

    canvas.pushSprite(0, 0);
}

// ── helpers for big readable values ────────────────────────────────────────
// Grid cell: label textSize 2, value textSize 4
static void maxCell(int x, int y, int w, const char* label,
                    const char* value, const char* unit, uint16_t col) {
    canvas.setTextColor(COL_DIM);
    canvas.setTextSize(2);
    canvas.setCursor(x + 4, y + 4);
    canvas.print(label);

    canvas.setTextColor(col);
    canvas.setTextSize(4);
    canvas.setCursor(x + 4, y + 26);
    canvas.print(value);

    canvas.setTextColor(COL_DIM);
    canvas.setTextSize(2);
    canvas.print(unit);
}

static void bigVal(int x, int y, const char* label, const char* value, const char* unit, uint16_t col) {
    canvas.setTextSize(1);
    canvas.setTextColor(COL_DIM);
    canvas.setCursor(x, y);
    canvas.print(label);
    canvas.setTextSize(3);
    canvas.setTextColor(col);
    canvas.setCursor(x, y + 12);
    canvas.print(value);
    canvas.setTextSize(2);
    canvas.setTextColor(COL_DIM);
    canvas.print(unit);
}

static void pageHeader(const char* title, int page, int pages) {
    // title bar
    canvas.fillRect(0, 0, 280, 28, canvas.color565(20, 20, 50));
    canvas.setTextColor(COL_TEXT);
    canvas.setTextSize(2);
    canvas.setCursor(10, 6);
    canvas.print(title);
    // phase name
    canvas.setTextSize(1);
    canvas.setTextColor(COL_YELLOW);
    canvas.setCursor(170, 10);
    canvas.print(PHASE_NAMES[flight.phase]);
    // battery icon (top-right)
    if (batteryPct >= 0)
        drawBatteryIcon(252, 8, batteryPct);
    else {
        canvas.setTextColor(COL_DIM);
        canvas.setCursor(254, 10);
        canvas.print("--");
    }
    // page dots
    int dotY = 447;
    for (int i = 0; i < pages; i++) {
        uint16_t c = (i == page) ? TFT_WHITE : canvas.color565(80, 80, 80);
        canvas.fillCircle(280/2 - (pages-1)*8 + i*16, dotY, 4, c);
    }
}

// ── Screen 0: Höhe ───────────────────────────────────────────────────────────
static void drawScreenAltitude() {
    char buf[20];
    pageHeader("HOEHE", 0, NUM_PAGES);

    // Full-width cell helper (label textSize 2, value textSize 6)
    auto wideCell = [&](int y, int h, const char* label,
                        const char* value, const char* unit, uint16_t col) {
        canvas.setTextColor(COL_DIM);
        canvas.setTextSize(2);
        canvas.setCursor(8, y + 4);
        canvas.print(label);
        canvas.setTextColor(col);
        canvas.setTextSize(6);
        canvas.setCursor(8, y + 26);
        canvas.print(value);
        canvas.setTextColor(COL_DIM);
        canvas.setTextSize(3);
        canvas.print(unit);
    };

    uint16_t divCol = canvas.color565(40, 40, 60);

    // Row 0: live altitude
    snprintf(buf, sizeof(buf), "%.1f", flight.altitude);
    wideCell(28, 140, "HOEHE  (live)", buf, "m", COL_CYAN);

    // small raw info
    canvas.setTextColor(COL_DIM); canvas.setTextSize(1);
    canvas.setCursor(8, 142);
    canvas.printf("%.1f hPa   %.1f C   abs %.0f m",
        flight.rawPressure, flight.rawTemp, flight.rawAltitude);

    canvas.drawFastHLine(0, 154, 280, divCol);

    // Row 1: max altitude
    snprintf(buf, sizeof(buf), "%.1f", flight.maxAltitude);
    wideCell(158, 140, "MAX HOEHE", buf, "m", COL_YELLOW);

    canvas.drawFastHLine(0, 306, 280, divCol);

    // Phase pill + dots
    drawPhaseIndicator(312);

    // page dots already in pageHeader — reuse
    if (flight.phase == IDLE || flight.phase == LANDED) {
        canvas.setTextColor(COL_DIM); canvas.setTextSize(1);
        canvas.setCursor(8, 330);
        canvas.printf("WiFi: %s", AP_SSID);
        canvas.setCursor(8, 344);
        canvas.printf("PW:   %s", AP_PASS);
        canvas.setCursor(8, 358);
        canvas.printf("http://192.168.4.1");
        if (bestAltEver > 0) {
            canvas.setTextColor(COL_CYAN);
            canvas.setCursor(8, 380);
            canvas.printf("Rekord: %.1fm  Flug #%d", bestAltEver, bestFlightNum);
        }
    }

    if (flight.phase == LANDED) {
        const char* rating = getFlightRating(flight.maxAltitude, flight.stabilityScore);
        uint16_t rc = getRatingColor(flight.maxAltitude, flight.stabilityScore);
        canvas.setTextColor(rc); canvas.setTextSize(3);
        int rw = strlen(rating) * 18;
        canvas.setCursor(140 - rw / 2, 390);
        canvas.print(rating);
        if (flight.maxAltitude >= bestAltEver) {
            canvas.setTextColor(COL_GREEN); canvas.setTextSize(1);
            canvas.setCursor(55, 424); canvas.print("** NEUER REKORD! **");
        }
        drawButton(btnReset, true);
    }

    if (flight.phase == IDLE) drawButton(btnArm, true);
}

// ── Screen 1: Beschleunigungssensor (Rohwerte) ──────────────────────────────
static void drawScreenAccel() {
    char bl[16], br[16];
    pageHeader("BESCHLEUNIGUNG", 1, NUM_PAGES);

    float ax = 0, ay = 0, az = 0;
    if (mpuOk) {
        sensors_event_t ea, eg, et;
        mpu.getEvent(&ea, &eg, &et);
        ax = ea.acceleration.x;
        ay = ea.acceleration.y;
        az = ea.acceleration.z;
    }
    float totalG = sqrtf(ax*ax + ay*ay + az*az) / 9.81f;

    // 2×2 grid — same geometry as page 4
    const int cx = 140;
    const int ry[2] = { 28, 242 };
    uint16_t div = canvas.color565(40, 40, 60);

    canvas.drawFastVLine(cx - 1, 28, 402, div);
    canvas.drawFastHLine(0, ry[1] - 2, 280, div);

    snprintf(bl, sizeof(bl), "%+.2f", ax);
    maxCell(0,  ry[0], cx, "Ax  (m/s2)", bl, "", COL_CYAN);

    snprintf(br, sizeof(br), "%+.2f", ay);
    maxCell(cx, ry[0], cx, "Ay  (m/s2)", br, "", COL_CYAN);

    snprintf(bl, sizeof(bl), "%+.2f", az);
    maxCell(0,  ry[1], cx, "Az  (m/s2)", bl, "", COL_CYAN);

    uint16_t gc = totalG > 3.0f ? COL_RED : totalG > 1.5f ? COL_YELLOW : COL_GREEN;
    snprintf(br, sizeof(br), "%.2f", totalG);
    maxCell(cx, ry[1], cx, "|G|  gesamt", br, "G", gc);
}

// ── Screen 2: Flugpfad-Graph ─────────────────────────────────────────────────
static void drawScreenGraph() {
    pageHeader("FLUGPFAD", 1, NUM_PAGES);
    drawGraph(2, 34, 276, 395);

    // Overlay: MAX HÖHE oben links, MAX SPEED oben rechts im Graph
    char buf[16];
    snprintf(buf, sizeof(buf), "%.1f", flight.maxAltitude);
    canvas.setTextColor(COL_DIM);  canvas.setTextSize(1); canvas.setCursor(8,  48); canvas.print("MAX");
    canvas.setTextColor(COL_CYAN); canvas.setTextSize(3); canvas.setCursor(8,  58); canvas.print(buf);
    canvas.setTextColor(COL_DIM);  canvas.setTextSize(2); canvas.print("m");

    snprintf(buf, sizeof(buf), "%.0f", flight.maxSpeed * 3.6f);
    int sw = strlen(buf) * 18 + 36;  // textSize3 width + "km/h"
    canvas.setTextColor(COL_DIM);    canvas.setTextSize(1); canvas.setCursor(278 - sw, 48); canvas.print("MAX");
    canvas.setTextColor(COL_YELLOW); canvas.setTextSize(3); canvas.setCursor(278 - sw, 58); canvas.print(buf);
    canvas.setTextColor(COL_DIM);    canvas.setTextSize(2); canvas.print("km/h");
}

// ── Screen 3: Sensor-Status ──────────────────────────────────────────────────
static void drawScreenControl() {
    pageHeader("SENSOREN", 3, NUM_PAGES);

    // ── BMP388 ──────────────────────────────────────────────────────────────
    uint16_t bmpCol = bmpOk ? COL_GREEN : COL_RED;
    canvas.fillRoundRect(8, 36, 264, 30, 6, bmpCol);
    canvas.setTextColor(COL_BG);
    canvas.setTextSize(2);
    canvas.setCursor(20, 44);
    canvas.print(bmpOk ? "BMP388   OK" : "BMP388   FEHLT");

    if (bmpOk) {
        canvas.setTextColor(COL_DIM); canvas.setTextSize(1);
        canvas.setCursor(14, 74);
        canvas.printf("Druck:  %.2f hPa", flight.rawPressure);
        canvas.setCursor(14, 86);
        canvas.printf("Temp:   %.1f C", flight.rawTemp);
        canvas.setCursor(14, 98);
        canvas.printf("Hoehe:  %.2f m (abs)", flight.rawAltitude);
    }

    canvas.drawFastHLine(0, 116, 280, canvas.color565(40,40,60));

    // ── MPU6050 ──────────────────────────────────────────────────────────────
    uint16_t mpuCol = mpuOk ? COL_GREEN : COL_RED;
    canvas.fillRoundRect(8, 124, 264, 30, 6, mpuCol);
    canvas.setTextColor(COL_BG);
    canvas.setTextSize(2);
    canvas.setCursor(20, 132);
    canvas.print(mpuOk ? "MPU6050  OK" : "MPU6050  FEHLT");

    if (mpuOk) {
        sensors_event_t ea, eg, et;
        mpu.getEvent(&ea, &eg, &et);
        float ax = ea.acceleration.x, ay = ea.acceleration.y, az = ea.acceleration.z;
        float gx = eg.gyro.x * 57.2958f, gy = eg.gyro.y * 57.2958f, gz = eg.gyro.z * 57.2958f;
        float totalG = sqrtf(ax*ax + ay*ay + az*az) / 9.81f;

        canvas.setTextColor(COL_DIM); canvas.setTextSize(1);
        canvas.setCursor(14, 162);
        canvas.printf("Ax:%.2f  Ay:%.2f  Az:%.2f", ax, ay, az);
        canvas.setCursor(14, 174);
        canvas.printf("Gx:%.1f  Gy:%.1f  Gz:%.1f", gx, gy, gz);
        canvas.setCursor(14, 186);
        canvas.printf("|G|: %.2f G", totalG);
    }

    canvas.drawFastHLine(0, 204, 280, canvas.color565(40,40,60));

    // ── WiFi ──────────────────────────────────────────────────────────────────
    bool wifiOk = WiFi.softAPgetStationNum() >= 0;
    canvas.fillRoundRect(8, 212, 264, 30, 6, COL_GREEN);
    canvas.setTextColor(COL_BG);
    canvas.setTextSize(2);
    canvas.setCursor(20, 220);
    canvas.print("WIFI     OK");

    canvas.setTextColor(COL_DIM); canvas.setTextSize(1);
    canvas.setCursor(14, 250);
    canvas.printf("%s  /  %s", AP_SSID, AP_PASS);
    canvas.setCursor(14, 262);
    canvas.printf("http://%s", WiFi.softAPIP().toString().c_str());
    canvas.setCursor(14, 274);
    canvas.printf("Clients: %d", WiFi.softAPgetStationNum());
}

// ── Screen 4: Max-Werte letzter Flug (2-Spalten) ────────────────────────────
static void drawScreenMaxValues() {
    char bl[16], br[16];
    pageHeader("MAX WERTE", 2, NUM_PAGES);

    // Grid: 2 columns × 3 rows
    // col0: x=0..138   col1: x=140..279
    // row0: y=28..170  row1: y=172..314  row2: y=316..430
    const int cx = 140;   // column divider x
    const int ry[3] = { 28, 172, 316 };
    const int rh = 142;
    const int cw = 138;
    uint16_t div = canvas.color565(40, 40, 60);

    // Dividers
    canvas.drawFastVLine(cx - 1, 28, 402, div);
    canvas.drawFastHLine(0, ry[1] - 2, 280, div);
    canvas.drawFastHLine(0, ry[2] - 2, 280, div);

    // Row 0: MAX HÖHE | MAX G-KRAFT
    snprintf(bl, sizeof(bl), "%.1f", flight.maxAltitude);
    maxCell(0,   ry[0], cw, "MAX HOEHE",  bl, "m", COL_CYAN);

    snprintf(br, sizeof(br), "%.2f", flight.maxAccel / 9.81f);
    maxCell(cx,  ry[0], cw, "MAX G",      br, "G", COL_ORANGE);

    // Row 1: TEMPO 10m | MAX SPEED
    snprintf(bl, sizeof(bl), "%.0f", flight.speed10m * 3.6f);
    maxCell(0,   ry[1], cw, "TEMPO 10m",  bl, "km/h", COL_MAGENTA);

    snprintf(br, sizeof(br), "%.0f", flight.maxSpeed * 3.6f);
    maxCell(cx,  ry[1], cw, "MAX SPEED",  br, "km/h", COL_YELLOW);

    // Row 2: STABILITÄT | FLUGZEIT
    uint16_t sc = flight.stabilityScore > 70 ? COL_GREEN :
                  flight.stabilityScore > 40 ? COL_YELLOW : COL_RED;
    snprintf(bl, sizeof(bl), "%.0f", flight.stabilityScore);
    maxCell(0,   ry[2], cw, "STABILITAET", bl, "%", sc);

    float flightSec = 0;
    if (flight.launchTime > 0)
        flightSec = (flight.phase == LANDED)
            ? (flight.landedTime - flight.launchTime) / 1000.0f
            : (millis() - flight.launchTime) / 1000.0f;
    snprintf(br, sizeof(br), "%.1f", flightSec);
    maxCell(cx,  ry[2], cw, "FLUGZEIT",   br, "s", COL_TEXT);
}

// Standby = echter Deep Sleep mit Timer-Wake. Das Board wacht alle STANDBY_POLL_US
// kurz auf, prüft den Touch und schläft weiter → Mittelwert wenige mA statt ~40 mA.
// Kein echtes Power-Off: dieses Board hat kein Power-Management-IC (siehe Waveshare-Wiki).
// Zum vollständigen Stromlosmachen den Akku abstecken.
static constexpr uint64_t STANDBY_POLL_US = 3000000ULL;   // 3 s zwischen den Touch-Checks

RTC_DATA_ATTR bool rtcStandby   = false;  // überlebt Deep Sleep
RTC_DATA_ATTR int  rtcSkipWakes = 0;      // erste Wakes ignorieren (Finger noch am Display)

static void powerOff() {
    saveRecords();                  // Flüge sichern
    Serial.println("Standby (Deep Sleep) — Display antippen zum Aufwecken");
    Serial.flush();

    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);

    display.fillScreen(TFT_BLACK);
    display.setBrightness(0);
    display.sleep();                // Panel in den Schlafmodus

    rtcStandby   = true;
    rtcSkipWakes = 1;               // ersten Wake (~3 s) ignorieren, bis der Finger weg ist
    esp_sleep_enable_timer_wakeup(STANDBY_POLL_US);
    esp_deep_sleep_start();
}

// ── Screen 4: Scharf stellen ─────────────────────────────────────────────────
static void drawScreenRearm() {
    pageHeader("SCHARF STELLEN", 3, NUM_PAGES);

    canvas.setTextColor(COL_DIM); canvas.setTextSize(2);
    canvas.setCursor(10, 36);
    canvas.printf("Boden:   %.1f m", flight.groundAltitude);
    canvas.setCursor(10, 58);
    canvas.printf("Aktuell: %.1f m", flight.rawAltitude);
    canvas.setCursor(10, 80);
    canvas.printf("Druck:   %.1f hPa", flight.rawPressure);

    canvas.drawFastHLine(0, 104, 280, canvas.color565(40,40,60));

    // Toggle-Button: BEREIT (grün) ↔ SCHARF (rot)
    bool isArmed = (flight.phase == ARMED);
    uint16_t btnFill = isArmed ? COL_RED : COL_GREEN;
    canvas.fillRoundRect(16, 116, 248, 100, 16, btnFill);
    canvas.setTextColor(COL_BG); canvas.setTextSize(5);
    const char* lbl = isArmed ? "SCHARF" : "BEREIT";
    int tw = strlen(lbl) * 30;
    canvas.setCursor(140 - tw / 2, 136);
    canvas.print(lbl);
    canvas.setTextSize(1);
    int hw = strlen(isArmed ? "Tippen: Entschaerfen" : "Tippen: Kalibrieren + Scharf") * 6;
    canvas.setCursor(140 - hw / 2, 200);
    canvas.print(isArmed ? "Tippen: Entschaerfen" : "Tippen: Kalibrieren + Scharf");

    canvas.drawFastHLine(0, 230, 280, canvas.color565(40,40,60));

    canvas.setTextColor(COL_DIM); canvas.setTextSize(1);
    canvas.setCursor(10, 240);
    canvas.printf("Fluege gesamt: %d", totalFlights);
    if (bestAltEver > 0) {
        canvas.setCursor(10, 254);
        canvas.printf("Rekord: %.1f m  Flug #%d", bestAltEver, bestFlightNum);
    }

    // ── AUS-Button (3 s halten) ──────────────────────────────────────────────
    canvas.fillRoundRect(40, 380, 200, 44, 8, canvas.color565(50, 25, 0));
    if (powerOffProgress > 0.0f) {
        int fillW = (int)(198 * powerOffProgress);
        if (fillW > 0) canvas.fillRoundRect(41, 381, fillW, 42, 7, canvas.color565(200, 100, 0));
    }
    canvas.drawRoundRect(40, 380, 200, 44, 8, COL_ORANGE);
    canvas.setTextColor(COL_ORANGE); canvas.setTextSize(3);
    canvas.setCursor(68, 388); canvas.print("STANDBY");
    canvas.setTextColor(COL_DIM); canvas.setTextSize(1);
    canvas.setCursor(60, 428); canvas.print("3 Sek. halten - Touch weckt auf");
}

// ── Screen 5: Flughistorie ────────────────────────────────────────────────────
static void drawScreenHistoryOverview() {
    pageHeader("HISTORIE", 4, NUM_PAGES);

    // ── Übersicht-Block ──────────────────────────────────────────────────────
    uint16_t divCol = canvas.color565(40, 40, 60);

    canvas.setTextColor(COL_DIM); canvas.setTextSize(1);
    canvas.setCursor(10, 34); canvas.print("FLUEGE GESAMT");
    canvas.setTextColor(COL_TEXT); canvas.setTextSize(4);
    canvas.setCursor(10, 44);
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", totalFlights);
    canvas.print(buf);

    if (bestAltEver > 0) {
        canvas.setTextColor(COL_DIM); canvas.setTextSize(1);
        canvas.setCursor(155, 34); canvas.print("REKORD HOEHE");
        canvas.setTextColor(COL_CYAN); canvas.setTextSize(3);
        canvas.setCursor(155, 44);
        snprintf(buf, sizeof(buf), "%.1f", bestAltEver);
        canvas.print(buf);
        canvas.setTextColor(COL_DIM); canvas.setTextSize(2); canvas.print("m");
        canvas.setTextColor(COL_DIM); canvas.setTextSize(1);
        canvas.setCursor(155, 82);
        snprintf(buf, sizeof(buf), "Flug #%d", bestFlightNum);
        canvas.print(buf);
    }

    canvas.drawFastHLine(0, 100, 280, divCol);

    // ── Flugliste ────────────────────────────────────────────────────────────
    if (leaderboardCount == 0) {
        canvas.setTextColor(COL_DIM); canvas.setTextSize(2);
        canvas.setCursor(20, 200); canvas.print("Keine Fluege");
        canvas.setCursor(40, 224); canvas.print("gespeichert");
    } else {
        int maxRows = min(leaderboardCount, 8);
        int rowH = 37;
        for (int i = 0; i < maxRows; i++) {
            int ry = 104 + i * rowH;
            bool isBest = (leaderboard[i].flightNum == bestFlightNum && bestAltEver > 0);
            uint16_t rowBg = isBest ? canvas.color565(0, 30, 50) : canvas.color565(12, 12, 18);
            canvas.fillRect(0, ry, 280, rowH - 2, rowBg);

            // Flight number
            canvas.setTextColor(isBest ? COL_CYAN : COL_DIM);
            canvas.setTextSize(2);
            canvas.setCursor(8, ry + 10);
            snprintf(buf, sizeof(buf), "#%d", leaderboard[i].flightNum);
            canvas.print(buf);

            // Altitude
            canvas.setTextColor(isBest ? COL_CYAN : COL_TEXT);
            canvas.setTextSize(2);
            canvas.setCursor(72, ry + 10);
            snprintf(buf, sizeof(buf), "%.1fm", leaderboard[i].maxAlt);
            canvas.print(buf);

            // Speed
            canvas.setTextColor(COL_YELLOW);
            canvas.setTextSize(2);
            canvas.setCursor(186, ry + 10);
            snprintf(buf, sizeof(buf), "%.0f", leaderboard[i].maxSpeed * 3.6f);
            canvas.print(buf);
            canvas.setTextColor(COL_DIM); canvas.setTextSize(1); canvas.print("km/h");

            // Chevron
            canvas.setTextColor(COL_DIM); canvas.setTextSize(1);
            canvas.setCursor(269, ry + 14); canvas.print(">");

            canvas.drawFastHLine(0, ry + rowH - 2, 280, divCol);
        }
        if (leaderboardCount > 8) {
            canvas.setTextColor(COL_DIM); canvas.setTextSize(1);
            int y8 = 104 + 8 * rowH;
            canvas.setCursor(90, y8 + 4);
            canvas.printf("... %d weitere", leaderboardCount - 8);
        }
    }

    // ── RESET Button ─────────────────────────────────────────────────────────
    canvas.fillRoundRect(20, 406, 240, 38, 8, canvas.color565(60, 10, 10));
    // Progress fill while holding
    if (resetHoldProgress > 0.0f) {
        int fillW = (int)(238 * resetHoldProgress);
        if (fillW > 0) canvas.fillRoundRect(21, 407, fillW, 36, 7, canvas.color565(160, 20, 20));
    }
    canvas.drawRoundRect(20, 406, 240, 38, 8, COL_RED);
    canvas.setTextColor(COL_RED); canvas.setTextSize(2);
    canvas.setCursor(72, 417); canvas.print("ALLE LOESCHEN");
}

static void drawScreenHistoryDetail() {
    if (historyIndex >= leaderboardCount) historyIndex = leaderboardCount - 1;
    if (historyIndex < 0) historyIndex = 0;
    const FlightRecord& r = leaderboard[historyIndex];

    // Header
    canvas.fillRect(0, 0, 280, 28, canvas.color565(20, 20, 50));
    canvas.setTextColor(COL_CYAN); canvas.setTextSize(2);
    canvas.setCursor(10, 6);
    canvas.printf("< FLUG #%d", r.flightNum);
    canvas.setTextColor(COL_YELLOW); canvas.setTextSize(1);
    canvas.setCursor(200, 10);
    canvas.printf("%d/%d", historyIndex + 1, leaderboardCount);
    // page dots
    for (int i = 0; i < NUM_PAGES; i++) {
        uint16_t c = (i == 4) ? TFT_WHITE : canvas.color565(80,80,80);
        canvas.fillCircle(280/2 - (NUM_PAGES-1)*8 + i*16, 447, 4, c);
    }

    char bl[16], br[16];
    const int cx = 140, cw = 138;
    const int ry[3] = { 42, 186, 330 };
    uint16_t div = canvas.color565(40, 40, 60);
    canvas.drawFastVLine(cx - 1, 42, 390, div);
    canvas.drawFastHLine(0, ry[1] - 2, 280, div);
    canvas.drawFastHLine(0, ry[2] - 2, 280, div);

    snprintf(bl, sizeof(bl), "%.1f", r.maxAlt);
    maxCell(0,  ry[0], cw, "MAX HOEHE",   bl, "m",   COL_CYAN);
    snprintf(br, sizeof(br), "%.2f", r.maxG);
    maxCell(cx, ry[0], cw, "MAX G",       br, "G",   COL_ORANGE);
    snprintf(bl, sizeof(bl), "%.0f", r.maxSpeed * 3.6f);
    maxCell(0,  ry[1], cw, "MAX SPEED",   bl, "km/h", COL_YELLOW);
    uint16_t sc = r.stability > 70 ? COL_GREEN : r.stability > 40 ? COL_YELLOW : COL_RED;
    snprintf(br, sizeof(br), "%.0f", r.stability);
    maxCell(cx, ry[1], cw, "STABILITAET", br, "%",   sc);
    snprintf(bl, sizeof(bl), "%.1f", r.flightTime);
    maxCell(0,  ry[2], cw, "FLUGZEIT",    bl, "s",   COL_TEXT);

    if (historyIndex < leaderboardCount - 1) {
        canvas.setTextColor(COL_DIM); canvas.setTextSize(1);
        canvas.setCursor(100, 432); canvas.print("v aelter");
    }
}

static void drawScreenHistory() {
    if (historyShowDetail && leaderboardCount > 0) {
        drawScreenHistoryDetail();
    } else {
        historyShowDetail = false;
        drawScreenHistoryOverview();
    }
}

// ── Page state ───────────────────────────────────────────────────────────────
static void drawUI() {
    if (debugMode) { drawDebug(); return; }
    if (countdownActive) { drawCountdown(); return; }

    canvas.fillScreen(COL_BG);

    switch (currentPage) {
        case 0: drawScreenAltitude();   break;
        case 1: drawScreenGraph();      break;
        case 2: drawScreenMaxValues();  break;
        case 3: drawScreenRearm();      break;
        case 4: drawScreenHistory();    break;
    }

    canvas.pushSprite(0, 0);
}

// Read FT3168 touch controller directly over Wire (addr 0x38)
static bool ft3168Read(int32_t& x, int32_t& y) {
    Wire.beginTransmission(0x38);
    Wire.write(0x02);
    if (Wire.endTransmission(false) != 0) return false;
    Wire.requestFrom(0x38, 5);
    if (Wire.available() < 5) return false;
    uint8_t fingers = Wire.read() & 0x0F;
    uint8_t xh = Wire.read(); uint8_t xl = Wire.read();
    uint8_t yh = Wire.read(); uint8_t yl = Wire.read();
    if (fingers == 0) return false;
    x = ((xh & 0x0F) << 8) | xl;
    y = ((yh & 0x0F) << 8) | yl;
    return true;
}

static void handleTouch() {
    int32_t tx, ty;
    bool touched = ft3168Read(tx, ty);

    static int32_t swipeStartX = -1;
    static int32_t swipeStartY = -1;
    static int32_t lastTx = 0, lastTy = 0;
    static unsigned long swipeStartMs = 0;
    static bool swipeActive = false;

    if (touched) {
        lastTx = tx; lastTy = ty;
        if (!swipeActive) {
            swipeStartX  = tx;
            swipeStartY  = ty;
            swipeStartMs = millis();
            swipeActive  = true;
        }
        // Long-press progress for reset button
        if (currentPage == 4 && !historyShowDetail
                && tx >= 20 && tx <= 260 && ty >= 406 && ty <= 444) {
            resetHoldProgress = fminf((millis() - swipeStartMs) / 5000.0f, 1.0f);
        } else {
            resetHoldProgress = 0.0f;
        }
        // Long-press progress for power-off button (Scharfstellen-Seite)
        if (currentPage == 3 && tx >= 40 && tx <= 240 && ty >= 380 && ty <= 424) {
            powerOffProgress = fminf((millis() - swipeStartMs) / 3000.0f, 1.0f);
        } else {
            powerOffProgress = 0.0f;
        }
        return;
    }
    float heldProgress = resetHoldProgress;
    float heldPower    = powerOffProgress;
    resetHoldProgress = 0.0f;
    powerOffProgress  = 0.0f;

    // finger lifted — use lastTx/lastTy as swipe end (tx/ty undefined on release)
    if (swipeActive) {
        swipeActive = false;
        unsigned long dt = millis() - swipeStartMs;

        // Long-press power-off (3 s hold on AUS button)
        if (heldPower >= 1.0f) {
            powerOff();
            return;
        }

        // Long-press reset (5 s hold on ALLE LOESCHEN button)
        if (heldProgress >= 1.0f) {
            resetAllRecords();
            historyIndex = 0;
            historyShowDetail = false;
            return;
        }
        int32_t dx = lastTx - swipeStartX;
        int32_t dy = lastTy - swipeStartY;

        if (dt < 600) {
            // Vertical swipe on history detail page
            if (currentPage == 4 && historyShowDetail && abs(dy) > 40 && abs(dy) > abs(dx)) {
                if (dy < 0) historyIndex = min(historyIndex + 1, leaderboardCount - 1);
                else        historyIndex = max(historyIndex - 1, 0);
                return;
            }
            // Horizontal swipe = page change (swipe right on history detail = back to overview)
            if (abs(dx) > 40 && abs(dx) > abs(dy)) {
                if (debugMode) { debugMode = false; return; }
                if (currentPage == 4 && historyShowDetail && dx > 0) {
                    historyShowDetail = false;
                    return;
                }
                if (dx < 0) currentPage = (currentPage + 1) % NUM_PAGES;
                else         currentPage = (currentPage - 1 + NUM_PAGES) % NUM_PAGES;
                return;
            }
        }

        // tap (short, small movement)
        if (dt < 400 && abs(dx) < 20 && abs(dy) < 20) {
            if (lastTx > 220 && lastTy < 40) { debugMode = !debugMode; return; }
            if (debugMode) { debugMode = false; return; }

            // Historie: Detail zurück zur Übersicht (Tap auf Header)
            if (currentPage == 4 && historyShowDetail && lastTy < 40) {
                historyShowDetail = false;
                return;
            }

            // Historie: Übersicht — Flugreihe antippen → Detail
            if (currentPage == 4 && !historyShowDetail && leaderboardCount > 0
                    && lastTy >= 104 && lastTy < 104 + min(leaderboardCount, 8) * 37) {
                int idx = (lastTy - 104) / 37;
                if (idx >= 0 && idx < leaderboardCount) {
                    historyIndex = idx;
                    historyShowDetail = true;
                }
                return;
            }

            // Historie: RESET-Button (long-press handled on release below)

            // Scharf-stellen Seite: Toggle BEREIT ↔ SCHARF
            if (currentPage == 3 && lastTy >= 116 && lastTy <= 216) {
                if (flight.phase == ARMED) {
                    flight.phase = IDLE;
                } else if (flight.phase == IDLE || flight.phase == LANDED) {
                    flight = FlightData();
                    sampleCount = 0;
                    calibrateGround();
                    flight.phase = ARMED;
                    currentPage = 1;  // direkt zu Flugpfad
                }
                return;
            }

            if (flight.phase == IDLE && !countdownActive && touchInButton(lastTx, lastTy, btnArm)) {
                countdownActive = true;
                countdownStart  = millis();
            } else if (flight.phase == LANDED && touchInButton(lastTx, lastTy, btnReset)) {
                flight     = FlightData();
                sampleCount = 0;
            }
        }
    }
}

// --- Shared JSON builder ---
static char flightJson[512];   // global — safe to call from lwIP task (no stack alloc)

static void buildFlightJson(char* json, size_t len) {
    float elapsed = 0;
    if (flight.phase >= BOOST) {
        elapsed = (flight.phase == LANDED)
            ? (flight.landedTime - flight.launchTime) / 1000.0f
            : (millis() - flight.launchTime) / 1000.0f;
    }
    float timeToApogee = 0;
    if (flight.apogeeTime > flight.launchTime)
        timeToApogee = (flight.apogeeTime - flight.launchTime) / 1000.0f;
    const char* rating = getFlightRating(flight.maxAltitude, flight.stabilityScore);
    bool newRecord = (flight.phase == LANDED && flight.maxAltitude >= bestAltEver);
    snprintf(json, len,
        "{\"p\":%d,\"a\":%.2f,\"ma\":%.2f,\"v\":%.2f,\"ms\":%.2f,"
        "\"ac\":%.2f,\"mg\":%.2f,\"t\":%.2f,\"ta\":%.2f,"
        "\"dr\":%.2f,\"ad\":%.2f,\"ch\":%d,"
        "\"rc\":%d,\"tc\":%d,\"mrr\":%.1f,\"mtr\":%.1f,\"stab\":%.1f,\"is\":%.1f,"
        "\"fn\":%d,\"ba\":%.1f,\"bn\":%d,\"nr\":%d,\"as\":%d,\"s10\":%.2f,\"rat\":\"%s\"}",
        (int)flight.phase, flight.altitude, flight.maxAltitude,
        flight.velocity, flight.maxSpeed, flight.accelZ, flight.maxAccel,
        elapsed, timeToApogee,
        flight.descentRate, flight.avgDescentSpeed, flight.chuteDetected ? 1 : 0,
        flight.rollCount, flight.tumbleCount, flight.maxRollRate,
        flight.maxTumbleRate, flight.stabilityScore, flight.instantStability,
        totalFlights, bestAltEver, bestFlightNum, newRecord ? 1 : 0, armSession, flight.speed10m, rating);
}

// --- SSE: push to connected browser clients ---
static void sendSSE() {
    if (millis() - lastSSE < 100) return;  // 10 Hz
    lastSSE = millis();
    buildFlightJson(flightJson, sizeof(flightJson));
    if (events.count() > 0)
        events.send(flightJson, NULL, millis());
}

void setup() {
    // --- Standby-Check: MUSS als Allererstes laufen, damit der Wake-Zyklus kurz bleibt.
    // Display, WiFi und Sensoren werden hier bewusst NICHT initialisiert.
    if (rtcStandby) {
        Wire.begin(I2C_SDA, I2C_SCL, 400000);
        delay(2);

        bool wake = false;
        if (rtcSkipWakes > 0) {
            rtcSkipWakes--;          // Finger vom STANDBY-Druck noch am Display → ignorieren
        } else {
            int32_t tx, ty;
            wake = ft3168Read(tx, ty);
        }

        if (!wake) {                 // nichts berührt → sofort weiterschlafen
            esp_sleep_enable_timer_wakeup(STANDBY_POLL_US);
            esp_deep_sleep_start();
        }
        rtcStandby = false;          // berührt → normal hochfahren
    }

    // Hinweis: Dieses Board hat kein Power-Management-IC und keinen PWR_EN-Pin.
    // Abschalten per Software ist nicht möglich — nur Akku abstecken (siehe Waveshare-Wiki).

    // Hard-reset CO5300 display panel immediately — after RST the panel stays powered
    // and the SPI bus may be in an inconsistent state; toggling pin_rst clears it.
    pinMode(21, OUTPUT);
    digitalWrite(21, LOW);
    delay(20);
    digitalWrite(21, HIGH);
    delay(150);

    Serial.begin(115200);
    delay(500);
    Serial.println("=== RocketTracker boot ===");

    loadRecords();

    // I2C must start BEFORE display.init() so LovyanGFX touch can use Wire
    Wire.begin(I2C_SDA, I2C_SCL);
    // Intercept log output to drop Wire.cpp i2c spam (esp_log_level_set is bypassed by Arduino macros)
    esp_log_set_vprintf([](const char* fmt, va_list args) -> int {
        if (strstr(fmt, "Wire.cpp") || strstr(fmt, "i2cWriteRead")) return 0;
        return vprintf(fmt, args);
    });
    Serial.println("I2C started");

    // --- Display + Framebuffer (must allocate ~255KB internal RAM before WiFi) ---
    Serial.println("Display init...");
    delay(800);
    bool dispOk = display.init();
    Serial.printf("display.init() = %s\n", dispOk ? "OK" : "FAIL");
    display.setRotation(0);
    display.setBrightness(200);
    display.fillScreen(TFT_RED);   delay(200);
    display.fillScreen(TFT_GREEN); delay(200);
    display.fillScreen(TFT_BLACK);

    Serial.printf("Display ready  heap: %u KB\n", ESP.getFreeHeap() / 1024);

    // --- WiFi AP + Web server (after framebuffer, before canvas) ---
    Serial.println("Starting WiFi AP...");
    WiFi.mode(WIFI_AP);
    delay(100);
    // Eindeutige SSID pro Board: WaterRocket-XXXX (letzte 2 MAC-Bytes)
    uint8_t mac[6];
    WiFi.softAPmacAddress(mac);
    snprintf(AP_SSID, sizeof(AP_SSID), "WaterRocket-%02X%02X", mac[4], mac[5]);
    bool apOk = WiFi.softAP(AP_SSID, AP_PASS, 6);
    WiFi.setSleep(false);
    Serial.printf("WiFi AP: %s  ok=%d  IP: %s\n", AP_SSID, apOk, WiFi.softAPIP().toString().c_str());

    dnsServer.start(53, "*", WiFi.softAPIP());

    events.onConnect([](AsyncEventSourceClient *client) {
        client->send("{\"p\":0}", NULL, millis());
    });
    server.addHandler(&events);
    server.on("/", HTTP_GET,                [](AsyncWebServerRequest *r){ r->send(200, "text/html", INDEX_HTML); });
    server.on("/generate_204",        HTTP_GET, [](AsyncWebServerRequest *r){ r->send(200, "text/html", INDEX_HTML); });
    server.on("/hotspot-detect.html", HTTP_GET, [](AsyncWebServerRequest *r){ r->send(200, "text/html", INDEX_HTML); });
    server.on("/connecttest.txt",     HTTP_GET, [](AsyncWebServerRequest *r){ r->send(200, "text/html", INDEX_HTML); });
    server.on("/data", HTTP_GET, [](AsyncWebServerRequest *r){ r->send(200, "application/json", flightJson); });
    server.on("/leaderboard", HTTP_GET, [](AsyncWebServerRequest *request) {
        String json = "[";
        for (int i = 0; i < leaderboardCount; i++) {
            if (i > 0) json += ",";
            char entry[128];
            snprintf(entry, sizeof(entry),
                "{\"n\":%d,\"alt\":%.1f,\"spd\":%.1f,\"g\":%.1f,\"stab\":%.0f,\"t\":%.1f}",
                leaderboard[i].flightNum, leaderboard[i].maxAlt,
                leaderboard[i].maxSpeed, leaderboard[i].maxG,
                leaderboard[i].stability, leaderboard[i].flightTime);
            json += entry;
        }
        json += "]";
        request->send(200, "application/json", json);
    });
    server.onNotFound([](AsyncWebServerRequest *r){ r->send(200, "text/html", INDEX_HTML); });
    server.begin();
    Serial.println("Web server started");
    Serial.printf("Free heap after WiFi: %u KB\n", ESP.getFreeHeap() / 1024);

    // Sprite is 280*456*2 = 255KB — too big for internal heap (243KB after WiFi).
    // Force PSRAM allocation; LovyanGFX pushSprite bounces PSRAM->display via internal DMA buffer.
    canvas.setColorDepth(16);
    canvas.setPsram(true);
    bool spriteOk = (canvas.createSprite(280, 456) != nullptr);
    canvas.setSwapBytes(false);
    Serial.printf("createSprite = %s  buffer=%p  heap: %u KB  psram free: %u KB\n",
                  spriteOk ? "OK" : "FAIL", canvas.getBuffer(),
                  ESP.getFreeHeap() / 1024, ESP.getFreePsram() / 1024);


    // --- Sensors ---
    Serial.println("I2C init...");

    // Beide Adressen probieren — bmpOk muss in BEIDEN Fällen gesetzt werden
    uint8_t bmpAddr = 0;
    if      (bmp.begin_I2C(0x77, &Wire)) bmpAddr = 0x77;
    else if (bmp.begin_I2C(0x76, &Wire)) bmpAddr = 0x76;

    if (bmpAddr == 0) {
        Serial.println("ERROR: BMP388 not found (weder 0x77 noch 0x76)!");
        canvas.fillScreen(TFT_BLACK);
        canvas.setTextColor(TFT_RED);
        canvas.setTextSize(2);
        canvas.setCursor(20, 200);
        canvas.print("BMP388 ERROR");
        canvas.pushSprite(0, 0);
        // Don't halt — continue without sensor
    } else {
        // Forced mode only — do NOT set ODR (normal/continuous mode conflicts with performReading)
        bmp.setTemperatureOversampling(BMP3_OVERSAMPLING_2X);
        bmp.setPressureOversampling(BMP3_OVERSAMPLING_8X);
        bmp.setIIRFilterCoeff(BMP3_IIR_FILTER_COEFF_3);
        bmpOk = true;
        Serial.printf("BMP388 OK (0x%02X)\n", bmpAddr);
    }

    if (!mpu.begin(0x68, &Wire)) {
        Serial.println("ERROR: MPU6050 not found!");
    } else {
        // ±16 g: Wasserraketen erreichen im Schub 10-30 g — 8 g würde clippen
        mpu.setAccelerometerRange(MPU6050_RANGE_16_G);
        mpu.setGyroRange(MPU6050_RANGE_500_DEG);
        mpu.setFilterBandwidth(MPU6050_BAND_21_HZ);
        mpuOk = true;
        Serial.println("MPU6050 OK");
    }

    // Check FT3168 touch (0x38) — only probe with STOP to avoid spurious errors
    Wire.beginTransmission(0x38);
    touchOk = (Wire.endTransmission() == 0);
    Serial.printf("Touch (FT3168): %s\n", touchOk ? "OK" : "nicht gefunden");

    flight.lastUpdateUs = micros();
    flight.phase = ARMED;

    updateBattery();
    Serial.printf("Battery: %d%%\n", batteryPct);
    Serial.println("=== Setup complete ===");
}

void loop() {
    dnsServer.processNextRequest();

    // Neuer ARMED-Zustand (Scharfstellen) → Browser soll neu laden
    static FlightPhase prevLoopPhase = IDLE;
    if (flight.phase == ARMED && prevLoopPhase != ARMED) armSession++;
    prevLoopPhase = flight.phase;

    if (flight.phase >= ARMED && flight.phase <= PARACHUTE) {
        updateFlight();
    } else if (bmpOk) {
        // Keep live sensor display updated in IDLE/LANDED
        static unsigned long lastIdlePoll = 0;
        if (millis() - lastIdlePoll > 200) {
            lastIdlePoll = millis();
            if (bmp.performReading()) {
                flight.rawAltitude = bmp.readAltitude(1013.25f);
                flight.rawPressure = bmp.pressure / 100.0f;
                flight.rawTemp     = bmp.temperature;
                flight.altitude    = flight.rawAltitude - flight.groundAltitude;
            }
        }
    }

    if (millis() - lastBatRead > 5000) {
        lastBatRead = millis();
        updateBattery();
    }

    handleTouch();
    yield();        // give WiFi stack CPU time before display push
    drawUI();
    sendSSE();
    yield();        // give WiFi stack CPU time after display push

    if (flight.phase >= ARMED && flight.phase <= PARACHUTE) {
        delay(20);
    } else {
        delay(30);
    }
}
