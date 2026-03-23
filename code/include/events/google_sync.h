#pragma once
#include "calendar.h"

class GoogleCalendarSync {
    public:
    GoogleCalendarSync(CalendarRepository& calendars);

    void syncCalendars(const std::string& accessToken);

    private:
    CalendarRepository& calendars;
};