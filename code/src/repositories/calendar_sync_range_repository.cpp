#include "repositories/calendar_sync_range_repository.h"

#include <algorithm>
#include <ctime>

#include "utils/sqlite_utils.h"

CalendarSyncRangeRepository::CalendarSyncRangeRepository(SQLite::Database& db) : db(db) {}

bool CalendarSyncRangeRepository::isRangeCovered(const long long calendarId,
                                                 const long long startEpoch,
                                                 const long long endEpoch) {
    if (startEpoch >= endEpoch) {
        return true;
    }

    SQLite::Statement query(
        db,
        "SELECT id, calendar_id, start_epoch, end_epoch, synced_at, last_viewed_at, sync_token "
        "FROM calendar_sync_ranges "
        "WHERE calendar_id = ? AND end_epoch > ? AND start_epoch < ? "
        "ORDER BY start_epoch ASC");
    BindInt64(query, 1, calendarId);
    BindInt64(query, 2, startEpoch);
    BindInt64(query, 3, endEpoch);

    long long coveredUntil = startEpoch;
    while (query.executeStep()) {
        const CalendarSyncRange range = mapRow(query);
        if (range.startEpoch > coveredUntil) {
            return false;
        }
        coveredUntil = std::max(coveredUntil, range.endEpoch);
        if (coveredUntil >= endEpoch) {
            return true;
        }
    }

    return false;
}

void CalendarSyncRangeRepository::markRangeCovered(const long long calendarId,
                                                   long long startEpoch,
                                                   long long endEpoch) {
    if (startEpoch >= endEpoch) {
        return;
    }

    RunInSavepoint(db, "calendar_sync_range_mark", [&]() {
        auto exact = getExactRange(calendarId, startEpoch, endEpoch);
        if (exact.has_value()) {
            exact->syncedAt = static_cast<long long>(std::time(nullptr));
            exact->lastViewedAt = exact->syncedAt;
            upsertRange(*exact);
            return;
        }

        SQLite::Statement overlapping(
            db,
            "SELECT id, calendar_id, start_epoch, end_epoch, synced_at, last_viewed_at, sync_token "
            "FROM calendar_sync_ranges "
            "WHERE calendar_id = ? AND end_epoch >= ? AND start_epoch <= ? "
            "ORDER BY start_epoch ASC");
        BindInt64(overlapping, 1, calendarId);
        BindInt64(overlapping, 2, startEpoch);
        BindInt64(overlapping, 3, endEpoch);

        std::vector<long long> idsToDelete;
        while (overlapping.executeStep()) {
            const CalendarSyncRange range = mapRow(overlapping);
            idsToDelete.push_back(range.id);
            startEpoch = std::min(startEpoch, range.startEpoch);
            endEpoch = std::max(endEpoch, range.endEpoch);
        }

        for (const long long id : idsToDelete) {
            SQLite::Statement deleteQuery(db, "DELETE FROM calendar_sync_ranges WHERE id = ?");
            BindInt64(deleteQuery, 1, id);
            deleteQuery.exec();
        }

        SQLite::Statement insert(
            db,
            "INSERT INTO calendar_sync_ranges(calendar_id, start_epoch, end_epoch, synced_at, last_viewed_at, sync_token) "
            "VALUES(?, ?, ?, ?, ?, '')");
        BindInt64(insert, 1, calendarId);
        BindInt64(insert, 2, startEpoch);
        BindInt64(insert, 3, endEpoch);
        BindInt64(insert, 4, static_cast<long long>(std::time(nullptr)));
        BindInt64(insert, 5, static_cast<long long>(std::time(nullptr)));
        insert.exec();
    });
}

std::optional<CalendarSyncRange> CalendarSyncRangeRepository::getExactRange(
    const long long calendarId,
    const long long startEpoch,
    const long long endEpoch) {
    SQLite::Statement query(
        db,
        "SELECT id, calendar_id, start_epoch, end_epoch, synced_at, last_viewed_at, sync_token "
        "FROM calendar_sync_ranges "
        "WHERE calendar_id = ? AND start_epoch = ? AND end_epoch = ?");
    BindInt64(query, 1, calendarId);
    BindInt64(query, 2, startEpoch);
    BindInt64(query, 3, endEpoch);

    if (!query.executeStep()) {
        return std::nullopt;
    }

    return mapRow(query);
}

std::vector<CalendarSyncRange> CalendarSyncRangeRepository::getRangesForCalendar(const long long calendarId) {
    SQLite::Statement query(
        db,
        "SELECT id, calendar_id, start_epoch, end_epoch, synced_at, last_viewed_at, sync_token "
        "FROM calendar_sync_ranges "
        "WHERE calendar_id = ? "
        "ORDER BY start_epoch ASC");
    BindInt64(query, 1, calendarId);

    std::vector<CalendarSyncRange> ranges;
    while (query.executeStep()) {
        ranges.push_back(mapRow(query));
    }
    return ranges;
}

std::vector<CalendarSyncRange> CalendarSyncRangeRepository::getMostRecentlyViewedRanges(
    const long long calendarId,
    const int limit) {
    SQLite::Statement query(
        db,
        "SELECT id, calendar_id, start_epoch, end_epoch, synced_at, last_viewed_at, sync_token "
        "FROM calendar_sync_ranges "
        "WHERE calendar_id = ? "
        "ORDER BY last_viewed_at DESC, synced_at DESC "
        "LIMIT ?");
    BindInt64(query, 1, calendarId);
    query.bind(2, limit);

    std::vector<CalendarSyncRange> ranges;
    while (query.executeStep()) {
        ranges.push_back(mapRow(query));
    }
    return ranges;
}

void CalendarSyncRangeRepository::upsertRange(const CalendarSyncRange& range) {
    RunInSavepoint(db, "calendar_sync_range_upsert", [&]() {
        SQLite::Statement query(
            db,
            "INSERT INTO calendar_sync_ranges(calendar_id, start_epoch, end_epoch, synced_at, last_viewed_at, sync_token) "
            "VALUES(?, ?, ?, ?, ?, ?) "
            "ON CONFLICT(calendar_id, start_epoch, end_epoch) DO UPDATE SET "
            "synced_at = excluded.synced_at, "
            "last_viewed_at = excluded.last_viewed_at, "
            "sync_token = excluded.sync_token");
        BindInt64(query, 1, range.calendarId);
        BindInt64(query, 2, range.startEpoch);
        BindInt64(query, 3, range.endEpoch);
        BindInt64(query, 4, range.syncedAt);
        BindInt64(query, 5, range.lastViewedAt);
        query.bind(6, range.syncToken);
        query.exec();
    });
}

CalendarSyncRange CalendarSyncRangeRepository::mapRow(SQLite::Statement& query) {
    CalendarSyncRange range;
    int col = 0;
    range.id = query.getColumn(col++).getInt64();
    range.calendarId = query.getColumn(col++).getInt64();
    range.startEpoch = query.getColumn(col++).getInt64();
    range.endEpoch = query.getColumn(col++).getInt64();
    range.syncedAt = query.getColumn(col++).getInt64();
    range.lastViewedAt = query.getColumn(col).isNull() ? 0 : query.getColumn(col).getInt64(); col++;
    range.syncToken = query.getColumn(col).isNull() ? "" : query.getColumn(col).getString();
    return range;
}
