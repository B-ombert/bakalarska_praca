#pragma once

#include <functional>
#include <set>
#include <vector>

#include <wx/dialog.h>

#include "models/account.h"

class AccountManagerDialog final : public wxDialog {
public:
    struct AccountState {
        Account account;
        bool signedIn = false;
        bool loginFailed = false;
    };

    using AccountCallback = std::function<void(long long)>;
    using SimpleCallback = std::function<void()>;

    AccountManagerDialog(wxWindow* parent,
                         const std::vector<AccountState>& accounts,
                         SimpleCallback onAddAccount,
                         AccountCallback onLoginAccount,
                         AccountCallback onRemoveAccount);

private:
    void Build(const std::vector<AccountState>& accounts);

    SimpleCallback onAddAccount_;
    AccountCallback onLoginAccount_;
    AccountCallback onRemoveAccount_;
};
