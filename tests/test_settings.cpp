#include <cstdio>
#include <filesystem>
#include <fstream>

#include "Test.hpp"
#include "plugin/Settings.hpp"

using coop::plugin::Settings;

TEST_CASE("settings: ini sections, comments and types")
{
    const auto file = std::filesystem::temp_directory_path() / "cp2077coop-settings-test.ini";
    {
        std::ofstream out(file);
        out << "; comment\n"
               "[Player]\n"
               "Name = Filip ; trailing comment\n"
               "[network]\n"
               "port=27100\n"
               "impairment = typical\n"
               "[dev]\n"
               "allowSimClients = no\n"
               "broken line without equals\n";
    }

    Settings settings;
    REQUIRE(settings.Load(file));
    CHECK_EQ(settings.Get("player.name"), "Filip");
    CHECK_EQ(settings.Get("PLAYER.NAME"), "Filip");
    CHECK_EQ(settings.GetInt("network.port", 0), 27100);
    CHECK_EQ(settings.Get("network.impairment"), "typical");
    CHECK(!settings.GetBool("dev.allowSimClients", true));
    CHECK(settings.GetBool("dev.verbose", true)); // missing -> default
    CHECK_EQ(settings.GetInt("player.name", 7), 7); // not a number -> default
    std::filesystem::remove(file);

    Settings missing;
    CHECK(!missing.Load(file));
    CHECK_EQ(missing.Get("player.name", "V"), "V");
}
