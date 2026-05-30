#pragma once

#include <memory>
#include <string>

#include "repositories/calendar_repository.h"
#include "repositories/event_repository.h"
#include "sync/calendar_sync_service.h"

std::unique_ptr<CalendarSyncService> CreateCalendarSyncService(const std::string& provider,
                                                               CalendarRepository& calendarRepository,
                                                               EventRepository& eventRepository);
