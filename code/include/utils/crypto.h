#pragma once
#include <string>

std::string GenerateCodeVerifier();
std::string GenerateCodeChallenge(const std::string& verifier);
std::string Base64UrlEncode(const unsigned char* data, size_t len);
