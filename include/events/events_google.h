#pragma once
#include <string>
#include <vector>
#include "oauth_google.h"

int GetGoogleCalendar();
void GetGoogleCalendarEvents(const std::string& access_token);
std::string BuildGoogleEventJson(const simpleEvent& event);
std::vector<simpleEvent> GetGoogleEventsLastNDays(int days);
void CreateGoogleCalendarEvent(const simpleEvent& event);