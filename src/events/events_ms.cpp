#include "events_ms.h"

std::string BuildMSEventJson(const simpleEvent& event) {
    std::ostringstream json;
    json << "{"
         << "\"subject\": \"" << event.summary << "\","
         << "\"start\": {"
         << "    \"dateTime\": \"" << event.start << "\","
         << "    \"timeZone\": \"Europe/Bratislava\""
         << "},"
         << "\"end\": {"
         << "    \"dateTime\": \"" << event.end << "\","
         << "    \"timeZone\": \"Europe/Bratislava\""
         << "}"
         << "}";
    return json.str();
}

void GetMSCalendarEvents(const std::string &access_token) {
    HttpRequest request;
    request.url = "https://graph.microsoft.com/v1.0/me/events";
    request.headers = {"Authorization: Bearer " + access_token, "Accept: application/json"};

    std::string resp = PerformHttpRequest(request);

    if (!resp.empty()) {
        std::cout << "\nEvents:\n" << resp << "\n";
    }
    else {
        std::cerr << "Error reading Microsoft events events";
    }
}

int GetMSCalendar(){
    std::string access_token = GetAccessTokenFromMS();

    if (!access_token.empty()){
        std::cout << "\nAccess token received\n";
        GetMSCalendarEvents(access_token);
    }
    else{
        std::cerr << "Couldn't get access token";
    }

    return 0;
}

void CreateMSCalendarEvent(const simpleEvent& event) {
    std::string access_token = GetAccessTokenFromMS();
    std::string jsonBody = BuildMSEventJson(event);

    HttpRequest request;
    request.url = "https://graph.microsoft.com/v1.0/me/events";
    request.headers = {"Authorization: Bearer " + access_token,
        "Content-Type: application/json"};
    request.postData = jsonBody;

    std::string resp = PerformHttpRequest(request);
    if (!resp.empty()) {
        std::cout << "\nEvent create response:\n" << resp << std::endl;
    }
    else {
        std::cerr << "Error creating events event\n";
    }
}