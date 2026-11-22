#include "ui/account.h"
#include "app/log.h"
#include "imgui.h"
#include "login/login.h"
#include "ui/imgui_window.h"
#include <array>
#include <chrono>
#include <format>
#include <mutex>
#include <thread>

namespace tmloader
{
namespace
{

using namespace std::chrono_literals;

// How long the outcome stays on screen before the game starts.
constexpr auto kResultDelay = 1500ms;

constexpr auto kErrorColor = ImVec4(1.0f, 0.55f, 0.5f, 1.0f);

std::mutex  statusMutex;
std::string status;

auto describeOutcome(const LoginResult& result) -> std::string
{
    if (result.created)
    {
        return std::format("Account {} created.", result.polId);
    }

    return std::format("Signed in as {}.", result.polId);
}

// The game connects to the address it was given, so a host name is resolved here.
auto attempt(const Config& config) -> std::pair<std::string, LoginResult>
{
    const auto address = resolveIpv4(config.server);
    logLine("LOGIN {} at {}:{}", config.polId, address, config.gamePort);
    return { address, login(address, config.gamePort, config.polId, config.password) };
}

auto signedIn(Config config, const std::string& address, const LoginResult& result) -> Config
{
    const auto outcome = describeOutcome(result);
    logLine("LOGIN accepted: {}", outcome);
    {
        const std::lock_guard lock(statusMutex);
        status = outcome;
    }

    config.server   = address;
    config.polId    = result.polId;
    config.password = result.gamePassword;
    return config;
}

template <size_t Size>
void copyTo(std::array<char, Size>& field, const std::string& value)
{
    field = {};
    value.copy(field.data(), Size - 1);
}

// Uppercases POL ID input; the alphabet itself is checked on submit.
auto polIdFilter(ImGuiInputTextCallbackData* data) -> int
{
    if (data->EventChar >= 'a' && data->EventChar <= 'z')
    {
        data->EventChar = static_cast<ImWchar>(data->EventChar - 'a' + 'A');
    }

    return 0;
}

class AccountPrompt
{
public:
    AccountPrompt(const Config& config, std::filesystem::path configFile, std::string message)
    : config_(config)
    , configFile_(std::move(configFile))
    , message_(std::move(message))
    , error_(!message_.empty())
    {
        copyTo(polId_, config.polId);
        copyTo(password_, config.password);
        copyTo(server_, config.server);
    }

    auto frame() -> bool
    {
        const auto [busy, message, error, done, closeAt] = snapshot();
        if (done && std::chrono::steady_clock::now() >= closeAt)
        {
            return false;
        }

        const auto* const viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(viewport->WorkSize);
        ImGui::Begin("account", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
        ImGui::TextUnformatted("TETRA MASTER");
        ImGui::TextWrapped("Sign in with your POL ID and password. A POL ID the server does not know yet becomes a new account on first login, so check it for typos.");
        ImGui::Spacing();
        ImGui::BeginDisabled(busy || done);
        ImGui::TextUnformatted("POL ID");
        ImGui::SetNextItemWidth(-1);
        ImGui::InputText("##pol-id", polId_.data(), polId_.size(), ImGuiInputTextFlags_CharsNoBlank | ImGuiInputTextFlags_CallbackCharFilter, polIdFilter);
        ImGui::TextUnformatted("Password");
        ImGui::SetNextItemWidth(-1);
        const auto submitted = ImGui::InputText("##password", password_.data(), password_.size(), ImGuiInputTextFlags_Password | ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::TextUnformatted("Server");
        ImGui::SetNextItemWidth(-1);
        ImGui::InputText("##server", server_.data(), server_.size(), ImGuiInputTextFlags_CharsNoBlank);
        ImGui::Spacing();
        if (ImGui::Button("Sign in") || submitted)
        {
            submit();
        }

        ImGui::EndDisabled();
        ImGui::Spacing();
        if (busy)
        {
            ImGui::TextDisabled("Signing in...");
        }
        else if (!message.empty())
        {
            ImGui::PushTextWrapPos(0);
            if (error)
            {
                ImGui::TextColored(kErrorColor, "%s", message.c_str());
            }
            else
            {
                ImGui::TextUnformatted(message.c_str());
            }

            ImGui::PopTextWrapPos();
        }

        ImGui::End();
        return true;
    }

    auto finish() -> std::optional<Config>
    {
        worker_ = {};
        const std::lock_guard lock(mutex_);
        return result_;
    }

private:
    struct State
    {
        bool        busy = false;
        std::string message;
        bool        error = false;
        bool        done  = false;
        // When a successful sign-in closes the window.
        std::chrono::steady_clock::time_point closeAt;
    };

    auto snapshot() -> State
    {
        const std::lock_guard lock(mutex_);
        return { busy_, message_, error_, result_.has_value(), closeAt_ };
    }

    void report(std::string message, bool error)
    {
        const std::lock_guard lock(mutex_);
        busy_    = false;
        message_ = std::move(message);
        error_   = error;
    }

    void submit()
    {
        auto entered     = config_;
        entered.polId    = polId_.data();
        entered.password = password_.data();
        entered.server   = server_.data();
        if (!isValidPolId(entered.polId))
        {
            report("A POL ID is 8 letters or digits.", true);
            return;
        }

        if (!isValidPassword(entered.password))
        {
            report("The password must be 1 to 15 characters: letters, digits, spaces or punctuation.", true);
            return;
        }

        if (!isValidServer(entered.server))
        {
            report("Enter the server's address or host name.", true);
            return;
        }

        {
            const std::lock_guard lock(mutex_);
            busy_ = true;
        }

        worker_ = std::jthread([this, entered]
                               {
                                   run(entered);
                               });
    }

    void run(const Config& entered)
    {
        try
        {
            const auto [address, result] = attempt(entered);
            saveConfigValues(configFile_, { { "pol-id", entered.polId }, { "password", entered.password }, { "server", entered.server } });
            auto game = signedIn(entered, address, result);
            report(describeOutcome(result) + " Starting the game...", false);
            const std::lock_guard lock(mutex_);
            closeAt_ = std::chrono::steady_clock::now() + kResultDelay;
            result_  = std::move(game);
        }
        catch (const LoginError& error)
        {
            logLine("LOGIN failed: {} {}", error.code(), error.what());
            report(describeLoginError(error), true);
        }
        catch (const std::exception& error)
        {
            logLine("Sign-in failed: {}", error.what());
            report(error.what(), true);
        }
    }

    const Config&                         config_;
    const std::filesystem::path           configFile_;
    std::array<char, 9>                   polId_{};
    std::array<char, 16>                  password_{};
    std::array<char, 254>                 server_{};
    std::mutex                            mutex_;
    bool                                  busy_ = false;
    std::string                           message_;
    bool                                  error_ = false;
    std::optional<Config>                 result_;
    std::chrono::steady_clock::time_point closeAt_;
    std::jthread                          worker_;
};

} // namespace

auto signIn(const Config& config, const std::filesystem::path& configFile) -> std::optional<Config>
{
    auto message = std::string{};
    if (!config.polId.empty() && !config.password.empty())
    {
        try
        {
            const auto [address, result] = attempt(config);
            return signedIn(config, address, result);
        }
        catch (const LoginError& error)
        {
            logLine("LOGIN failed: {} {}", error.code(), error.what());
            message = describeLoginError(error);
        }
    }

    auto       prompt = AccountPrompt(config, configFile, message);
    const auto opened = runImGuiWindow({ L"Tetra Master", 460, 430 }, [&prompt]() -> bool
                                       {
                                           return prompt.frame();
                                       });
    if (!opened)
    {
        throw std::runtime_error("Cannot open the sign-in window");
    }

    return prompt.finish();
}

auto accountStatus() -> std::string
{
    const std::lock_guard lock(statusMutex);
    return status;
}

} // namespace tmloader
