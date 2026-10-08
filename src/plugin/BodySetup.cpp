#include "plugin/BodySetup.hpp"

#include <sstream>

#include <RED4ext/GameEngine.hpp>
#include <RED4ext/Scripting/Natives/Generated/anim/AnimSetup.hpp>
#include <RED4ext/Scripting/Natives/Generated/anim/AnimSetupEntry.hpp>
#include <RED4ext/Scripting/Natives/entEntity.hpp>
#include <RED4ext/Scripting/Natives/entIComponent.hpp>

#include "core/Log.hpp"

namespace coop::plugin
{
namespace
{
using RED4ext::CName;

RED4ext::CClass* ClassOf(const char* aName)
{
    return RED4ext::CRTTISystem::Get()->GetClass(CName(aName));
}

// The animation setup ("animations") of a component that has one, or nullptr.
RED4ext::anim::AnimSetup* AnimSetupOf(RED4ext::IScriptable* aComponent)
{
    auto* prop = aComponent ? aComponent->GetType()->GetProperty(CName("animations")) : nullptr;
    if (!prop || prop->type->GetName() != CName("animAnimSetup"))
        return nullptr;
    return prop->GetValuePtr<RED4ext::anim::AnimSetup>(aComponent);
}

bool SetBool(RED4ext::IScriptable* aObject, const char* aName, bool aValue)
{
    auto* prop = aObject->GetType()->GetProperty(CName(aName));
    if (!prop || prop->type->GetName() != CName("Bool"))
        return false;
    *prop->GetValuePtr<bool>(aObject) = aValue;
    return true;
}
} // namespace

BodySetup& BodySetup::Get()
{
    // Never destroyed (it used to hold an engine resource reference; kept that way, it costs nothing).
    static auto* instance = new BodySetup();
    return *instance;
}

void BodySetup::SetOptions(const BodyOptions& aOptions)
{
    std::scoped_lock lock(m_mutex);
    m_options = aOptions;
}

BodyOptions BodySetup::Options() const
{
    std::scoped_lock lock(m_mutex);
    return m_options;
}

bool BodySetup::IsRegistered() const
{
    return m_active.load();
}

bool BodySetup::EnsureRegistered(const Red::Handle<Red::IScriptable>& aTarget)
{
    {
        std::scoped_lock lock(m_mutex);
        if (m_attempted)
            return m_active.load();
    }
    const auto fail = [this](const std::string& aError, bool aForGood) {
        std::scoped_lock lock(m_mutex);
        m_registerError = aError;
        m_attempted = aForGood;
        return false;
    };
    if (!aTarget)
        return fail("no handle to the co-op system", false);

    // Codeware's callback system is a game system; found by its class.
    auto* systemClass = ClassOf("CallbackSystem");
    auto* engine = RED4ext::CGameEngine::Get();
    if (!systemClass || !engine || !engine->framework || !engine->framework->gameInstance)
        return fail("Codeware's CallbackSystem class not found", false);
    auto* system = reinterpret_cast<RED4ext::IScriptable*>(engine->framework->gameInstance->GetSystem(systemClass));
    if (!system)
        return fail("Codeware's CallbackSystem not running yet", false);

    Red::Handle<Red::IScriptable> handler;
    CName eventName("Entity/Initialize");
    CName function("OnBodyInitialize");
    bool sticky = true; // survives session changes
    auto target = aTarget;
    if (!Red::CallVirtual(system, "RegisterCallback", handler, eventName, target, function, sticky) || !handler)
    {
        COOP_LOG_WARN("body setup: CallbackSystem.RegisterCallback failed");
        return fail("CallbackSystem.RegisterCallback failed", true); // don't retry every second
    }
    // Until both targets are on, Codeware runs the handler for every entity; Prepare ignores it (m_active false).
    int targets = 0;
    for (const char* tag : {"Cp2077Coop.Puppet", "CoopMirror"})
    {
        Red::Handle<Red::IScriptable> entityTarget;
        CName tagName(tag);
        if (!Red::CallStatic("DynamicEntityTarget", "Tag", entityTarget, tagName) || !entityTarget)
            continue;
        Red::Handle<Red::IScriptable> same;
        if (Red::CallVirtual(handler.instance, "AddTarget", same, entityTarget))
            ++targets;
    }
    if (targets < 2)
    {
        // An untargeted handler would fire for every entity in the game: take it out again.
        Red::CallVirtual(handler.instance, "Unregister");
        COOP_LOG_WARN("body setup: DynamicEntityTarget.Tag / AddTarget failed, callback removed");
        return fail("DynamicEntityTarget.Tag / AddTarget failed (Codeware too old?)", true);
    }
    {
        std::scoped_lock lock(m_mutex);
        m_registerError.clear();
        m_attempted = true;
    }
    m_active = true;
    COOP_LOG_INFO("body setup: listening for puppet and mirror bodies being built");
    return true;
}

void BodySetup::RememberPlayer(Red::IScriptable* aPlayer)
{
    m_localPlayer = aPlayer;
    static auto* entityClass = ClassOf("entEntity");
    static auto* animatedClass = ClassOf("entAnimatedComponent");
    static auto* extensionClass = ClassOf("entAnimationSetupExtensionComponent");
    if (!aPlayer || aPlayer == m_animsetsOf || !entityClass || !aPlayer->GetType()->IsA(entityClass))
        return;
    // V's gameplay animation sets: its root animated component's and every animation setup extension's.
    std::vector<std::pair<uint64_t, uint8_t>> sets;
    for (auto& component : reinterpret_cast<RED4ext::ent::Entity*>(aPlayer)->components)
    {
        if (!component)
            continue;
        const auto* type = component->GetType();
        const bool root = animatedClass && type->IsA(animatedClass) && component->name == CName("root");
        const bool extension = extensionClass && type->IsA(extensionClass);
        if (!root && !extension)
            continue;
        if (const auto* setup = AnimSetupOf(component.instance))
        {
            for (const auto& entry : setup->gameplay)
                sets.emplace_back(entry.animSet.path.hash, entry.priority);
        }
    }
    if (sets.empty())
        return; // not loaded yet: try again next time
    m_animsetsOf = aPlayer;
    std::scoped_lock lock(m_mutex);
    m_playerAnimsets = std::move(sets);
}

void BodySetup::ForgetPlayer()
{
    m_localPlayer = nullptr;
    m_animsetsOf = nullptr;
    std::scoped_lock lock(m_mutex);
    m_playerAnimsets.clear();
}

void BodySetup::Prepare(Red::IScriptable* aEntity)
{
    static auto* entityClass = ClassOf("entEntity");
    static auto* impostorClass = ClassOf("gameImpostorComponent");
    static auto* playerClass = ClassOf("PlayerPuppet");
    // Only once both tag targets are on (see EnsureRegistered), and never the local V itself.
    if (!m_active.load() || !aEntity || aEntity == m_localPlayer.load() || !entityClass
        || !aEntity->GetType()->IsA(entityClass))
        return;
    ++m_prepared;
    const auto options = Options();
    auto* entity = reinterpret_cast<RED4ext::ent::Entity*>(aEntity);

    // Only player bodies (TPP_Player and the like) get an impostor: they have V's rig, so copying the local V fits.
    // NPC bodies (the plain NPC of drive=ai) are left as they are.
    const bool playerBody = playerClass && aEntity->GetType()->IsA(playerClass);
    const char* impostorResult = "off";
    if (options.addImpostor && impostorClass && !playerBody)
        impostorResult = "left alone (not a player body)";
    if (options.addImpostor && impostorClass && playerBody)
    {
        bool has = false;
        for (auto& component : entity->components)
            has = has || (component && component->GetType()->IsA(impostorClass));
        impostorResult = has ? "had one" : "could not be made";
        if (!has)
        {
            // Set up like the cutscene lookalike's (round D dump): a character replica with its own head.
            auto impostor = Red::MakeScriptedHandle<RED4ext::ent::IComponent>(impostorClass);
            if (impostor)
            {
                impostor->name = CName("CoopImpostor");
                const bool ok = SetBool(impostor.instance, "isCharacterReplica", true)
                                && SetBool(impostor.instance, "addHead", true)
                                && SetBool(impostor.instance, "ignorePlayerHeadSlot", true);
                if (ok)
                {
                    entity->components.PushBack(impostor);
                    ++m_impostorsAdded;
                    impostorResult = "added";
                }
                else
                {
                    impostorResult = "not added (unexpected fields)";
                    std::scoped_lock lock(m_mutex);
                    m_lastProblem = "the impostor component has other fields than expected";
                }
            }
        }
    }

    // borrowAnimsets: V's gameplay animation sets added to an NPC body's root animated component.
    int borrowed = -1;
    if (options.borrowAnimsets && !playerBody)
    {
        static auto* animatedClass = ClassOf("entAnimatedComponent");
        RED4ext::anim::AnimSetup* setup = nullptr;
        for (auto& component : entity->components)
        {
            if (component && animatedClass && component->GetType()->IsA(animatedClass)
                && component->name == CName("root"))
                setup = AnimSetupOf(component.instance);
        }
        std::scoped_lock lock(m_mutex);
        if (setup && !m_playerAnimsets.empty())
        {
            borrowed = 0;
            for (const auto& [hash, priority] : m_playerAnimsets)
            {
                bool present = false;
                for (const auto& entry : setup->gameplay)
                    present = present || entry.animSet.path.hash == hash;
                if (present)
                    continue;
                RED4ext::anim::AnimSetupEntry entry{};
                entry.animSet.path = hash;
                entry.priority = priority;
                setup->gameplay.PushBack(entry);
                ++borrowed;
            }
            m_animsetsBorrowed += static_cast<uint64_t>(borrowed);
        }
        else
        {
            m_lastProblem = setup ? "V's animation sets aren't known yet" : "the body has no root animation setup";
        }
    }

    // One line per body, so a test round can be checked from the log alone.
    COOP_LOG_INFO("body setup: %s (%u components): impostor %s; animation sets borrowed: %d",
                  aEntity->GetType()->GetName().ToString(), static_cast<unsigned>(entity->components.Size()),
                  impostorResult, borrowed);
}

std::string BodySetup::Status() const
{
    std::scoped_lock lock(m_mutex);
    std::ostringstream out;
    out << "body setup: " << (m_active.load() ? "listening" : "NOT listening");
    if (!m_registerError.empty())
        out << " (" << m_registerError << ")";
    out << "; add impostor " << (m_options.addImpostor ? "on" : "off") << ", borrow V's animation sets "
        << (m_options.borrowAnimsets ? "on" : "off") << " (" << m_playerAnimsets.size() << " known); bodies prepared "
        << m_prepared.load() << ", impostors added " << m_impostorsAdded.load() << ", animation sets borrowed "
        << m_animsetsBorrowed.load();
    if (!m_lastProblem.empty())
        out << "; last problem: " << m_lastProblem;
    out << "\n";
    return out.str();
}
} // namespace coop::plugin
