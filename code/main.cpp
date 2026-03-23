#include <iostream>

#include "events_ms.h"
#include "events_google.h"
#include "access_token.h"
#include "SQLiteCpp/Backup.h"


int main(){

    //std::cout << AccessToken(GOOGLE).ExpiresAt;

    try {
        SQLite::Database db("calendar_app.db", SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);

        db.exec("CREATE TABLE IF NOT EXISTS accounts ("
                "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                "provider TEXT NOT NULL,"
                "provider_user_id TEXT NOT NULL,"
                "access_token TEXT,"
                "refresh_token TEXT NOT NULL )"
            );

        db.exec("CREATE INDEX IF NOT EXISTS idx_accounts_provider"
                "ON accounts(provider, provider_user_id)");

        db.exec("CREATE TABLE IF NOT EXISTS calendars ("
                "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                ""
                "account_id INTEGER,"
                "provider_calendar_id TEXT,"
                ""
                "name TEXT,"
                "timezone TEXT NOT NULL,"
                ""
                "is_primary INTEGER DEFAULT 0,"
                "is_read_only INTEGER DEFAULT 0,"
                "sync_enabled INTEGER DEFAULT 1,"
                ""
                "sync_token TEXT,"
                "last_synced_at INTEGER,"
                ""
                "FOREIGN KEY(account_id) REFERENCES accounts(account_id) )"
            );

        db.exec("CREATE INDEX IF NOT EXISTS idx_calendars_account"
                "ON calendars(account_id)");

        db.exec("CREATE TABLE IF NOT EXISTS events ("
                "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                "calendar_id INTEGER NOT NULL,"
                ""
                "provider_event_id TEXT,"
                "provider_etag TEXT,"
                ""
                "title TEXT NOT NULL,"
                "description TEXT,"
                "location TEXT,"
                ""
                "start_datetime INTEGER NOT NULL,"
                "end_datetime INTEGER NOT NULL,"
                "all_day INTEGER DEFAULT 0,"
                ""
                "status TEXT DEFAULT 'confirmed',"
                ""
                "recurrence_rule TEXT,"
                "recurrence_id TEXT,"
                ""
                "deleted_at INTEGER,"
                ""
                "sync_status TEXT DEFAULT 'synced',"
                ""
                "last_modified INTEGER,"
                "created_at INTEGER NOT NULL,"
                "updated_at INTEGER,"
                "FOREIGN KEY(calendar_id) REFERENCES calendars(id) )"
            );

        db.exec("CREATE INDEX IF NOT EXISTS idx_events_calendar"
                "ON events(calendar_id)");

        db.exec("CREATE INDEX IF NOT EXISTS idx_events_start"
                "ON events(start_datetime)");

        db.exec("CREATE INDEX IF NOT EXISTS idx_events_end"
                "ON events(start_endtime)");

        db.exec("CREATE INDEX IF NOT EXISTS idx_events_provider"
                "ON events(provider_event_id)");

        db.exec("CREATE INDEX IF NOT EXISTS idx_events_sync_status"
                "ON events(sync_status)");

        db.exec("CREATE TABLE IF NOT EXISTS refresh_tokens ("
                "refresh_token TEXT, "
                "platform TEXT PRIMARY KEY )"
                );

        std::string MSrefresh_token;
        std::string Grefresh_token;

        SQLite::Statement selectStmt(
            db,
            "SELECT refresh_token FROM refresh_tokens WHERE platform = ?"
            );

        selectStmt.bind(1, "MICROSOFT");

        if (selectStmt.executeStep()) {
            MSrefresh_token = selectStmt.getColumn(0).getString();

            AccessToken token = AccessToken(MICROSOFT, MSrefresh_token);
            token.TestingFunction();
            auto time = std::chrono::system_clock::to_time_t(token.ExpiresAt);
            std::cout << std::ctime(&time);
        }

        else{
            AccessToken token = AccessToken(MICROSOFT);
            auto time = std::chrono::system_clock::to_time_t(token.ExpiresAt);
            std::cout << std::ctime(&time);

            SQLite::Statement insertStmt(db,
                "INSERT INTO refresh_tokens (refresh_token, platform) values (?,?)"
                );
            insertStmt.bind(1, token.GetRefreshToken());
            insertStmt.bind(2, "MICROSOFT");
            insertStmt.exec();
        }


        SQLite::Statement selectStmt2(db,
            "SELECT refresh_token FROM refresh_tokens WHERE platform = ?"
            );
        selectStmt2.bind(1, "GOOGLE");

        if (selectStmt2.executeStep()) {
            Grefresh_token = selectStmt2.getColumn(0).getString();

            AccessToken GoogleToken = AccessToken(GOOGLE, Grefresh_token);
            GoogleToken.TestingFunction();
            auto time = std::chrono::system_clock::to_time_t(GoogleToken.ExpiresAt);
            std::cout << std::ctime(&time);
        }
        else {
            AccessToken GoogleToken = AccessToken(GOOGLE, Grefresh_token);
            auto time = std::chrono::system_clock::to_time_t(GoogleToken.ExpiresAt);
            std::cout << std::ctime(&time);

            SQLite::Statement insertStmt2(db,
                "INSERT INTO refresh_tokens (refresh_token, platform) values (?,?)"
                );
            insertStmt2.bind(1, GoogleToken.GetRefreshToken());
            insertStmt2.bind(2, "GOOGLE");
            insertStmt2.exec();
        }

    }
    catch (SQLite::Exception& ex) {
        std::cerr << "Error: " << ex.what() << "\n";
    }

    //return GetGoogleCalendar();

    //return GetMSCalendar();

    /*CreateGoogleCalendarEvent("Meeting",
                              "Discuss further work",
                              "2025-12-05T11:30:00+01:00",
                              "2025-12-05T12:30:00+01:00");
                              */


    /*CreateMSCalendarEvent("Meeting",
                          "Discussing future work",
                          "2025-11-28T11:30:00",
                          "2025-11-28T12:00:00");
                          */

    //UploadEventsToGoogleCalendarBatch();

    /*
    int dni = 365;
    auto events = GetGoogleEventsLastNDays(dni);

    for(auto& e : events){
        std::cout << e.start << " - " << e.end << " : " << e.summary << "\n";
    }
     */

    /*
    try {

        SQLite::Database db("calendar_app.db", SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);
        db.exec("CREATE TABLE IF NOT EXISTS events ("
                "event_id INTEGER PRIMARY KEY, "
                "calendar_id INTEGER NOT NULL, "
                "event_summary TEXT, "
                "start_time TEXT, "
                "end_time TEXT)");

        db.exec("CREATE TABLE IF NOT EXISTS calendars ("
                "calendar_id INTEGER PRIMARY KEY, "
                "calendar_name TEXT, "
                "source TEXT,"
                "timezone TEXT)");

        {
            SQLite::Statement insert(db,
               "INSERT INTO events (calendar_id, event_summary, start_time, end_time) VALUES (1, ?, ?, ?)");
            SQLite::Transaction transaction(db);


            const int TOTAL_DAYS = 365;
            const int BATCH_DAYS = 5;

            for(int day=0; day<50; day++){
                auto events = GenerateEventsBatch(day, BATCH_DAYS);

                for(auto& e : events){
                    insert.bind(1, e.summary);
                    insert.bind(2, e.start);
                    insert.bind(3, e.end);

                    insert.exec();
                    insert.reset();
                }
            }

            transaction.commit();
        }


        {
            SQLite::Statement count(db, "SELECT COUNT(*) FROM events");
            count.executeStep();
            int total = count.getColumn(0).getInt();
            std::cout << "Total entries: " << total << "\n";

            std::cout << "First 10 entries: \n\n";
        }

        {
            SQLite::Statement firstTen(db, "SELECT event_id, calendar_id, event_summary, start_time, end_time"
                                          " FROM events ORDER BY event_id ASC LIMIT 10");

            while (firstTen.executeStep()) {
                std::cout
                << firstTen.getColumn(0) << " | "
                << firstTen.getColumn(1) << " | "
                << firstTen.getColumn(2).getString() << " | "
                << firstTen.getColumn(3).getString() << " | "
                << firstTen.getColumn(4).getString() << "\n";
            }
        }

        std::cout << "\n";

        std::cout << "Last 10 entries: \n\n";

        {
            SQLite::Statement lastTen(db, "SELECT event_id, calendar_id, event_summary, start_time, end_time"
                                         " FROM events ORDER BY event_id DESC LIMIT 10");

            while (lastTen.executeStep()) {
                std::cout
                << lastTen.getColumn(0) << " | "
                << lastTen.getColumn(1) << " | "
                << lastTen.getColumn(2).getString() << " | "
                << lastTen.getColumn(3).getString() << " | "
                << lastTen.getColumn(4).getString() << "\n";
            }
        }

        {
            SQLite::Transaction transaction(db);
            db.exec("DELETE FROM events");
            db.exec("DROP TABLE IF EXISTS events");
            transaction.commit();
        }

    }
    catch (SQLite::Exception& ex) {
        std::cerr << "Error: " << ex.what() << "\n";
    }
    */


    return 0;
}
