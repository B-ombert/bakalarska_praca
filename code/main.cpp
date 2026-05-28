#include <iostream>

#include "models/account.h"
#include "models/calendar.h"
#include "repositories/account_repository.h"
#include "repositories/calendar_repository.h"
#include "ui/local_calendar_app.h"
#include "utils/app_paths.h"
#include "utils/sqlite_utils.h"
#include "utils/types.h"

namespace {

bool TableHasColumn(SQLite::Database& db, const std::string& tableName, const std::string& columnName) {
    SQLite::Statement query(db, "PRAGMA table_info(" + tableName + ")");
    while (query.executeStep()) {
        if (query.getColumn(1).getString() == columnName) {
            return true;
        }
    }
    return false;
}

bool InitializeSchema(SQLite::Database& db) {
    ConfigureSqliteConnection(db);
    db.exec("CREATE TABLE IF NOT EXISTS accounts ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT, "
            "name TEXT NOT NULL, "
            "provider TEXT NOT NULL, "
            "provider_user_id TEXT NOT NULL, "
            "email TEXT DEFAULT '', "
            "refresh_token TEXT NOT NULL, "
            "UNIQUE (provider, provider_user_id)"
            ")");

    if (!TableHasColumn(db, "accounts", "email")) {
        db.exec("ALTER TABLE accounts ADD COLUMN email TEXT DEFAULT ''");
    }

    db.exec("CREATE INDEX IF NOT EXISTS idx_accounts_provider "
            "ON accounts(provider, provider_user_id)");

    db.exec("CREATE TABLE IF NOT EXISTS calendars ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT, "
            "account_id INTEGER, "
            "provider_calendar_id TEXT, "
            "name TEXT, "
            "description TEXT, "
            "timezone TEXT NOT NULL, "
            "color_hex TEXT NOT NULL DEFAULT '#1A73E8', "
            "is_primary INTEGER DEFAULT 0, "
            "is_read_only INTEGER DEFAULT 0, "
            "is_shared INTEGER DEFAULT 0, "
            "sync_enabled INTEGER DEFAULT 1, "
            "sync_status INTEGER DEFAULT 0, "
            "deleted_at INTEGER DEFAULT 0, "
            "FOREIGN KEY(account_id) REFERENCES accounts(id) ON DELETE CASCADE, "
            "UNIQUE (account_id, provider_calendar_id)"
            ")");

    db.exec("CREATE TABLE IF NOT EXISTS events ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT, "
            "calendar_id INTEGER NOT NULL, "
            "provider_event_id TEXT, "
            "provider_master_id TEXT, "
            "recurrence_group_id TEXT, "
            "instance_start INTEGER, "
            "type INTEGER, "
            "title TEXT NOT NULL, "
            "description TEXT, "
            "location TEXT, "
            "timezone TEXT, "
            "start_datetime INTEGER NOT NULL, "
            "end_datetime INTEGER NOT NULL, "
            "all_day INTEGER DEFAULT 0, "
            "status TEXT DEFAULT 'confirmed', "
            "recurrence_rule TEXT, "
            "deleted_at INTEGER, "
            "sync_status INTEGER DEFAULT " + std::to_string(PENDING_INSERT) + ", "
            "created_at INTEGER NOT NULL, "
            "updated_at INTEGER, "
            "FOREIGN KEY(calendar_id) REFERENCES calendars(id) ON DELETE CASCADE, "
            "UNIQUE (calendar_id, provider_event_id)"
            ")");

    db.exec("CREATE INDEX IF NOT EXISTS idx_calendars_account "
            "ON calendars(account_id)");

    db.exec("CREATE INDEX IF NOT EXISTS idx_calendars_account_sync "
            "ON calendars(account_id, sync_status, deleted_at)");

    db.exec("CREATE TABLE IF NOT EXISTS calendar_sync_ranges ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT, "
            "calendar_id INTEGER NOT NULL, "
            "start_epoch INTEGER NOT NULL, "
            "end_epoch INTEGER NOT NULL, "
            "synced_at INTEGER NOT NULL, "
            "last_viewed_at INTEGER DEFAULT 0, "
            "sync_token TEXT DEFAULT '', "
            "FOREIGN KEY(calendar_id) REFERENCES calendars(id) ON DELETE CASCADE"
            ")");

    db.exec("CREATE UNIQUE INDEX IF NOT EXISTS idx_calendar_sync_ranges_exact "
            "ON calendar_sync_ranges(calendar_id, start_epoch, end_epoch)");

    db.exec("CREATE INDEX IF NOT EXISTS idx_calendar_sync_ranges_calendar_range "
            "ON calendar_sync_ranges(calendar_id, start_epoch, end_epoch)");

    db.exec("CREATE INDEX IF NOT EXISTS idx_events_calendar "
            "ON events(calendar_id)");

    db.exec("CREATE INDEX IF NOT EXISTS idx_events_start "
            "ON events(start_datetime)");

    db.exec("CREATE INDEX IF NOT EXISTS idx_events_end "
            "ON events(end_datetime)");

    db.exec("CREATE INDEX IF NOT EXISTS idx_events_provider "
            "ON events(provider_event_id)");

    db.exec("CREATE INDEX IF NOT EXISTS idx_events_sync_status "
            "ON events(sync_status)");

    db.exec("CREATE INDEX IF NOT EXISTS idx_events_calendar_range "
            "ON events(calendar_id, start_datetime, end_datetime)");

    db.exec("CREATE INDEX IF NOT EXISTS idx_events_calendar_recurrence_start "
            "ON events(calendar_id, start_datetime, recurrence_rule)");

    db.exec("CREATE INDEX IF NOT EXISTS idx_events_calendar_sync "
            "ON events(calendar_id, sync_status)");

    db.exec("CREATE INDEX IF NOT EXISTS idx_events_calendar_sync_updated "
            "ON events(calendar_id, sync_status, updated_at, id)");

    db.exec("CREATE INDEX IF NOT EXISTS idx_events_calendar_provider "
            "ON events(calendar_id, provider_event_id)");

    db.exec("CREATE INDEX IF NOT EXISTS idx_events_calendar_master_instance "
            "ON events(calendar_id, provider_master_id, instance_start)");

    db.exec("CREATE TABLE IF NOT EXISTS recurrence_overrides ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT, "
            "master_event_id INTEGER NOT NULL, "
            "original_start INTEGER NOT NULL, "
            "type INTEGER NOT NULL, "
            "replacement_event_id INTEGER, "
            "sync_status INTEGER DEFAULT " + std::to_string(PENDING_INSERT) + ", "
            "deleted_at INTEGER DEFAULT 0, "
            "created_at INTEGER NOT NULL, "
            "updated_at INTEGER, "
            "FOREIGN KEY(master_event_id) REFERENCES events(id) ON DELETE CASCADE, "
            "FOREIGN KEY(replacement_event_id) REFERENCES events(id) ON DELETE SET NULL, "
            "UNIQUE(master_event_id, original_start)"
            ")");

    db.exec("CREATE INDEX IF NOT EXISTS idx_recurrence_overrides_master "
            "ON recurrence_overrides(master_event_id, original_start)");

    db.exec("CREATE INDEX IF NOT EXISTS idx_recurrence_overrides_master_sync "
            "ON recurrence_overrides(master_event_id, sync_status, deleted_at)");

    db.exec("CREATE INDEX IF NOT EXISTS idx_recurrence_overrides_sync "
            "ON recurrence_overrides(sync_status)");

    return true;
}

void ResetApplicationData(SQLite::Database& db) {
    db.exec("BEGIN IMMEDIATE TRANSACTION");
    try {
        db.exec("DELETE FROM recurrence_overrides");
        db.exec("DELETE FROM events");
        db.exec("DELETE FROM calendar_sync_ranges");
        db.exec("DELETE FROM calendars");
        db.exec("DELETE FROM accounts");
        db.exec("DELETE FROM sqlite_sequence WHERE name IN ('events', 'recurrence_overrides', 'calendar_sync_ranges', 'calendars', 'accounts')");
        db.exec("COMMIT");
    }
    catch (...) {
        db.exec("ROLLBACK");
        throw;
    }
}

void EnsureLocalSeedData(SQLite::Database& db) {
    AccountRepository accountRepository(db);
    CalendarRepository calendarRepository(db);

    Account localAccount{};
    localAccount.name = "Local account";
    localAccount.provider = "LOCAL";
    localAccount.providerUserId = "local-account";
    localAccount.email.clear();
    localAccount.refreshToken.clear();
    const long long localAccountId = accountRepository.Upsert(localAccount);

    Calendar localCalendar{};
    localCalendar.accountId = localAccountId;
    localCalendar.providerCalendarId = "local-calendar";
    localCalendar.name = "Local Calendar";
    localCalendar.description = "Local-only calendar";
    localCalendar.timezone = "UTC";
    localCalendar.colorHex = "#1A73E8";
    localCalendar.isPrimary = true;
    localCalendar.isReadOnly = false;
    localCalendar.syncEnabled = false;
    localCalendar.syncStatus = SYNCED;
    calendarRepository.upsert(localCalendar);

    SQLite::Statement primaryOnly(
        db,
        "UPDATE calendars SET is_primary = CASE WHEN provider_calendar_id = ? THEN 1 ELSE 0 END WHERE account_id = ?");
    primaryOnly.bind(1, "local-calendar");
    BindInt64(primaryOnly, 2, localAccountId);
    primaryOnly.exec();
}

} // namespace

int main() {


    const std::string dbPath = GetApplicationDatabasePath().string();

    try {
        SQLite::Database db(dbPath, SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);
        ConfigureSqliteConnection(db);
        InitializeSchema(db);

        //ResetApplicationData(db);
        EnsureLocalSeedData(db);
    }
    catch (const SQLite::Exception& ex) {
        std::cerr << "Database initialization failed: " << ex.what() << "\n";
        return 1;
    }

    return RunLocalCalendarUi(dbPath);
}
