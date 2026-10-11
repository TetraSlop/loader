#include "ui/launcher.h"
#include "app/log.h"
#include "imgui.h"
#include "ui/image.h"
#include "ui/imgui_window.h"
#include <array>
#include <atomic>
#include <chrono>
#include <format>
#include <mutex>
#include <optional>
#include <shellapi.h>
#include <thread>
#include <utility>

namespace tmloader
{
namespace
{

using namespace std::chrono_literals;

// Signed-in players go straight to the game after this pause unless they switch account.
constexpr auto kAutoPlayDelay = 3s;

// The title screen is six tiles forming a 640x448 composition.
struct TitleTile
{
    const char* name{};
    ImVec2      position;
    ImVec2      size;
};

constexpr std::array<TitleTile, 6> kTitleTiles{ {
    { "title_0", { 0, 0 }, { 256, 256 } },
    { "title_1", { 256, 0 }, { 256, 256 } },
    { "title_2", { 512, 0 }, { 128, 256 } },
    { "title_3", { 0, 256 }, { 256, 192 } },
    { "title_4", { 256, 256 }, { 256, 192 } },
    { "title_5", { 512, 256 }, { 128, 192 } },
} };

constexpr auto kDiscordBlurple = ImVec4(0.345f, 0.396f, 0.949f, 1.0f);

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
        return "This Discord account isn't linked to a game account yet. Send your user ID to the server operator.";
    }

    if (code == "session_owner_mismatch")
    {
        return "That play session belongs to a different account. Sign in again.";
    }

    if (code == "auth_unavailable" || code == "storage_unavailable" || code == "capacity")
    {
        return "The server is busy right now. Try again in a moment.";
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

// Returns from the browser to the launcher. Windows only lets the foreground
// thread hand over focus, so briefly share its input state.
void bringToFront(HWND window)
{
    const auto foreground = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
    const auto current    = GetCurrentThreadId();
    AttachThreadInput(foreground, current, TRUE);
    ShowWindow(window, SW_RESTORE);
    SetForegroundWindow(window);
    AttachThreadInput(foreground, current, FALSE);
}

auto coloredButton(const char* label, const ImVec4& color) -> bool
{
    ImGui::PushStyleColor(ImGuiCol_Button, color);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(color.x * 1.15f, color.y * 1.15f, color.z * 1.15f, 1));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(color.x * 0.85f, color.y * 0.85f, color.z * 0.85f, 1));
    const auto pressed = ImGui::Button(label);
    ImGui::PopStyleColor(3);
    return pressed;
}

class Launcher
{
public:
    explicit Launcher(const Config& config)
    : config_(config)
    {
        run([this](const std::stop_token&)
            {
                connect();
            });
    }

    void created(HWND window, IDirect3DDevice9* device)
    {
        window_ = window;
        device_ = device;
        // The HD pack's title art; the launcher still works without it.
        const auto folder = config_.texturePack.parent_path() / "gW201/1001";
        for (size_t i = 0; i < kTitleTiles.size(); ++i)
        {
            title_[i] = loadImage(device, folder / (std::string(kTitleTiles[i].name) + ".png"));
        }
    }

    auto frame() -> bool
    {
        const auto [stage, message] = snapshot();
        takeAvatar(stage);
        if (raise_.exchange(false))
        {
            bringToFront(window_);
        }

        const auto* const viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(viewport->WorkSize);
        ImGui::Begin("launcher", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground);
        drawBackground(viewport->WorkPos, viewport->WorkSize);
        drawPanel(stage, message, viewport->WorkPos, viewport->WorkSize);
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
    void drawBackground(ImVec2 origin, ImVec2 size)
    {
        auto* const draw = ImGui::GetWindowDrawList();
        draw->AddRectFilled(origin, ImVec2(origin.x + size.x, origin.y + size.y), IM_COL32(12, 14, 20, 255));
        // Cover the window with the 640x448 composition, cropping any overflow.
        const auto scale  = std::max(size.x / 640, size.y / 448);
        const auto offset = ImVec2(origin.x + (size.x - 640 * scale) / 2, origin.y + (size.y - 448 * scale) / 2);
        for (size_t i = 0; i < kTitleTiles.size(); ++i)
        {
            if (!title_[i])
            {
                continue;
            }

            const auto& tile = kTitleTiles[i];
            const auto  from = ImVec2(offset.x + tile.position.x * scale, offset.y + tile.position.y * scale);
            draw->AddImage(title_[i].texture.Get(), from, ImVec2(from.x + tile.size.x * scale, from.y + tile.size.y * scale));
        }

        // Darken the lower part so the panel stays readable over the artwork.
        const auto fade = origin.y + size.y * 0.5f;
        draw->AddRectFilledMultiColor(ImVec2(origin.x, fade), ImVec2(origin.x + size.x, origin.y + size.y), IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 235), IM_COL32(0, 0, 0, 235));
    }

    void drawPanel(Stage stage, const std::string& message, ImVec2 origin, ImVec2 size)
    {
        const auto padding = ImGui::GetStyle().WindowPadding;
        auto       rows    = 3.5f;
        if (!message.empty())
        {
            rows = 6.0f;
        }

        ImGui::SetCursorScreenPos(ImVec2(origin.x + padding.x, origin.y + size.y - ImGui::GetFrameHeightWithSpacing() * rows - padding.y));
        ImGui::PushTextWrapPos(origin.x + size.x - padding.x);
        const auto profile = [&]
        {
            const std::lock_guard lock(mutex_);
            return profile_;
        }();
        if (avatar_)
        {
            const auto extent = ImGui::GetTextLineHeightWithSpacing() * 2;
            ImGui::Image(avatar_.texture.Get(), ImVec2(extent, extent));
            ImGui::SameLine();
        }

        ImGui::BeginGroup();
        if (!profile.displayName.empty() && stage != Stage::signedOut)
        {
            ImGui::TextUnformatted(profile.displayName.c_str());
        }
        else
        {
            ImGui::TextUnformatted("Tetra Master");
        }

        ImGui::TextDisabled("%s", status(stage).c_str());
        ImGui::EndGroup();
        if (!message.empty())
        {
            ImGui::TextUnformatted(message.c_str());
            if (!profile.userId.empty())
            {
                ImGui::TextDisabled("User ID: %s", profile.userId.c_str());
                ImGui::SameLine();
                if (ImGui::SmallButton("Copy"))
                {
                    ImGui::SetClipboardText(profile.userId.c_str());
                }
            }
        }

        ImGui::PopTextWrapPos();
        ImGui::Spacing();
        buttons(stage, message);
    }

    auto status(Stage stage) -> std::string
    {
        switch (stage)
        {
            case Stage::connecting:
                return "Connecting to " + config_.hostedServer + "...";
            case Stage::unavailable:
                return "Can't reach the server.";
            case Stage::signedOut:
                return "Sign in with Discord to play.";
            case Stage::signingIn:
                return "Finish signing in in your browser...";
            case Stage::signedIn:
                if (const auto remaining = secondsUntilPlay())
                {
                    return std::format("Starting in {}...", *remaining);
                }

                return "Signed in with Discord.";
            case Stage::starting:
            case Stage::ready:
                return "Joining the table...";
            case Stage::conflict:
                return "This account is already playing somewhere else.";
        }

        return {};
    }

    void buttons(Stage stage, const std::string& message)
    {
        switch (stage)
        {
            case Stage::unavailable:
                if (ImGui::Button("Retry"))
                {
                    run([this](const std::stop_token&)
                        {
                            connect();
                        });
                }

                break;
            case Stage::signedOut:
                if (coloredButton("Sign in with Discord", kDiscordBlurple))
                {
                    run([this](std::stop_token stop)
                        {
                            signIn(std::move(stop));
                        });
                }

                break;
            case Stage::signingIn:
                if (ImGui::Button("Cancel"))
                {
                    worker_ = {};
                    update(Stage::signedOut);
                }

                break;
            case Stage::signedIn:
                if (message.empty() && secondsUntilPlay() == 0)
                {
                    startPlay(false);
                    break;
                }

                if (coloredButton("Play", kDiscordBlurple))
                {
                    startPlay(false);
                }

                ImGui::SameLine();
                if (ImGui::Button("Switch account"))
                {
                    run([this](const std::stop_token&)
                        {
                            logout();
                        });
                }

                break;
            case Stage::conflict:
                if (coloredButton("Disconnect other session", kDiscordBlurple))
                {
                    startPlay(true);
                }

                ImGui::SameLine();
                if (ImGui::Button("Switch account"))
                {
                    run([this](const std::stop_token&)
                        {
                            logout();
                        });
                }

                break;
            case Stage::connecting:
            case Stage::starting:
            case Stage::ready:
                break;
        }
    }

    auto secondsUntilPlay() -> std::optional<long long>
    {
        const std::lock_guard lock(mutex_);
        if (!autoPlayAt_)
        {
            return std::nullopt;
        }

        const auto remaining = std::chrono::ceil<std::chrono::seconds>(*autoPlayAt_ - std::chrono::steady_clock::now()).count();
        return std::max<long long>(remaining, 0);
    }

    void startPlay(bool disconnectOther)
    {
        {
            const std::lock_guard lock(mutex_);
            autoPlayAt_.reset();
        }

        run([this, disconnectOther](const std::stop_token&)
            {
                play(disconnectOther);
            });
    }

    // Textures belong to the UI thread: build the fetched avatar here, drop it when signed out.
    void takeAvatar(Stage stage)
    {
        if (stage == Stage::signedOut)
        {
            avatar_ = {};
        }

        auto bytes = std::string{};
        {
            const std::lock_guard lock(mutex_);
            bytes = std::exchange(avatarBytes_, {});
        }

        if (!bytes.empty() && device_)
        {
            avatar_ = loadImage(device_, std::as_bytes(std::span(bytes)));
        }
    }

    void run(std::function<void(std::stop_token)> task)
    {
        worker_ = std::jthread([task = std::move(task)](std::stop_token stop)
                               {
                                   task(std::move(stop));
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
        autoPlayAt_.reset();
    }

    // Fetches the avatar and, unless something needs attention, starts the game shortly.
    void signedIn()
    {
        auto                  avatar  = session_->avatar();
        auto                  profile = session_->profile();
        const std::lock_guard lock(mutex_);
        profile_ = std::move(profile);
        stage_   = Stage::signedIn;
        message_.clear();
        avatarBytes_ = std::move(avatar);
        autoPlayAt_  = std::chrono::steady_clock::now() + kAutoPlayDelay;
    }

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
                    if (session_->resume())
                    {
                        signedIn();
                    }
                    else
                    {
                        update(Stage::signedOut);
                    }
                });
    }

    void signIn(std::stop_token stop)
    {
        update(Stage::signingIn);
        attempt(Stage::signedOut, [&]
                {
                    if (session_->signIn(openBrowser, stop))
                    {
                        signedIn();
                        raise_ = true;
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
        {
            const std::lock_guard lock(mutex_);
            profile_ = {};
        }

        update(Stage::signedOut);
    }

    const Config&                                        config_;
    HWND                                                 window_ = nullptr;
    IDirect3DDevice9*                                    device_ = nullptr;
    std::array<Image, kTitleTiles.size()>                title_;
    Image                                                avatar_;
    std::atomic<bool>                                    raise_{ false };
    std::mutex                                           mutex_;
    Stage                                                stage_ = Stage::connecting;
    std::string                                          message_;
    std::string                                          avatarBytes_;
    hosted::HostedSession::Profile                       profile_;
    std::optional<std::chrono::steady_clock::time_point> autoPlayAt_;
    std::unique_ptr<hosted::HostedSession>               session_;
    std::jthread                                         worker_;
};

} // namespace

auto runLauncher(const Config& config) -> std::unique_ptr<hosted::HostedSession>
{
    auto       launcher = Launcher(config);
    const auto opened   = runImGuiWindow({ L"Tetra Master", 640, 480 }, [&]
                                         {
                                           return launcher.frame();
                                         },
                                         [&](HWND window, IDirect3DDevice9* device)
                                         {
                                           launcher.created(window, device);
                                         });
    if (!opened)
    {
        throw std::runtime_error("Cannot open the launcher window");
    }

    return launcher.finish();
}

} // namespace tmloader
