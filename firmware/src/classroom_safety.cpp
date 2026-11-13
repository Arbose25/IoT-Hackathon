#include "classroom_safety.h"
#include "classroom_network.h"
#include <Preferences.h>
#include <cmath>
#include <atomic>

#ifdef CLASSROOM_LEVEL2_ONLY
void classroomSafetyBegin() {}
void classroomSafetyTick(bool) {}
void classroomSafetySample(float, float) {}
void classroomSafetyCard(bool) {}
ClassroomSafety classroomSafety() { return {"MONITORING","UNLOCKED","STEADY",0,false,false,false}; }
bool classroomSafetyCommand(const String&) { return false; }
bool classroomSafetyLed(uint32_t, uint8_t&, uint8_t&, uint8_t&) { return false; }
bool classroomSafetyTone(uint32_t, bool&) { return false; }
#else
#ifndef CLASSROOM_HEAT_WARNING_C
#define CLASSROOM_HEAT_WARNING_C 32
#endif
#ifndef CLASSROOM_HEAT_DANGER_C
#define CLASSROOM_HEAT_DANGER_C 40
#endif
static_assert(CLASSROOM_HEAT_WARNING_C >= 25 && CLASSROOM_HEAT_DANGER_C <= 60 &&
              CLASSROOM_HEAT_DANGER_C - CLASSROOM_HEAT_WARNING_C > 2,
              "Invalid classroom heat thresholds");

namespace {
enum Heat { Safe, Warning, Danger };
struct Sample { float temperature, humidity; };
QueueHandle_t samples;
Preferences safetyNvs;
portMUX_TYPE guard = portMUX_INITIALIZER_UNLOCKED;
ClassroomSafety published{"SAFE","LOCKED","STEADY",0,false,false,false};
Heat heat = Safe;
bool fire, panic, valid, syntheticHeat, nvsReady, fireSaved, fireDirty;
int heat10;
const char* trend = "STEADY";
uint32_t flameStart, fakeFireUntil, fakePanicUntil, panicStart, clearStart;
bool flameTiming, panicTiming, clearTiming, fakeFire, fakePanic;
uint32_t lastPersistAttempt;
std::atomic<unsigned> resetRequest{0};

float indexC(float c, float humidity) {
    const float t = c * 1.8f + 32;
    float hi = (t + 61.0f + (t - 68.0f) * 1.2f + humidity * 0.094f) / 2;
    hi = (hi + t) / 2;
    if (hi >= 80) {
        const float r = humidity;
        hi = -42.379f + 2.04901523f*t + 10.14333127f*r - 0.22475541f*t*r
             - 0.00683783f*t*t - 0.05481717f*r*r + 0.00122874f*t*t*r
             + 0.00085282f*t*r*r - 0.00000199f*t*t*r*r;
        if (r < 13 && t >= 80 && t <= 112)
            hi -= ((13-r)/4)*sqrtf((17-fabsf(t-95))/17);
        if (r > 85 && t >= 80 && t <= 87) hi += ((r-85)/10)*((87-t)/5);
    }
    return (hi - 32) / 1.8f;
}

void evaluate(Sample s) {
    if (!isfinite(s.temperature) || !isfinite(s.humidity) ||
        s.temperature < 0 || s.temperature > 50 || s.humidity < 0 || s.humidity > 100) {
        valid = false;
        return;
    }
    const int next = lroundf(indexC(s.temperature, s.humidity)*10);
    trend = !valid ? "STEADY" : next > heat10 + 2 ? "RISING" : next < heat10 - 2 ? "FALLING" : "STEADY";
    valid = true;
    heat10 = next;
    const float risk = std::max(next/10.0f, s.temperature);
    if (risk >= CLASSROOM_HEAT_DANGER_C) heat = Danger;
    else if (heat == Danger && risk < CLASSROOM_HEAT_DANGER_C - 2) heat = Warning;
    else if (heat == Safe && risk >= CLASSROOM_HEAT_WARNING_C) heat = Warning;
    else if (heat == Warning && risk < CLASSROOM_HEAT_WARNING_C - 2) heat = Safe;
}

void publish() {
    ClassroomSafety next{fire ? "FIRE" : panic ? "PANIC" : heat == Danger ? "DANGER" :
                         heat == Warning ? "WARNING" : valid ? "SAFE" : "FAULT",
                         fire ? "UNLOCKED" : "LOCKED", trend, heat10, valid, fire, panic};
    portENTER_CRITICAL(&guard);
    published = next;
    portEXIT_CRITICAL(&guard);
}

bool held(bool active, bool& timing, uint32_t& since, uint32_t now, uint32_t duration) {
    if (!active) {
        timing = false;
        return false;
    }
    if (!timing) {
        since = now;
        timing = true;
    }
    return now - since >= duration;
}
}

void classroomSafetyBegin() {
    samples = xQueueCreate(1, sizeof(Sample));
    nvsReady = safetyNvs.begin("safety", false);
    fire = fireSaved = nvsReady && safetyNvs.getBool("fire", false);
    publish();
    if (!nvsReady) Serial.println("SAFETY ERROR: NVS unavailable");
    if (fire) Serial.println("SAFETY: FIRE LATCH RESTORED, DOOR SOFTWARE UNLOCKED");
}

void classroomSafetySample(float temperature, float humidity) {
    const Sample sample{temperature, humidity};
    if (samples) xQueueOverwrite(samples, &sample);
}

void classroomSafetyCard(bool teacher) {
    if (classroomSafety().fire) resetRequest.fetch_or(teacher ? 1U : 2U);
}

ClassroomSafety classroomSafety() {
    portENTER_CRITICAL(&guard);
    const auto copy = published;
    portEXIT_CRITICAL(&guard);
    return copy;
}

void classroomSafetyTick(bool flame) {
    const uint32_t now = millis();
    Sample sample{};
    if (samples && xQueueReceive(samples, &sample, 0) == pdPASS && !syntheticHeat) evaluate(sample);
    if (fakeFire && static_cast<int32_t>(now - fakeFireUntil) >= 0) fakeFire = false;
    if (fakePanic && static_cast<int32_t>(now - fakePanicUntil) >= 0) fakePanic = false;
    const bool activeFlame = flame || fakeFire;
    const bool confirmedFire = held(activeFlame, flameTiming, flameStart, now, 1000);
    const bool cleared = held(!activeFlame, clearTiming, clearStart, now, 1000);
    const bool confirmedPanic = held(fakePanic, panicTiming, panicStart, now, 3000);
    const char* event = nullptr;
    if (confirmedFire && !fire) {
        fire = true;
        fireDirty = !fireSaved;
        lastPersistAttempt = now - 1000;
        event = "fire";
        Serial.println("SAFETY: FIRE, DOOR SOFTWARE UNLOCKED");
    }
    publish();
    if (event) classroomUrgent(event);
    if (fireDirty && now - lastPersistAttempt >= 1000) {
        lastPersistAttempt = now;
        if (nvsReady && safetyNvs.putBool("fire", true) == 1) {
            fireSaved = true;
            fireDirty = false;
        } else {
            Serial.println("SAFETY ERROR: fire latch persistence failed");
        }
    }
    const bool newPanic = confirmedPanic && !panic;
    if (newPanic) {
        panic = true;
        Serial.println("SAFETY: PANIC (SIMULATED)");
    }
    const unsigned request = resetRequest.exchange(0);
    const bool accepted = (request & 1U) && fire && cleared && nvsReady &&
                          safetyNvs.putBool("fire", false) == 1;
    if (accepted) {
        fire = fireSaved = fireDirty = false;
        Serial.println("SAFETY: TEACHER RESET ACCEPTED");
    }
    if ((request & 2U) || ((request & 1U) && !accepted))
        Serial.println("SAFETY: FIRE RESET DENIED");
    publish();
    if (newPanic) classroomUrgent("panic");
    if (accepted) classroomUrgent("fire_reset");
    if ((request & 2U) || ((request & 1U) && !accepted)) classroomUrgent("reset_denied");
}

bool classroomSafetyCommand(const String& command) {
    if (command == "safety status") {
        const auto s = classroomSafety();
        Serial.printf("SAFETY: %s door=%s heat_quality=%s heat_c=%.1f trend=%s\n",
                      s.mode,s.door,s.valid ? "VALID" : "INVALID",s.heat10/10.0f,s.trend);
        return true;
    }
    const auto now = millis();
    if (command == "test heat warning") { syntheticHeat = true; evaluate({34,40}); }
    else if (command == "test heat danger") { syntheticHeat = true; evaluate({42,50}); }
    else if (command == "test heat cool") { syntheticHeat = true; evaluate({24,45}); }
    else if (command == "test heat invalid") { syntheticHeat = true; evaluate({NAN,NAN}); }
    else if (command == "test panic" || command == "test panic pulse") {
        fakePanic = true;
        fakePanicUntil = now + (command == "test panic" ? 3500 : 500);
    } else if (command == "test fire" || command == "test fire pulse") {
        fakeFire = true;
        fakeFireUntil = now + (command == "test fire" ? 60000 : 500);
    } else if (command == "test fire clear") fakeFire = false;
    else if (command == "test safety clear") {
        const bool hadPanic = panic;
        fakeFire = fakePanic = panic = syntheticHeat = false;
        valid = false;
        publish();
        if (hadPanic) classroomUrgent("panic_clear");
    } else return false;
    publish();
    Serial.println("SAFETY TEST: simulation updated");
    return true;
}

bool classroomSafetyLed(uint32_t now, uint8_t& r, uint8_t& g, uint8_t& b) {
    const auto s = classroomSafety();
    r = g = b = 0;
    if (s.fire) r = now % 200 < 100 ? 32 : 0;
    else if (s.panic) b = now % 400 < 200 ? 32 : 0;
    else if (strcmp(s.mode,"DANGER") == 0) r = now % 600 < 300 ? 20 : 0;
    else if (strcmp(s.mode,"WARNING") == 0) { r = 20; g = 8; }
    else return false;
    return true;
}

bool classroomSafetyTone(uint32_t now, bool& on) {
    const auto s = classroomSafety();
    if (s.fire) on = now % 500 < 250;
    else if (s.panic) on = now % 1000 < 200;
    else if (strcmp(s.mode,"DANGER") == 0) on = now % 3000 < 120;
    else return false;
    return true;
}
#endif
