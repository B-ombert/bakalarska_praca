#pragma once

#include "repositories/account_repository.h"
#include "repositories/calendar_repository.h"
#include "repositories/event_repository.h"

struct RepositoryHolder {
    CalendarRepository& calendarRepository;
    EventRepository& eventRepository;
    AccountRepository* accountRepository = nullptr;

    RepositoryHolder(CalendarRepository& calendarRepository, EventRepository& eventRepository)
        : calendarRepository(calendarRepository), eventRepository(eventRepository) {}

    RepositoryHolder(CalendarRepository& calendarRepository,
                     EventRepository& eventRepository,
                     AccountRepository& accountRepository)
        : calendarRepository(calendarRepository),
          eventRepository(eventRepository),
          accountRepository(&accountRepository) {}
};
