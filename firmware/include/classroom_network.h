#pragma once
#include <Arduino.h>

struct ClassroomReading {
    int temperature10;
    int humidity10;
    bool dhtOk;
    bool rfidOk;
    bool flame;
    bool watchdog;
    unsigned cards;
};

ClassroomReading classroomReading();
void classroomNetworkBegin();
void classroomCard(const char* role, const char* uid);
bool classroomNetworkCommand(const String& command);
void classroomUrgent(const char* kind);
