//
// MCPApprovalGate.cpp
//
// Implementation marshals a ContentDialog onto the MainWindow
// DispatcherQueue. winrt com_ptr handles are stored in opaque
// void* members of MCPApprovalGate so the header stays free of
// winrt includes.

#include "MCPApprovalGate.h"

#include <windows.h>
#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Windows.Foundation.h>
#include <atomic>
#include <memory>

namespace mux  = winrt::Microsoft::UI::Xaml;
namespace mxc  = winrt::Microsoft::UI::Xaml::Controls;
namespace mxd  = winrt::Microsoft::UI::Dispatching;

namespace DBModels
{
    void MCPApprovalGate::setUIContext(void* dispatcherQueue, void* xamlRoot)
    {
        std::lock_guard<std::mutex> lk(mtx_);
        dispatcher_ = dispatcherQueue;
        xamlRoot_   = xamlRoot;
    }

    void MCPApprovalGate::revokeSessionApproval(const std::wstring& connectionId)
    {
        std::lock_guard<std::mutex> lk(mtx_);
        for (auto it = sessionApprovals_.begin(); it != sessionApprovals_.end(); )
        {
            if (it->first.first == connectionId) it = sessionApprovals_.erase(it);
            else ++it;
        }
    }

    void MCPApprovalGate::revokeAllSessionApprovals()
    {
        std::lock_guard<std::mutex> lk(mtx_);
        sessionApprovals_.clear();
    }

    std::future<bool> MCPApprovalGate::requestApproval(const MCPApprovalRequest& req)
    {
        auto promise = std::make_shared<std::promise<bool>>();
        auto future = promise->get_future();

        const auto key = std::make_pair(req.connectionId, req.tool);

        // Session cache hit?
        {
            std::lock_guard<std::mutex> lk(mtx_);
            auto it = sessionApprovals_.find(key);
            if (it != sessionApprovals_.end())
            {
                const auto age = std::chrono::system_clock::now() - it->second;
                if (age < kSessionTTL)
                {
                    promise->set_value(true);
                    return future;
                }
            }
        }

        // No UI hookup yet (server in headless / pre-window state).
        // Fail closed — deny rather than hang forever.
        if (!dispatcher_ || !xamlRoot_)
        {
            promise->set_value(false);
            return future;
        }

        // Copy winrt handles onto the stack before capturing.
        auto dispatcher = *reinterpret_cast<mxd::DispatcherQueue*>(&dispatcher_);
        auto xamlRoot   = *reinterpret_cast<mux::XamlRoot*>(&xamlRoot_);

        auto resolved = std::make_shared<std::atomic<bool>>(false);

        auto self = this;
        auto enqueued = dispatcher.TryEnqueue(
            [self, req, xamlRoot, promise, resolved, key]()
            {
                try
                {
                    mxc::ContentDialog dlg;
                    dlg.Title(winrt::box_value(winrt::hstring(
                        req.clientName + L" wants to:")));

                    const std::wstring body =
                        req.description + L"\n\n" +
                        req.details     + L"\n\n" +
                        L"Connection: " + (req.connectionId.size() > 8
                            ? req.connectionId.substr(0, 8) + L"..."
                            : req.connectionId);

                    dlg.Content(winrt::box_value(winrt::hstring(body)));
                    dlg.CloseButtonText(L"Deny");
                    dlg.PrimaryButtonText(L"Approve Once");
                    dlg.SecondaryButtonText(L"Approve for Session");
                    dlg.DefaultButton(mxc::ContentDialogButton::Close);
                    dlg.XamlRoot(xamlRoot);

                    auto op = dlg.ShowAsync();
                    op.Completed(
                        [self, promise, resolved, key](
                            auto const& asyncOp,
                            winrt::Windows::Foundation::AsyncStatus status)
                        {
                            if (resolved->exchange(true)) return;

                            bool approved = false;
                            bool sessionApproval = false;
                            if (status == winrt::Windows::Foundation::AsyncStatus::Completed)
                            {
                                try
                                {
                                    const auto r = asyncOp.GetResults();
                                    if (r == mxc::ContentDialogResult::Primary)
                                        approved = true;
                                    else if (r == mxc::ContentDialogResult::Secondary)
                                    {
                                        approved = true;
                                        sessionApproval = true;
                                    }
                                }
                                catch (...) {}
                            }

                            if (sessionApproval)
                            {
                                std::lock_guard<std::mutex> lk(self->mtx_);
                                self->sessionApprovals_[key] =
                                    std::chrono::system_clock::now();
                            }

                            promise->set_value(approved);
                        });
                }
                catch (...)
                {
                    if (!resolved->exchange(true))
                        promise->set_value(false);
                }
            });

        if (!enqueued)
        {
            // Dispatcher already shut down.
            promise->set_value(false);
        }
        return future;
    }
}
