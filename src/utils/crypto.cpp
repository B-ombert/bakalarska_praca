#include "crypto.h"
#include <openssl/sha.h>
#include <random>
#include <string>

std::string Base64UrlEncode(const unsigned char* data, size_t len){
    static const char* chars =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string encoded;

    int val = 0, valb = -6;
    for(size_t i=0; i<len; i++){
        val = (val<<8) + data[i];
        valb+=8;
        while (valb >= 0){
            encoded.push_back(chars[(val >> valb) & 0x3F]);
            valb-=6;
        }
    }

    if (valb > -6){
        encoded.push_back(chars[((val << 8) >> (valb + 8)) & 0x3F]);
    }

    while (!encoded.empty() && encoded.back() == '=') {
        encoded.pop_back();
    }

    return encoded;
}

std::string GenerateCodeVerifier(){
    static const char charset[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                                   "abcdefghijklmnopqrstuvwxyz"
                                   "0123456789-._~";

    std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<> dist(0, sizeof(charset) - 2);

    std::string verifier;
    for (int i=0; i<64; i++){
        verifier += charset[(dist(rng))];
    }
    return verifier;
}

std::string GenerateCodeChallenge(const std::string& verifier){
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256((const unsigned char*)verifier.c_str(), verifier.size(), hash);
    return Base64UrlEncode(hash, SHA256_DIGEST_LENGTH);
}