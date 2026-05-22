#pragma once

#include <string>

struct Account {
    long long id = 0;
    std::string name;

    std::string provider;
    std::string providerUserId;
    std::string email;

    std::string refreshToken;
};
