#pragma once
#include <string>

std::string GenerateCodeVerifier();
std::string GenerateCodeChallenge(const std::string& verifier);
