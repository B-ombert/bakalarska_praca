#pragma once
#include <string>
#include "oauth_ms.h"

int GetMSCalendar();  // entry function, volá GetMSCalendarEvents
void GetMSCalendarEvents(const std::string& access_token);
std::string BuildMSEventJson(const simpleEvent& event);
void CreateMSCalendarEvent(const simpleEvent& event);