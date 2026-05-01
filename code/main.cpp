#include <iostream>

#include "SQLiteCpp/Backup.h"
#include "ui/local_calendar_app.h"
#include "utils/access_token.h"
#include "utils/types.h"

namespace {

bool InitializeSchema(SQLite::Database& db) {
    db.exec("CREATE TABLE IF NOT EXISTS accounts ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT, "
            "name TEXT NOT NULL, "
            "provider TEXT NOT NULL, "
            "provider_user_id TEXT NOT NULL, "
            "refresh_token TEXT NOT NULL, "
            "UNIQUE (provider, provider_user_id)"
            ")");

    db.exec("CREATE INDEX IF NOT EXISTS idx_accounts_provider "
            "ON accounts(provider, provider_user_id)");

    db.exec("CREATE TABLE IF NOT EXISTS calendars ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT, "
            "account_id INTEGER, "
            "provider_calendar_id TEXT, "
            "name TEXT, "
            "timezone TEXT NOT NULL, "
            "is_primary INTEGER DEFAULT 0, "
            "is_read_only INTEGER DEFAULT 0, "
            "sync_enabled INTEGER DEFAULT 1, "
            "sync_token TEXT, "
            "last_synced_at INTEGER, "
            "FOREIGN KEY(account_id) REFERENCES accounts(id), "
            "UNIQUE (account_id, provider_calendar_id)"
            ")");

    db.exec("CREATE INDEX IF NOT EXISTS idx_calendars_account "
            "ON calendars(account_id)");

    db.exec("CREATE TABLE IF NOT EXISTS events ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT, "
            "calendar_id INTEGER NOT NULL, "
            "provider_event_id TEXT, "
            "provider_master_id TEXT, "
            "instance_start INTEGER, "
            "type INTEGER, "
            "title TEXT NOT NULL, "
            "description TEXT, "
            "location TEXT, "
            "start_datetime INTEGER NOT NULL, "
            "end_datetime INTEGER NOT NULL, "
            "all_day INTEGER DEFAULT 0, "
            "status TEXT DEFAULT 'confirmed', "
            "recurrence_rule TEXT, "
            "deleted_at INTEGER, "
            "sync_status INTEGER DEFAULT " + std::to_string(PENDING_INSERT) + ", "
            "last_modified INTEGER, "
            "created_at INTEGER NOT NULL, "
            "updated_at INTEGER, "
            "FOREIGN KEY(calendar_id) REFERENCES calendars(id), "
            "UNIQUE (calendar_id, provider_event_id, instance_start)"
            ")");

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

    return true;
}

} // namespace

int main() {
        

    const std::string dbPath = APP_DB_PATH;

    try {
        SQLite::Database db(dbPath, SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);
        InitializeSchema(db);
    }
    catch (const SQLite::Exception& ex) {
        std::cerr << "Database initialization failed: " << ex.what() << "\n";
        return 1;
    }

    return RunLocalCalendarUi(dbPath);
}
