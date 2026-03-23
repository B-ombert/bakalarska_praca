#include "events_google.h"

std::string BuildGoogleEventJson(const simpleEvent& event) {
    std::ostringstream json;
    json << "{"
         << "\"summary\": \"" << event.summary << "\","
         << "\"start\": {\"dateTime\": \"" << event.start << "\"},"
         << "\"end\":   {\"dateTime\": \"" << event.end << "\"}"
         << "}";
    return json.str();
}

void GetGoogleCalendarEvents(const std::string& access_token) {
    std::string url = "https://www.googleapis.com/calendar/v3/calendars/primary/events";
    HttpRequest req;
    req.url = url;
    req.headers = {"Authorization: Bearer " + access_token};

    std::string resp = PerformHttpRequest(req);

    if (!resp.empty()) {
        std::cout << "\nEvents:\n" << resp << "\n";
    }
}

int GetGoogleCalendar(){
    std::string access_token = GetAccessTokenFromGoogle();

    if(!access_token.empty()){
        std::cout << "\n Access token received";
        GetGoogleCalendarEvents(access_token);
    }
    else{
        std::cerr << "Coudln't get access token";
    }

    return 0;
}

void CreateGoogleCalendarEvent(const simpleEvent& event) {
    std::string access_token = GetAccessTokenFromGoogle();
    std::string jsonBody = BuildGoogleEventJson(event);

    HttpRequest req;
    req.url = "https://www.googleapis.com/calendar/v3/calendars/primary/events";
    req.headers = {"Authorization: Bearer " + access_token,
        "Content-Type: application/json"};
    req.postData = jsonBody;

    std::string resp = PerformHttpRequest(req);
    if (!resp.empty()) {
        std::cout << "\nEvent create response:\n" << resp << "\n";
    }
    else {
        std::cerr << "Error with creating event";
    }
}

std::vector<simpleEvent> GetGoogleEventsLastNDays(int days){
    return std::vector<simpleEvent>();
}