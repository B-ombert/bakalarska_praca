#include "sync/calendar_sync_service_factory.h"

#include "sync/google_calendar_sync_service.h"
#include "sync/outlook_calendar_sync_service.h"
#include "utils/provider_utils.h"

std::unique_ptr<CalendarSyncService> CreateCalendarSyncService(const std::string& provider,
                                                               CalendarRepository& calendarRepository,
                                                               EventRepository& eventRepository) {
    if (provider == kProviderGoogle) {
        return std::make_unique<GoogleCalendarSyncService>(calendarRepository, eventRepository);
    }
    if (provider == kProviderMicrosoft) {
        return std::make_unique<OutlookCalendarSyncService>(calendarRepository, eventRepository);
    }

    return nullptr;
}
