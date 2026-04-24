#pragma once

#include "repositories/account_repository.h"
#include "repositories/calendar_repository.h"
#include "repositories/event_repository.h"

struct RepositoryHolder {
    EventRepository eventRepository;
    CalendarRepository calendarRepository;
    AccountRepository accountRepository;
};
