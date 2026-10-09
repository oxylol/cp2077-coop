#include "Settings.h"
#include <RED4ext/LaunchParameters.hpp>

#include <fstream>
#include <map>
#include <optional>

extern std::filesystem::path GPluginFolder;

namespace
{
// coop.ini as it's first written: code/assets/coop.ini (also in the release zip), built in by xmake's utils.bin2c
// with a terminating zero.
constexpr unsigned char kDefaultIni[] = {
#include "coop.ini.h"
};

std::string Trim(std::string aText)
{
    const auto first = aText.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return {};
    const auto last = aText.find_last_not_of(" \t\r\n");
    return aText.substr(first, last - first + 1);
}

// key = value lines; ";" or "#" start a comment line; [sections] are ignored.
std::map<std::string, std::string> ReadIni(const fs::path& acPath)
{
    std::map<std::string, std::string> values;
    std::ifstream file(acPath);
    std::string line;
    while (std::getline(file, line))
    {
        line = Trim(line);
        if (line.empty() || line[0] == ';' || line[0] == '#' || line[0] == '[')
            continue;
        const auto equals = line.find('=');
        if (equals == std::string::npos)
            continue;
        auto key = Trim(line.substr(0, equals));
        std::ranges::transform(key, key.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        values[key] = Trim(line.substr(equals + 1));
    }
    return values;
}

std::optional<std::string> LaunchArgument(const char* acName)
{
    // The game's parser only fills these in for the --name=value form.
    const auto values = RED4ext::GetLaunchParameters().Get(acName);
    if (values && values->size > 0)
        return std::string((*values)[0].c_str());
    return std::nullopt;
}

uint16_t ToPort(const std::string& acText, uint16_t aDefault)
{
    const auto value = std::strtoul(acText.c_str(), nullptr, 10);
    return value > 0 && value <= 0xFFFF ? static_cast<uint16_t>(value) : aDefault;
}
} // namespace

void Settings::Load()
{
    Settings& settings = Get();
    settings.iniPath = GPluginFolder / "coop.ini";

    std::error_code error;
    if (!fs::exists(settings.iniPath, error))
    {
        std::ofstream file(settings.iniPath, std::ios::binary);
        file.write(reinterpret_cast<const char*>(kDefaultIni), sizeof(kDefaultIni) - 1);
        spdlog::info("Created {}", settings.iniPath.string());
    }

    auto values = ReadIni(settings.iniPath);
    const auto value = [&](const char* acKey, const char* acArgument) -> std::optional<std::string> {
        if (auto argument = LaunchArgument(acArgument))
            return argument;
        if (const auto it = values.find(acKey); it != values.end())
            return it->second;
        return std::nullopt;
    };

    if (auto name = value("name", "-name"); name && !name->empty())
        settings.name = name->c_str();
    if (settings.name.empty())
        settings.name = "V";
    if (auto password = value("password", "-password"))
        settings.password = password->c_str();
    if (auto join = value("join_address", "-join"); join && !join->empty())
        settings.joinAddress = join->c_str();
    if (auto port = value("port", "-port"))
        settings.port = ToPort(*port, settings.port);
    if (auto maxPlayers = value("max_players", "-max_players"))
        settings.maxPlayers = std::clamp<uint16_t>(ToPort(*maxPlayers, settings.maxPlayers), 2, 16);

    spdlog::info("Co-op settings ({}): name {}, join {}, host port {}, up to {} players, {}", settings.iniPath.string(),
                 settings.name, settings.joinAddress, settings.port, settings.maxPlayers,
                 settings.password.empty() ? "no password" : "password set");
}
