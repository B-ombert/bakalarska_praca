#pragma once
#include <string>
#include "crypto.h"
#include "oauth_utils.h"

std::string GetGoogleAuthCode(const std::string& code_challenge);
std::string GetAccessTokenFromGoogle();
