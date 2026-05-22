#pragma once

#include "utils/types.h"

struct RecurrenceOverride {
    long long id = -1;
    long long masterEventId = 0;
    long long originalStart = 0;
    RecurrenceOverrideType type = RecurrenceOverrideType::CANCELLED;
    long long replacementEventId = 0;
    int syncStatus = PENDING_INSERT;
    long long deletedAt = 0;
    long long createdAt = 0;
    long long updatedAt = 0;
};
