#pragma once
#include <Arduino.h>

struct ClassroomSafety {
    const char* mode;
    const char* door;
    const char* trend;
    int heat10;
    bool valid;
    bool fire;
    bool panic;
};

void classroomSafetyBegin();
void classroomSafetyTick(bool flame);
void classroomSafetySample(float temperature, float humidity);
void classroomSafetyCard(bool teacher);
ClassroomSafety classroomSafety();
bool classroomSafetyCommand(const String& command);
bool classroomSafetyLed(uint32_t now, uint8_t& r, uint8_t& g, uint8_t& b);
bool classroomSafetyTone(uint32_t now, bool& on);
