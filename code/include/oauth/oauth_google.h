#pragma once
#include <string>

std::string GetGoogleAuthCode(const std::string& code_challenge);
std::string GetAccessTokenFromGoogle();
