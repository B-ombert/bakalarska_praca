#pragma once
#include <string>
#include "utils/crypto.h"
#include "oauth_utils.h"

std::string GetGoogleAuthCode(const std::string& code_challenge);
std::string GetAccessTokenFromGoogle();
