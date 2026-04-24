#pragma once

#include <optional>

#include "models/account.h"
#include "utils/access_token.h"

std::optional<Account> GetAccountUserInfo(AccessToken& token);
