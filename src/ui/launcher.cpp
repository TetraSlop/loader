#include "ui/launcher.h"
#include "app/log.h"
#include "imgui.h"
#include "ui/imgui_window.h"
#include <mutex>
#include <shellapi.h>
#include <thread>

namespace tmloader
{
namespace
{

enum class Stage
{
    connecting,
    unavailable,
    signedOut,
    signingIn,
    signedIn,
    starting,
    conflict,
    ready,
};

auto describe(const std::exception& error) -> std::string
{
    const auto* const api = dynamic_cast<const hosted::ApiError*>(&error);
    if (!api)
    {
        return error.what();
    }

    const auto& code = api->code();
    if (code == "account_not_linked")
    {
        return "This Discord account is not enrolled on the server yet. Send the user ID above to the server operator.";
    }

    if (code == "session_owner_mismatch")
    {
        return "The play session belongs to a different account. Sign in again.";
    }

    if (code == "auth_unavailable" || code == "storage_unavailable" || code == "capacity")
    {
        return "The server is busy or unavailable. Try again in a moment.";
    }

    if (api->status() == 401)
    {
        return "Your sign-in has expired. Sign in again.";
    }

    if (code.empty())
    {
        return std::format("The server refused the request (HTTP {}).", api->status());
    }

    return std::format("The server refused the request (HTTP {} {}).", api->status(), code);
}

void openBrowser(const std::string& url)
{
    const auto wide = std::wstring(url.begin(), url.end()); // Percent-encoded, so ASCII.
    ShellExecuteW(nullptr, L"open", wide.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

class Launcher
{
public:
    explicit Launcher(const Config& config)
    : config_(config)
    {
        run([this](std::stop_token)
            {
                connect();
            });
    }

    auto frame() -> bool
    {
        const auto* const viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(viewport->WorkSize);
        ImGui::Begin("launcher", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
        ImGui::TextUnformatted("TETRA MASTER");
        ImGui::TextDisabled("%s", config_.hostedServer.c_str());
        ImGui::Separator();
        ImGui::Spacing();
        const auto [stage, message] = snapshot();
        contents(stage);
        if (!message.empty())
        {
            ImGui::Spacing();
            ImGui::TextWrapped("%s", message.c_str());
        }

        ImGui::End();
        return stage != Stage::ready;
    }

    auto finish() -> std::unique_ptr<hosted::HostedSession>
    {
        worker_ = {};
        if (snapshot().first != Stage::ready)
        {
            return nullptr;
        }

        return std::move(session_);
    }

private:
    void contents(Stage stage)
    {
        switch (stage)
        {
            case Stage::connecting:
                ImGui::TextUnformatted("Connecting to the server...");
                break;
            case Stage::unavailable:
                if (ImGui::Button("Retry"))
                {
                    run([this](std::stop_token)
                        {
                            connect();
                        });
                }

                break;
            case Stage::signedOut:
                if (ImGui::Button("Sign in with Discord"))
                {
                    run([this](std::stop_token stop)
                        {
                            signIn(stop);
                        });
                }

                break;
            case Stage::signingIn:
                ImGui::TextWrapped("Complete sign-in in your browser, then return here.");
                if (ImGui::Button("Cancel"))
                {
                    worker_ = {};
                    update(Stage::signedOut);
                }

                break;
            case Stage::signedIn:
                userIdRow();
                if (ImGui::Button("Play"))
                {
                    run([this](std::stop_token)
                        {
                            play(false);
                        });
                }

                ImGui::SameLine();
                if (ImGui::Button("Log out"))
                {
                    run([this](std::stop_token)
                        {
                            logout();
                        });
                }

                break;
            case Stage::starting:
            case Stage::ready:
                ImGui::TextUnformatted("Starting...");
                break;
            case Stage::conflict:
                ImGui::TextWrapped("This account is already playing in another window or on another computer.");
                if (ImGui::Button("Disconnect other session"))
                {
                    run([this](std::stop_token)
                        {
                            play(true);
                        });
                }

                ImGui::SameLine();
                if (ImGui::Button("Back"))
                {
                    update(Stage::signedIn);
                }

                break;
        }
    }

    void userIdRow()
    {
        const auto id = session_->userId();
        if (id.empty())
        {
            return;
        }

        ImGui::TextDisabled("User ID: %s", id.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Copy"))
        {
            ImGui::SetClipboardText(id.c_str());
        }
    }

    void run(std::function<void(std::stop_token)> task)
    {
        worker_ = std::jthread([task = std::move(task)](std::stop_token stop)
                               {
                                   task(stop);
                               });
    }

    auto snapshot() -> std::pair<Stage, std::string>
    {
        const std::lock_guard lock(mutex_);
        return { stage_, message_ };
    }

    void update(Stage stage, std::string message = {})
    {
        const std::lock_guard lock(mutex_);
        stage_   = stage;
        message_ = std::move(message);
    }

    // Each step reports failures in the launcher instead of throwing.
    template <typename Step>
    void attempt(Stage onFailure, Step step)
    {
        try
        {
            step();
        }
        catch (const std::exception& error)
        {
            logLine("Hosted launcher: {}", error.what());
            update(onFailure, describe(error));
        }
    }

    void connect()
    {
        update(Stage::connecting);
        attempt(Stage::unavailable, [this]
                {
                    session_ = std::make_unique<hosted::HostedSession>(config_);
                    update(session_->resume() ? Stage::signedIn : Stage::signedOut);
                });
    }

    void signIn(std::stop_token stop)
    {
        update(Stage::signingIn);
        attempt(Stage::signedOut, [&]
                {
                    if (session_->signIn(openBrowser, stop))
                    {
                        update(Stage::signedIn);
                    }
                });
    }

    void play(bool disconnectOther)
    {
        update(Stage::starting);
        try
        {
            if (disconnectOther)
            {
                session_->disconnectOtherSession();
            }

            session_->startPlay();
            update(Stage::ready);
        }
        catch (const hosted::ApiError& error)
        {
            logLine("Hosted launcher: {}", error.what());
            if (error.code() == "session_conflict")
            {
                update(Stage::conflict);
            }
            else if (error.status() == 401 && !session_->signedIn())
            {
                update(Stage::signedOut, describe(error));
            }
            else
            {
                update(Stage::signedIn, describe(error));
            }
        }
        catch (const std::exception& error)
        {
            logLine("Hosted launcher: {}", error.what());
            update(Stage::signedIn, describe(error));
        }
    }

    void logout()
    {
        session_->logout();
        update(Stage::signedOut);
    }

    const Config&                          config_;
    std::mutex                             mutex_;
    Stage                                  stage_ = Stage::connecting;
    std::string                            message_;
    std::unique_ptr<hosted::HostedSession> session_;
    std::jthread                           worker_;
};

} // namespace

auto runLauncher(const Config& config) -> std::unique_ptr<hosted::HostedSession>
{
    auto launcher = Launcher(config);
    if (!runImGuiWindow({ L"Tetra Master", 460, 300 }, [&]
                        {
                            return launcher.frame();
                        }))
    {
        throw std::runtime_error("Cannot open the launcher window");
    }

    return launcher.finish();
}

} // namespace tmloader
