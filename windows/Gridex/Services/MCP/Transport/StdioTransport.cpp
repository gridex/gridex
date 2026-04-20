//
// StdioTransport.cpp
//
// Uses Win32 ReadFile/WriteFile on GetStdHandle instead of
// std::cin/std::cout. The Gridex EXE is GUI subsystem so the C
// runtime does not auto-wire stdio — we must talk to the inherited
// handles directly.
//
// Threading:
//   Reader runs on a dedicated std::thread. RequestHandler is
//   invoked inline — handlers that do heavy work should marshal
//   off-thread themselves.
//
// Framing:
//   Newline-delimited JSON. One request per line. Binary-safe
//   enough for JSON (we stop at '\n').

#include "StdioTransport.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>
#include <string>
#include <fstream>

// Diagnostic: mirror every stdio event to
// %APPDATA%\Gridex\mcp-stdio-debug.log. Flip kDebug to true while
// bringing up a new MCP client; keep false in ship builds so we
// don't spam the audit folder.
// Flipped on while Claude Desktop integration is stabilizing —
// the log at %APPDATA%\Gridex\mcp-stdio-debug.log is the only
// thing we have for post-mortem diagnosis of stdio crashes.
static constexpr bool kDebug = true;

static void dbg(const std::string& line)
{
    if (!kDebug) return;
    wchar_t* ad = nullptr;
    if (SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &ad) != S_OK) return;
    std::wstring dir = std::wstring(ad) + L"\\Gridex";
    CoTaskMemFree(ad);
    CreateDirectoryW(dir.c_str(), nullptr);
    std::ofstream f(dir + L"\\mcp-stdio-debug.log",
                    std::ios::app | std::ios::binary);
    if (!f.is_open()) return;
    SYSTEMTIME st; GetSystemTime(&st);
    char buf[32];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ ",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    f << buf << line << '\n';
}

namespace DBModels
{
    namespace
    {
        // Read up to and including a '\n' from `h`. Strip trailing
        // \r\n. Returns false on EOF or error.
        bool readLine(HANDLE h, std::string& out)
        {
            out.clear();
            char buf;
            DWORD got = 0;
            while (true)
            {
                if (!ReadFile(h, &buf, 1, &got, nullptr) || got == 0)
                    return !out.empty(); // final line without newline
                if (buf == '\n') break;
                out.push_back(buf);
            }
            if (!out.empty() && out.back() == '\r') out.pop_back();
            return true;
        }

        void writeAll(HANDLE h, const std::string& s)
        {
            const char* p = s.data();
            size_t rem = s.size();
            while (rem > 0)
            {
                DWORD wrote = 0;
                if (!WriteFile(h, p, static_cast<DWORD>(rem), &wrote, nullptr) || wrote == 0)
                    return;
                p += wrote;
                rem -= wrote;
            }
        }
    }

    StdioTransport::~StdioTransport()
    {
        stop();
        if (reader_.joinable()) reader_.join();
    }

    void StdioTransport::start()
    {
        if (running_.exchange(true)) return;
        reader_ = std::thread([this]() { readLoop(); });
    }

    void StdioTransport::stop()
    {
        running_.store(false);
    }

    void StdioTransport::readLoop()
    {
        HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
        dbg("readLoop start; stdinHandle=" +
            std::to_string(reinterpret_cast<intptr_t>(in)));
        if (in == INVALID_HANDLE_VALUE || in == nullptr)
        {
            dbg("readLoop: no stdin handle, bailing");
            running_.store(false);
            return;
        }

        std::string line;
        while (running_.load())
        {
            line.clear();
            if (!readLine(in, line))
            { dbg("readLoop: EOF"); break; }
            if (line.empty()) continue;
            dbg("readLoop got " + std::to_string(line.size()) + " bytes");

            try
            {
                auto j = nlohmann::json::parse(line);
                JSONRPCRequest req;
                from_json(j, req);
                dbg("readLoop dispatching method=" + req.method);
                if (handler_) handler_(req);
            }
            catch (const std::exception& e)
            {
                dbg(std::string("readLoop parse/handler std::exception: ") + e.what());
                try
                {
                    auto resp = JSONRPCResponse::fail(nullptr, JSONRPCError::parseError());
                    send(resp);
                } catch (...) {}
            }
            catch (...)
            {
                // SEH or non-std exception — catch-all so reader
                // thread can never call std::terminate (which Debug
                // CRT shows as abort() + popup).
                dbg("readLoop unknown exception, sending internalError");
                try
                {
                    auto resp = JSONRPCResponse::fail(
                        nullptr, JSONRPCError::internalError());
                    send(resp);
                } catch (...) {}
            }
        }
        running_.store(false);
    }

    void StdioTransport::send(const JSONRPCResponse& response)
    {
        nlohmann::json j;
        to_json(j, response);
        const std::string line = j.dump() + "\n";

        std::lock_guard<std::mutex> lk(writeMtx_);
        HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
        dbg("send " + std::to_string(line.size()) + " bytes; stdoutHandle=" +
            std::to_string(reinterpret_cast<intptr_t>(out)));
        if (out == INVALID_HANDLE_VALUE || out == nullptr) return;
        writeAll(out, line);
    }

    void StdioTransport::sendNotification(const std::string& method,
                                           const nlohmann::json& params)
    {
        JSONRPCRequest notif;
        notif.method = method;
        notif.params = params;
        nlohmann::json j;
        to_json(j, notif);
        const std::string line = j.dump() + "\n";

        std::lock_guard<std::mutex> lk(writeMtx_);
        HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
        if (out == INVALID_HANDLE_VALUE || out == nullptr) return;
        writeAll(out, line);
    }
}
