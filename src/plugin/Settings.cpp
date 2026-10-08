#include "plugin/Settings.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>

namespace coop::plugin
{
namespace
{
std::string Trim(const std::string& aText)
{
    const auto begin = std::find_if_not(aText.begin(), aText.end(), [](unsigned char c) { return std::isspace(c); });
    const auto end = std::find_if_not(aText.rbegin(), aText.rend(), [](unsigned char c) { return std::isspace(c); }).base();
    return begin < end ? std::string(begin, end) : std::string();
}

std::string Lower(std::string aText)
{
    std::transform(aText.begin(), aText.end(), aText.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return aText;
}
} // namespace

bool Settings::Load(const std::filesystem::path& aFile)
{
    std::ifstream file(aFile);
    if (!file)
        return false;

    std::string section;
    std::string line;
    while (std::getline(file, line))
    {
        const auto comment = line.find_first_of(";#");
        if (comment != std::string::npos)
            line.erase(comment);
        line = Trim(line);
        if (line.empty())
            continue;

        if (line.front() == '[' && line.back() == ']')
        {
            section = Lower(Trim(line.substr(1, line.size() - 2)));
            continue;
        }

        const auto equals = line.find('=');
        if (equals == std::string::npos)
            continue;
        const std::string key = Lower(Trim(line.substr(0, equals)));
        const std::string value = Trim(line.substr(equals + 1));
        m_values[section.empty() ? key : section + "." + key] = value;
    }
    return true;
}

std::string Settings::Get(const std::string& aKey, const std::string& aDefault) const
{
    auto it = m_values.find(Lower(aKey));
    return it != m_values.end() ? it->second : aDefault;
}

int Settings::GetInt(const std::string& aKey, int aDefault) const
{
    const auto value = Get(aKey);
    if (value.empty())
        return aDefault;
    char* end = nullptr;
    const long parsed = std::strtol(value.c_str(), &end, 10);
    return end && *end == '\0' ? static_cast<int>(parsed) : aDefault;
}

bool Settings::GetBool(const std::string& aKey, bool aDefault) const
{
    const auto value = Lower(Get(aKey));
    if (value == "true" || value == "1" || value == "yes" || value == "on")
        return true;
    if (value == "false" || value == "0" || value == "no" || value == "off")
        return false;
    return aDefault;
}
} // namespace coop::plugin
