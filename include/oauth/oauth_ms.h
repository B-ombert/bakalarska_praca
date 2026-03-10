#pragma once
#include <string>
#include "oauth_utils.h"



std::string GetMSAuthCode(const std::string& code_challenge);
std::string GetAccessTokenFromMS();
