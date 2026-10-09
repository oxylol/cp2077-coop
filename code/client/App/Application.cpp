#include "Application.h"

#include "Core/Foundation/LocaleProvider.hpp"
#include "Core/Foundation/RuntimeProvider.hpp"
#include "Network/NetworkService.h"
#include "Threading/ThreadService.h"

#include "Support/MinHook/MinHookProvider.hpp"
#include "Support/RedLib/RedLibProvider.hpp"
#include "Support/Spdlog/SpdlogProvider.hpp"


namespace App
{
UniquePtr<Application> GApplication;

Application::Application(RED4ext::PluginHandle aPlugin, const RED4ext::Sdk* aSdk)
    : Core::Application(aPlugin, aSdk)
{
    Register<Core::LocaleProvider>();
    Register<Core::RuntimeProvider>(aPlugin)->SetBaseImagePathDepth(2);

    Register<Support::MinHookProvider>();
    Register<Support::SpdlogProvider>();
    Register<Support::RedLibProvider>();

    Register<ThreadService>();
    Register<NetworkService>();
}

void Application::OnStarted()
{
    Core::Application::OnStarted();
}

void Application::OnStopped()
{
    Core::Application::OnStopped();
}
} // namespace App
