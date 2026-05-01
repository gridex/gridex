// ChatGPTOAuthService.cpp
// Full ChatGPT OAuth lifecycle: sign-in (PKCE + loopback listener),
// token refresh (coalesced), DPAPI storage, sign-out.

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <shlobj.h>
#include <shellapi.h>
#include <wincrypt.h>
#include <winhttp.h>
#pragma comment(lib, "Crypt32.lib")
#pragma comment(lib, "Shell32.lib")
#pragma comment(lib, "Winhttp.lib")

#pragma warning(push)
#pragma warning(disable: 4996)
#define CPPHTTPLIB_OPENSSL_SUPPORT
#include <httplib.h>
#pragma warning(pop)

#include <nlohmann/json.hpp>

#include "ChatGPTOAuthService.h"
#include "ChatGPTOAuthConstants.h"
#include "PKCE.h"
#include "JwtDecoder.h"

namespace {
    struct HttpResult {
        int status = 0;          // 0 = transport-level failure
        std::string body;
    };

    // POST a body to https://<host><path>. WinHTTP-based — replaces an
    // earlier httplib::SSLClient path that crashed on SSL_shutdown
    // (vcpkg httplib 0.40.0 vs OpenSSL 3.x ABI / threading mismatch).
    // Native Windows HTTPS sidesteps the whole question — no extra
    // dependency, no per-thread OpenSSL state to leak.
    HttpResult HttpsPost(const std::wstring& host,
                         const std::wstring& path,
                         const std::string& body,
                         const std::wstring& contentType)
    {
        HttpResult out;
        HINTERNET hSession = ::WinHttpOpen(
            L"Gridex/ChatGPTOAuth",
            WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!hSession) return out;

        HINTERNET hConnect = ::WinHttpConnect(
            hSession, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
        if (!hConnect) { ::WinHttpCloseHandle(hSession); return out; }

        HINTERNET hReq = ::WinHttpOpenRequest(
            hConnect, L"POST", path.c_str(), nullptr,
            WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
            WINHTTP_FLAG_SECURE);
        if (!hReq) {
            ::WinHttpCloseHandle(hConnect);
            ::WinHttpCloseHandle(hSession);
            return out;
        }

        std::wstring headers = L"Content-Type: " + contentType;
        BOOL ok = ::WinHttpSendRequest(
            hReq, headers.c_str(), (DWORD)-1L,
            (LPVOID)body.data(), (DWORD)body.size(),
            (DWORD)body.size(), 0);
        if (ok) ok = ::WinHttpReceiveResponse(hReq, nullptr);

        if (ok)
        {
            DWORD statusCode = 0, statusLen = sizeof(statusCode);
            ::WinHttpQueryHeaders(hReq,
                WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusLen,
                WINHTTP_NO_HEADER_INDEX);
            out.status = static_cast<int>(statusCode);

            DWORD avail = 0;
            while (::WinHttpQueryDataAvailable(hReq, &avail) && avail > 0)
            {
                std::string chunk(avail, '\0');
                DWORD read = 0;
                if (!::WinHttpReadData(hReq, chunk.data(), avail, &read)) break;
                chunk.resize(read);
                out.body.append(chunk);
            }
        }

        ::WinHttpCloseHandle(hReq);
        ::WinHttpCloseHandle(hConnect);
        ::WinHttpCloseHandle(hSession);
        return out;
    }

    std::wstring Utf8ToWide(const std::string& s)
    {
        if (s.empty()) return {};
        int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
        std::wstring w(n, L'\0');
        ::MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
        return w;
    }

    // RFC 3986 percent-encoder for query / form-body values. Anything
    // outside the unreserved set turns into %XX. Required because the
    // scope string contains spaces and the redirect_uri contains
    // reserved chars (:, /) that the OAuth server rejects raw.
    std::string PercentEncode(const std::string& s)
    {
        std::string out;
        out.reserve(s.size() * 3);
        auto isUnreserved = [](unsigned char c) {
            return (c >= 'A' && c <= 'Z') ||
                   (c >= 'a' && c <= 'z') ||
                   (c >= '0' && c <= '9') ||
                   c == '-' || c == '_' || c == '.' || c == '~';
        };
        for (unsigned char c : s)
        {
            if (isUnreserved(c)) out.push_back(static_cast<char>(c));
            else
            {
                char buf[4];
                snprintf(buf, sizeof(buf), "%%%02X", c);
                out.append(buf);
            }
        }
        return out;
    }
}

#include <thread>
#include <chrono>
#include <stdexcept>
#include <filesystem>
#include <fstream>

namespace ChatGPT
{
    // ── Singleton ─────────────────────────────────────────────────────────────

    OAuthService& OAuthService::Instance()
    {
        static OAuthService instance;
        return instance;
    }

    // ── Helpers: UTF-8 ↔ wstring ─────────────────────────────────────────────

    static std::string toUtf8(const std::wstring& w)
    {
        if (w.empty()) return {};
        int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
        std::string s(n - 1, '\0');
        WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
        return s;
    }

    static std::wstring fromUtf8(const std::string& s)
    {
        if (s.empty()) return {};
        int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
        std::wstring w(n - 1, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
        return w;
    }

    // ── DPAPI helpers ─────────────────────────────────────────────────────────

    std::wstring OAuthService::TokenFilePath()
    {
        wchar_t* appData = nullptr;
        SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appData);
        std::wstring path = appData ? std::wstring(appData) + L"\\Gridex" : L".";
        CoTaskMemFree(appData);
        std::filesystem::create_directories(path);
        return path + L"\\chatgpt-tokens.bin";
    }

    bool OAuthService::SaveBundle(const DBModels::ChatGPTTokenBundle& bundle)
    {
        std::string json = bundle.toJson();

        DATA_BLOB input;
        input.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(json.data()));
        input.cbData = static_cast<DWORD>(json.size());

        DATA_BLOB output = {};
        if (!CryptProtectData(&input, nullptr, nullptr, nullptr, nullptr, 0, &output))
            return false;

        auto path = TokenFilePath();
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        if (!f) { LocalFree(output.pbData); return false; }
        f.write(reinterpret_cast<char*>(output.pbData), output.cbData);
        LocalFree(output.pbData);
        return true;
    }

    DBModels::ChatGPTTokenBundle OAuthService::LoadBundle()
    {
        auto path = TokenFilePath();
        std::ifstream f(path, std::ios::binary);
        if (!f) return {};

        std::vector<BYTE> enc((std::istreambuf_iterator<char>(f)),
                               std::istreambuf_iterator<char>());
        if (enc.empty()) return {};

        DATA_BLOB input;
        input.pbData = enc.data();
        input.cbData = static_cast<DWORD>(enc.size());

        DATA_BLOB output = {};
        if (!CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr, 0, &output))
            return {};

        std::string json(reinterpret_cast<char*>(output.pbData), output.cbData);
        LocalFree(output.pbData);
        return DBModels::ChatGPTTokenBundle::fromJson(json);
    }

    void OAuthService::DeleteBundle()
    {
        auto path = TokenFilePath();
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }

    // ── JWT claims helper ─────────────────────────────────────────────────────

    void OAuthService::ApplyClaims(DBModels::ChatGPTTokenBundle& bundle)
    {
        if (bundle.idToken.empty()) return;
        try
        {
            auto claims = JwtDecodePayload(bundle.idToken);
            if (claims.contains("email") && claims["email"].is_string())
                bundle.email = claims["email"].get<std::string>();
            if (claims.contains("chatgpt_account_id") && claims["chatgpt_account_id"].is_string())
                bundle.accountId = claims["chatgpt_account_id"].get<std::string>();
            if (claims.contains("chatgpt_plan_type") && claims["chatgpt_plan_type"].is_string())
                bundle.planType = claims["chatgpt_plan_type"].get<std::string>();
        }
        catch (...) {}
    }

    // ── Token refresh ─────────────────────────────────────────────────────────

    DBModels::ChatGPTTokenBundle OAuthService::RefreshBundle(
        const DBModels::ChatGPTTokenBundle& current)
    {
        // OAuth token endpoint takes form-urlencoded — switched from
        // JSON because some upstream OIDC implementations reject JSON.
        std::string formBody =
            "client_id="     + PercentEncode(kClientId) +
            "&grant_type=refresh_token"
            "&refresh_token=" + PercentEncode(current.refreshToken);

        auto res = HttpsPost(Utf8ToWide(kIssuerHost), L"/oauth/token",
                             formBody, L"application/x-www-form-urlencoded");
        if (res.status < 200 || res.status >= 300) return {};

        try
        {
            auto j = nlohmann::json::parse(res.body);
            DBModels::ChatGPTTokenBundle updated = current;
            if (j.contains("access_token") && j["access_token"].is_string())
                updated.accessToken = j["access_token"].get<std::string>();
            // Server may rotate refresh_token and id_token
            if (j.contains("refresh_token") && j["refresh_token"].is_string())
            {
                auto rt = j["refresh_token"].get<std::string>();
                if (!rt.empty()) updated.refreshToken = rt;
            }
            if (j.contains("id_token") && j["id_token"].is_string())
            {
                auto idt = j["id_token"].get<std::string>();
                if (!idt.empty()) { updated.idToken = idt; ApplyClaims(updated); }
            }
            updated.obtainedAtUnix = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            return updated;
        }
        catch (...) { return {}; }
    }

    // ── BearerToken — public, coalesced refresh ────────────────────────────────

    std::string OAuthService::BearerToken()
    {
        auto bundle = LoadBundle();
        if (bundle.empty()) return {};

        // Check expiry with skew window
        int64_t exp = JwtExpiration(bundle.accessToken);
        int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();

        if (exp > 0 && now < exp - kRefreshSkewSeconds)
            return bundle.accessToken;

        // Coalesce concurrent refresh calls
        {
            std::lock_guard<std::mutex> lock(refreshMutex_);
            if (refreshFuture_.valid())
                return refreshFuture_.get();

            auto promise = std::make_shared<std::promise<std::string>>();
            refreshFuture_ = promise->get_future().share();

            std::thread([bundle, promise, this]() mutable {
                auto updated = RefreshBundle(bundle);
                if (updated.empty())
                {
                    // Refresh failed — do not wipe tokens, return old (may be expired)
                    promise->set_value(bundle.accessToken);
                }
                else
                {
                    SaveBundle(updated);
                    promise->set_value(updated.accessToken);
                }
                std::lock_guard<std::mutex> lk(refreshMutex_);
                refreshFuture_ = {};
            }).detach();
        }

        return refreshFuture_.get();
    }

    // ── IsSignedIn / CurrentEmail ─────────────────────────────────────────────

    bool OAuthService::IsSignedIn()
    {
        return !LoadBundle().empty();
    }

    std::wstring OAuthService::CurrentEmail()
    {
        auto b = LoadBundle();
        return fromUtf8(b.email);
    }

    std::string OAuthService::AccountId()
    {
        return LoadBundle().accountId;
    }

    // ── SignOut ───────────────────────────────────────────────────────────────

    void OAuthService::SignOut()
    {
        DeleteBundle();
    }

    // ── SignIn — loopback listener + PKCE exchange ─────────────────────────────

    void OAuthService::SignIn(SignInCallback callback)
    {
        std::thread([callback]() {
            try
            {
                auto verifier  = PKCEMakeVerifier();
                auto challenge = PKCEChallenge(verifier);
                auto state     = PKCEMakeState();
                // The Codex CLI client_id is registered for the
                // `localhost` host, not `127.0.0.1`. Using the literal
                // IP yields OpenAI's generic Authentication Error
                // ("unknown_error") with no further detail.
                std::string redirectURI =
                    std::string("http://localhost:") + std::to_string(kCallbackPort) + kCallbackPath;

                // Build the authorize URL — every value is percent-
                // encoded since the scope contains spaces and the
                // redirect URI contains reserved chars (:, /).
                std::string authURL =
                    std::string(kIssuer) + "/oauth/authorize"
                    "?response_type=code"
                    "&client_id="              + PercentEncode(kClientId) +
                    "&redirect_uri="           + PercentEncode(redirectURI) +
                    "&scope="                  + PercentEncode(kScopes) +
                    "&code_challenge="         + PercentEncode(challenge) +
                    "&code_challenge_method=S256"
                    "&state="                  + PercentEncode(state) +
                    "&id_token_add_organizations=true"
                    "&codex_cli_simplified_flow=true"
                    "&originator="             + PercentEncode(kOriginator);

                // Open the system browser
                std::wstring wUrl = fromUtf8(authURL);
                ShellExecuteW(nullptr, L"open", wUrl.c_str(), nullptr, nullptr, SW_SHOWNORMAL);

                // Single-shot loopback HTTP listener on 127.0.0.1:1455
                // Uses cpp-httplib in server mode (already available in the project).
                std::string receivedCode;
                std::string receivedState;
                bool        gotCallback = false;

                httplib::Server svr;

                svr.Get(kCallbackPath, [&](const httplib::Request& req,
                                           httplib::Response& res)
                {
                    receivedCode  = req.get_param_value("code");
                    receivedState = req.get_param_value("state");
                    gotCallback   = true;

                    res.set_content(
                        "<!doctype html><html><head><meta charset='utf-8'>"
                        "<title>Gridex sign-in</title>"
                        "<style>body{font:14px sans-serif;text-align:center;padding:80px}</style>"
                        "</head><body><h2>Sign-in complete</h2>"
                        "<p>You can close this tab and return to Gridex.</p>"
                        "<script>setTimeout(()=>window.close(),500)</script>"
                        "</body></html>",
                        "text/html");
                    // Don't call svr.stop() from inside the handler —
                    // the outer wait loop handles teardown via
                    // gotCallback + is_running(). Calling stop() here
                    // (and again outside) trips httplib's
                    // svr_sock_ != INVALID_SOCKET assertion.
                });

                // Bind to 127.0.0.1 only (not 0.0.0.0) to prevent LAN-side hijack
                svr.set_address_family(AF_INET);

                // Run the server in a separate thread with timeout
                bool serverStarted = false;
                std::mutex startMtx;
                std::condition_variable startCv;

                std::thread svrThread([&]() {
                    {
                        std::lock_guard<std::mutex> lk(startMtx);
                        serverStarted = true;
                    }
                    startCv.notify_one();
                    svr.listen("127.0.0.1", kCallbackPort);
                });

                // Wait for server to be ready
                {
                    std::unique_lock<std::mutex> lk(startMtx);
                    startCv.wait(lk, [&] { return serverStarted; });
                }

                // Give the server a small moment to bind
                std::this_thread::sleep_for(std::chrono::milliseconds(100));

                // Wait for callback (up to kCallbackTimeoutSeconds)
                auto deadline = std::chrono::steady_clock::now()
                    + std::chrono::seconds(kCallbackTimeoutSeconds);
                while (!gotCallback && std::chrono::steady_clock::now() < deadline)
                    std::this_thread::sleep_for(std::chrono::milliseconds(200));

                if (svrThread.joinable())
                {
                    if (svr.is_running()) svr.stop();
                    svrThread.join();
                }

                if (!gotCallback)
                    throw std::runtime_error("Sign-in timed out — no callback received");

                // CSRF guard
                if (receivedState != state)
                    throw std::runtime_error("State mismatch — possible CSRF, sign-in aborted");

                if (receivedCode.empty())
                    throw std::runtime_error("No authorization code in callback");

                // Exchange code for tokens via WinHTTP (replaces an
                // earlier httplib::SSLClient path that crashed on
                // SSL_shutdown when the SSLClient destructor ran on
                // the worker thread).
                std::string formBody =
                    "grant_type=authorization_code"
                    "&code="           + PercentEncode(receivedCode) +
                    "&redirect_uri="   + PercentEncode(redirectURI) +
                    "&client_id="      + PercentEncode(kClientId) +
                    "&code_verifier="  + PercentEncode(verifier);

                auto res2 = HttpsPost(Utf8ToWide(kIssuerHost), L"/oauth/token",
                                      formBody, L"application/x-www-form-urlencoded");

                if (res2.status < 200 || res2.status >= 300)
                {
                    throw std::runtime_error(
                        "Token exchange failed (HTTP " + std::to_string(res2.status)
                        + "): " + (res2.body.empty() ? "(no body)" : res2.body));
                }

                auto j = nlohmann::json::parse(res2.body);
                std::string accessToken  = j.value("access_token",  "");
                std::string refreshToken = j.value("refresh_token", "");
                std::string idToken      = j.value("id_token",      "");

                if (accessToken.empty())
                    throw std::runtime_error("Token exchange returned no access_token");
                if (refreshToken.empty())
                    throw std::runtime_error("Token exchange returned no refresh_token (offline_access scope missing?)");

                DBModels::ChatGPTTokenBundle bundle;
                bundle.accessToken  = accessToken;
                bundle.refreshToken = refreshToken;
                bundle.idToken      = idToken;
                bundle.obtainedAtUnix = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
                ApplyClaims(bundle);

                SaveBundle(bundle);

                callback(true, fromUtf8(bundle.email));
            }
            catch (const std::exception& e)
            {
                callback(false, fromUtf8(e.what()));
            }
            catch (...)
            {
                callback(false, L"Unknown error during sign-in");
            }
        }).detach();
    }
}
