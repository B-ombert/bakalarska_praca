#include "ui/account_manager_dialog.h"

#include <utility>

#include <wx/button.h>
#include <wx/panel.h>
#include <wx/scrolwin.h>
#include <wx/sizer.h>
#include <wx/stattext.h>

namespace {

constexpr const char* kLocalProvider = "LOCAL";

wxString ProviderLabel(const std::string& provider) {
    if (provider == "GOOGLE") {
        return "Google";
    }
    if (provider == "MICROSOFT") {
        return "Outlook";
    }
    if (provider == kLocalProvider) {
        return "Local";
    }
    return wxString::FromUTF8(provider);
}

wxString AccountTitle(const Account& account) {
    if (account.provider == kLocalProvider) {
        return "Local account";
    }
    if (!account.name.empty()) {
        return wxString::FromUTF8(account.name);
    }
    if (!account.email.empty()) {
        return wxString::FromUTF8(account.email);
    }
    return ProviderLabel(account.provider) + " account";
}

wxString AccountEmail(const Account& account) {
    if (!account.email.empty()) {
        return wxString::FromUTF8(account.email);
    }
    if (!account.providerUserId.empty()) {
        return wxString::FromUTF8(account.providerUserId);
    }
    return "No email stored";
}

} // namespace

AccountManagerDialog::AccountManagerDialog(wxWindow* parent,
                                           const std::vector<AccountState>& accounts,
                                           SimpleCallback onAddAccount,
                                           AccountCallback onLoginAccount,
                                           AccountCallback onRemoveAccount)
    : wxDialog(parent,
               wxID_ANY,
               "Manage accounts",
               wxDefaultPosition,
               wxSize(520, 460),
               wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER),
      onAddAccount_(std::move(onAddAccount)),
      onLoginAccount_(std::move(onLoginAccount)),
      onRemoveAccount_(std::move(onRemoveAccount)) {
    Build(accounts);
    CentreOnParent();
}

void AccountManagerDialog::Build(const std::vector<AccountState>& accounts) {
    auto* root = new wxBoxSizer(wxVERTICAL);

    auto* headerSizer = new wxBoxSizer(wxHORIZONTAL);
    auto* title = new wxStaticText(this, wxID_ANY, "Accounts");
    wxFont titleFont = title->GetFont();
    titleFont.SetPointSize(titleFont.GetPointSize() + 2);
    titleFont.SetWeight(wxFONTWEIGHT_BOLD);
    title->SetFont(titleFont);

    auto* addButton = new wxButton(this, wxID_ANY, "Add account");
    addButton->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (onAddAccount_) {
            EndModal(wxID_OK);
            onAddAccount_();
        }
    });

    headerSizer->Add(title, 1, wxALIGN_CENTER_VERTICAL);
    headerSizer->Add(addButton, 0, wxALIGN_CENTER_VERTICAL);
    root->Add(headerSizer, 0, wxEXPAND | wxALL, 12);

    auto* scroller = new wxScrolledWindow(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxVSCROLL | wxBORDER_NONE);
    scroller->SetScrollRate(0, 8);
    auto* listSizer = new wxBoxSizer(wxVERTICAL);

    for (const auto& state : accounts) {
        const Account account = state.account;
        auto* card = new wxPanel(scroller, wxID_ANY);
        card->SetBackgroundColour(wxColour(255, 255, 255));
        auto* cardSizer = new wxBoxSizer(wxVERTICAL);

        auto* row = new wxBoxSizer(wxHORIZONTAL);
        auto* textSizer = new wxBoxSizer(wxVERTICAL);
        auto* name = new wxStaticText(card, wxID_ANY, AccountTitle(account));
        wxFont nameFont = name->GetFont();
        nameFont.SetWeight(wxFONTWEIGHT_BOLD);
        name->SetFont(nameFont);
        textSizer->Add(name, 0, wxBOTTOM, 2);

        if (account.provider != kLocalProvider) {
            textSizer->Add(new wxStaticText(card, wxID_ANY, AccountEmail(account)), 0, wxBOTTOM, 2);
        }
        textSizer->Add(new wxStaticText(card, wxID_ANY, wxString("Platform: ") + ProviderLabel(account.provider)), 0);
        row->Add(textSizer, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, 10);

        if (account.provider == kLocalProvider) {
            auto* localStatus = new wxStaticText(card, wxID_ANY, "Always available");
            localStatus->SetForegroundColour(wxColour(34, 128, 70));
            row->Add(localStatus, 0, wxALIGN_CENTER_VERTICAL);
        }
        else if (state.signedIn) {
            auto* signedStatus = new wxStaticText(card, wxID_ANY, "Signed in");
            signedStatus->SetForegroundColour(wxColour(34, 128, 70));
            row->Add(signedStatus, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
        }
        else if (state.signingIn) {
            auto* signingInStatus = new wxStaticText(card, wxID_ANY, "Signing in...");
            signingInStatus->SetForegroundColour(wxColour(80, 100, 140));
            row->Add(signingInStatus, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
        }
        else {
            auto* loginButton = new wxButton(card, wxID_ANY, "Log in");
            loginButton->SetForegroundColour(wxColour(180, 40, 40));
            loginButton->Bind(wxEVT_BUTTON, [this, accountId = account.id](wxCommandEvent&) {
                if (onLoginAccount_) {
                    EndModal(wxID_OK);
                    onLoginAccount_(accountId);
                }
            });
            row->Add(loginButton, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
        }

        if (account.provider != kLocalProvider) {
            auto* removeButton = new wxButton(card, wxID_ANY, "Remove");
            removeButton->Bind(wxEVT_BUTTON, [this, accountId = account.id](wxCommandEvent&) {
                if (onRemoveAccount_) {
                    EndModal(wxID_OK);
                    onRemoveAccount_(accountId);
                }
            });
            row->Add(removeButton, 0, wxALIGN_CENTER_VERTICAL);
        }

        cardSizer->Add(row, 0, wxEXPAND | wxALL, 10);
        card->SetSizer(cardSizer);
        listSizer->Add(card, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);
    }

    scroller->SetSizer(listSizer);
    root->Add(scroller, 1, wxEXPAND | wxLEFT | wxRIGHT, 12);

    auto* closeButton = new wxButton(this, wxID_CANCEL, "Close");
    root->Add(closeButton, 0, wxALIGN_RIGHT | wxALL, 12);
    SetSizer(root);
}
