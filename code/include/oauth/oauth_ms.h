#pragma once
#include <string>

std::string GetMSAuthCode(const std::string& code_challenge);
std::string GetAccessTokenFromMS();
