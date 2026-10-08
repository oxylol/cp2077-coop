#pragma once

#include <filesystem>
#include <map>
#include <string>

namespace coop::plugin
{
// Reads red4ext/plugins/Cp2077Coop/coop.ini ("section.key" lookups). Missing keys use defaults.
class Settings
{
public:
    bool Load(const std::filesystem::path& aFile);

    [[nodiscard]] std::string Get(const std::string& aKey, const std::string& aDefault = {}) const;
    [[nodiscard]] int GetInt(const std::string& aKey, int aDefault) const;
    [[nodiscard]] bool GetBool(const std::string& aKey, bool aDefault) const;

    void Set(const std::string& aKey, const std::string& aValue) { m_values[aKey] = aValue; }

private:
    std::map<std::string, std::string> m_values;
};
} // namespace coop::plugin
