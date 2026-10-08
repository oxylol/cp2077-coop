// Ported from CyberpunkMP (Tilted Phoques SRL), code/client/App/World/AppearanceSystem.cpp,
// code/client/App/Network/NetworkService.cpp and code/client/Game/{Utils,CharacterCustomizationSystem}.h, under the
// CyberpunkMP license (LICENSE.md). See src/plugin/Looks.hpp for what was changed.
#include "plugin/Looks.hpp"

#include <Windows.h>

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>

#include <RED4ext/GameEngine.hpp>
#include <RED4ext/Memory/Pools.hpp>
#include <RED4ext/ResourcePath.hpp>
#include <RED4ext/Scripting/Natives/Generated/game/Object.hpp>
#include <RED4ext/Scripting/Natives/Generated/game/PuppetPS.hpp>
#include <RED4ext/Scripting/Utils.hpp>
#include <RED4ext/TweakDB.hpp>

#include "core/Log.hpp"

namespace coop::plugin
{
namespace
{
using RED4ext::CName;

// ---------------------------------------------------------------------------------------------------------------------
// Engine addresses: hashes from the game's address list (bin/x64/cyberpunk2077_addresses.json), as CyberpunkMP uses
// them. Looked up through RED4ext's resolver directly: the SDK's UniversalRelocBase ends the game when a hash is
// missing, which a new patch can cause; here the step that needs it is switched off and the status says which.

constexpr uint32_t kSerializeState = 108403442u;           // gameuiCharacterCustomizationState: to/from a stream
constexpr uint32_t kCreateState = 2710710832u;             // a new gameuiCharacterCustomizationState handle
constexpr uint32_t kScheduleAppearanceChanges = 386815609u; // worldRuntimeSystemEntityAppearanceChanger
constexpr uint32_t kItemAppearanceName = 3029088864u;      // the appearance suffix of an item on an owner

uintptr_t Resolve(uint32_t aHash)
{
    using ResolveFn = uintptr_t (*)(uint32_t);
    static const ResolveFn resolve = []() -> ResolveFn {
        const HMODULE module = GetModuleHandleW(L"RED4ext.dll");
        return module ? reinterpret_cast<ResolveFn>(GetProcAddress(module, "RED4ext_ResolveAddress")) : nullptr;
    }();
    static std::mutex mutex;
    static std::map<uint32_t, uintptr_t> cache;
    std::scoped_lock lock(mutex);
    const auto found = cache.find(aHash);
    if (found != cache.end())
        return found->second;
    const uintptr_t address = resolve ? resolve(aHash) : 0;
    if (!address)
        COOP_LOG_WARN("look: the game function with address hash %u wasn't found", aHash);
    cache[aHash] = address;
    return address;
}

template<typename T>
T Address(uint32_t aHash)
{
    return reinterpret_cast<T>(Resolve(aHash));
}

RED4ext::CClass* ClassOf(const char* aName)
{
    return RED4ext::CRTTISystem::Get()->GetClass(CName(aName));
}

// IsA on a pointer read from engine memory at a known offset; false instead of a crash if the offset is wrong.
bool SafeIsA(RED4ext::IScriptable* aInstance, RED4ext::CClass* aClass)
{
    if (!aInstance || !aClass)
        return false;
#if defined(_MSC_VER)
    __try
    {
        return aInstance->GetType()->IsA(aClass);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
#else
    return aInstance->GetType()->IsA(aClass);
#endif
}

// ---------------------------------------------------------------------------------------------------------------------
// The engine's serialization stream, as CyberpunkMP declares it (CMPStream/CMPWriter/CMPReader): the customization
// state writes and reads itself through these virtual functions, in this order.

struct ByteStream
{
    virtual void* GetMemoryPool() const // 00
    {
        return RED4ext::Memory::PoolSerializable::Get();
    }
    virtual ~ByteStream() = default;                          // 08
    virtual void Serialize(uint8_t* aBytes, uint64_t aSize) = 0; // 10
    virtual uint64_t GetOffset() const = 0;                   // 18
    virtual uint64_t GetSize() const = 0;                     // 20
    virtual void Seek(int64_t) {}                             // 28
    virtual void Flush() {}                                   // 30
    virtual void ClearError() {}                              // 38
    virtual void GetFileNameForDebug() {}                     // 40
    virtual uint8_t GetSaveVersion() const                    // 48
    {
        return version;
    }
    virtual uint32_t GetGameVersion() const // 50
    {
        return 1;
    }
    virtual RED4ext::CString GetName() const // 58
    {
        return {};
    }
    virtual RED4ext::CString GetName2() const // 60
    {
        return {};
    }
    virtual void Begin() {}                  // 68
    virtual bool EnterNode(CName, bool)      // 70
    {
        return true;
    }
    virtual void LeaveNode() {}  // 78
    virtual bool NodeFound() const // 80
    {
        return true;
    }
    virtual bool sub_88() const // 88
    {
        return true;
    }
    virtual bool IsOK() const // 90
    {
        return true;
    }
    virtual bool sub_98() const // 98
    {
        return true;
    }

    explicit ByteStream(uint8_t aMode)
        : mode(aMode)
    {
    }

    uint8_t mode;           // 08: 1 writing, 2 reading
    uint8_t type = 0;       // 09
    uint8_t status = 0;     // 0A
    uint8_t unk0B = 0;      // 0B
    uint8_t version = 0xC3; // 0C
    void* mapper = nullptr; // 10
};

struct ByteWriter final : ByteStream
{
    ByteWriter()
        : ByteStream(1)
    {
    }
    void Serialize(uint8_t* aBytes, uint64_t aSize) override
    {
        bytes.insert(bytes.end(), aBytes, aBytes + aSize);
    }
    uint64_t GetOffset() const override
    {
        return bytes.size();
    }
    uint64_t GetSize() const override
    {
        return bytes.size();
    }

    std::vector<uint8_t> bytes;
};

struct ByteReader final : ByteStream
{
    explicit ByteReader(const std::vector<uint8_t>& aBytes)
        : ByteStream(2)
        , bytes(aBytes)
    {
    }
    void Serialize(uint8_t* aBytes, uint64_t aSize) override
    {
        for (uint64_t i = 0; i < aSize; ++i)
            aBytes[i] = offset < bytes.size() ? bytes[offset++] : 0;
    }
    uint64_t GetOffset() const override
    {
        return offset;
    }
    uint64_t GetSize() const override
    {
        return bytes.size();
    }

    const std::vector<uint8_t>& bytes;
    uint64_t offset = 0;
};

using SerializeStateFn = void (*)(RED4ext::IScriptable*, ByteStream*);
using CreateStateFn = RED4ext::Handle<RED4ext::IScriptable>* (*)(RED4ext::Handle<RED4ext::IScriptable>*);

// ---------------------------------------------------------------------------------------------------------------------
// gameuiICharacterCustomizationState's virtual functions, declared exactly as in Tilted Phoques' RED4ext.SDK fork
// (ICharacterCustomizationState.hpp), so the compiler gives them the same table slots CyberpunkMP's build calls.
// Only the Get*Customization functions are used. Never instantiated.

struct AppearanceKey // world::EntityAppearanceChangeParameter::Key
{
    RED4ext::ResourcePath resource;
    CName definition;
};

struct CustomizationState : RED4ext::IScriptable
{
    virtual bool IsFinalized() const = 0;                                                         // 108
    virtual const RED4ext::TweakDBID& GetLifePath() const = 0;                                    // 110
    virtual void SetLifePath(const RED4ext::TweakDBID&) = 0;                                      // 118
    virtual bool IsBodyGenderMale() const = 0;                                                    // 120
    virtual void SetIsBodyGenderMale(bool) = 0;                                                   // 128
    virtual bool IsBrainGenderMale() const = 0;                                                   // 130
    virtual void SetIsBrainGenderMale(bool) = 0;                                                  // 138
    virtual void sub_140() = 0;                                                                   // 140
    virtual void sub_148() = 0;                                                                   // 148
    virtual unsigned int GetAttributePointsAvailable() const = 0;                                 // 150
    virtual void SetAttributePointsAvailable(unsigned int) = 0;                                   // 158
    virtual unsigned int GetAttribute(uint32_t) const = 0;                                        // 160
    virtual void SetAttribute(uint32_t, unsigned int) = 0;                                        // 168
    virtual RED4ext::DynArray<uint64_t> GetAttributes() const = 0;                                // 170
    virtual void ClearAttributes() = 0;                                                           // 178
    virtual void sub_180() = 0;                                                                   // 180
    virtual void sub_188() = 0;                                                                   // 188
    virtual bool GetHeadCustomization(CName, bool, RED4ext::DynArray<AppearanceKey>&) const = 0; // 190
    virtual bool GetHeadCustomization(CName, bool, RED4ext::DynArray<uint64_t>&) const = 0;      // 198
    virtual bool GetBodyCustomization(CName, bool, RED4ext::DynArray<AppearanceKey>&) const = 0; // 1A0
    virtual bool GetBodyCustomization(CName, bool, RED4ext::DynArray<uint64_t>&) const = 0;      // 1A8
    virtual bool GetArmsCustomization(CName, bool, RED4ext::DynArray<AppearanceKey>&) const = 0; // 1B0
    virtual bool GetArmsCustomization(CName, bool, RED4ext::DynArray<uint64_t>&) const = 0;      // 1B8
};

template<typename T>
struct KeySpan
{
    T* start;
    T* end;
};

using ScheduleChangesFn = void (*)(RED4ext::IScriptable* aChanger, RED4ext::WeakHandle<RED4ext::IScriptable>& aEntity,
                                   KeySpan<AppearanceKey>* aOld, KeySpan<AppearanceKey>* aNew,
                                   const std::function<void()>& aDone, uint32_t aPriority);

using ItemAppearanceNameFn = RED4ext::CString* (*)(RED4ext::CString*, const RED4ext::Handle<RED4ext::IScriptable>&,
                                                   const RED4ext::Handle<RED4ext::IScriptable>&,
                                                   const RED4ext::Handle<RED4ext::IScriptable>&,
                                                   const RED4ext::ItemID&);

// ---------------------------------------------------------------------------------------------------------------------
// Calling script natives through RTTI with every parameter filled in.

struct Arg
{
    const char* typeName; // expected parameter type, or nullptr for any handle (ref<> / wref<>)
    void* value;
};

struct NamedArg
{
    const char* name;     // parameter name
    const char* typeName; // only used if the parameter has this type
    void* value;
};

bool IsHandleType(RED4ext::CBaseRTTIType* aType)
{
    const auto kind = aType->GetType();
    return kind == RED4ext::ERTTIType::Handle || kind == RED4ext::ERTTIType::WeakHandle;
}

// Calls aFunc on aInstance (nullptr for a static function) with aLeading as its first arguments; every later
// parameter is aNamed's value for a parameter of that name and type, or the default value of its type. Returns false
// (with a reason) if the function takes fewer parameters or other types than given.
bool CallFilled(RED4ext::IScriptable* aInstance, RED4ext::CBaseFunction* aFunc, void* aOut,
                std::initializer_list<Arg> aLeading, std::initializer_list<NamedArg> aNamed, std::string& aError)
{
    if (!aFunc)
    {
        aError = "function not found";
        return false;
    }
    const uint32_t count = aFunc->params.Size();
    if (aLeading.size() > count)
    {
        aError = "takes " + std::to_string(count) + " parameter(s), not " + std::to_string(aLeading.size());
        return false;
    }

    // Storage for the defaults, constructed and destructed by their RTTI types.
    struct Default
    {
        RED4ext::CBaseRTTIType* type;
        std::unique_ptr<uint8_t[]> storage;
        void* value;
    };
    std::vector<Default> defaults;
    RED4ext::StackArgs_t args;
    uint32_t index = 0;
    for (const auto& arg : aLeading)
    {
        auto* type = aFunc->params[index]->type;
        const bool fits = arg.typeName ? type->GetName() == CName(arg.typeName) : IsHandleType(type);
        if (!fits)
        {
            aError = "parameter " + std::to_string(index + 1) + " is " + type->GetName().ToString();
            return false;
        }
        args.emplace_back(type, arg.value);
        ++index;
    }
    for (; index < count; ++index)
    {
        auto* param = aFunc->params[index];
        void* value = nullptr;
        for (const auto& named : aNamed)
        {
            if (param->name == CName(named.name) && param->type->GetName() == CName(named.typeName))
                value = named.value;
        }
        if (!value)
        {
            Default entry{param->type, std::make_unique<uint8_t[]>(param->type->GetSize() + 16), nullptr};
            entry.value = reinterpret_cast<void*>((reinterpret_cast<uintptr_t>(entry.storage.get()) + 15)
                                                  & ~uintptr_t{15}); // engine types are aligned to 16 at most
            param->type->Construct(entry.value);
            value = entry.value;
            defaults.push_back(std::move(entry));
        }
        args.emplace_back(param->type, value);
    }
    const bool ok = RED4ext::ExecuteFunction(aInstance, aFunc, aOut, args);
    for (auto& entry : defaults)
        entry.type->Destruct(entry.value);
    if (!ok)
        aError = "the call failed";
    return ok;
}

RED4ext::CBaseFunction* FunctionOf(RED4ext::IScriptable* aInstance, const char* aName)
{
    return aInstance ? aInstance->GetType()->GetFunction(CName(aName)) : nullptr;
}

RED4ext::Handle<RED4ext::IScriptable> TransactionSystem()
{
    Red::ScriptGameInstance game;
    Red::Handle<Red::IScriptable> system;
    Red::CallStatic("ScriptGameInstance", "GetTransactionSystem", system, game);
    return system;
}

std::string RecordName(uint64_t aId)
{
    RED4ext::TweakDBID id(aId);
    Red::CString text;
    if (Red::CallStatic("gamedataTDBIDHelper", "ToStringDEBUG", text, id))
        return text.c_str();
    return {};
}

// The attachment slots a look is read from: every slot that dresses V's body (rounds I and K), no weapons.
const std::vector<uint64_t>& LookSlots()
{
    static const std::vector<uint64_t> slots = {
        TweakDbId("AttachmentSlots.TppHead"),   TweakDbId("AttachmentSlots.Head"),
        TweakDbId("AttachmentSlots.Face"),      TweakDbId("AttachmentSlots.Eyes"),
        TweakDbId("AttachmentSlots.Chest"),     TweakDbId("AttachmentSlots.Torso"),
        TweakDbId("AttachmentSlots.Legs"),      TweakDbId("AttachmentSlots.Feet"),
        TweakDbId("AttachmentSlots.Outfit"),    TweakDbId("AttachmentSlots.UnderwearTop"),
        TweakDbId("AttachmentSlots.UnderwearBottom"), TweakDbId("AttachmentSlots.RightArm"),
        TweakDbId("AttachmentSlots.LeftArm"),
    };
    return slots;
}

// The local V's customization state: CyberpunkMP reads the handle the customization system keeps at +0x78 (patch
// 2.2), checked here by class before use.
RED4ext::Handle<RED4ext::IScriptable> LocalCustomizationState(std::string& aError)
{
    auto* systemClass = ClassOf("gameuiCharacterCustomizationSystem");
    auto* stateClass = ClassOf("gameuiCharacterCustomizationState");
    auto* engine = RED4ext::CGameEngine::Get();
    if (!systemClass || !stateClass || !engine || !engine->framework || !engine->framework->gameInstance)
    {
        aError = "customization system class not found";
        return {};
    }
    auto* system = engine->framework->gameInstance->GetSystem(systemClass);
    if (!system)
    {
        aError = "customization system not running";
        return {};
    }
    auto* slot = reinterpret_cast<RED4ext::Handle<RED4ext::IScriptable>*>(reinterpret_cast<uintptr_t>(system) + 0x78);
    if (!SafeIsA(slot->instance, stateClass))
    {
        aError = "customization state not found at +0x78 (layout changed since patch 2.2?)";
        return {};
    }
    return *slot;
}

// The world's entity appearance changer: runtime system 34 of the runtime scene (CyberpunkMP for patch 2.2, and
// Codeware's mapping for 2.31), checked by class before use.
RED4ext::IScriptable* AppearanceChanger()
{
    auto* engine = RED4ext::CGameEngine::Get();
    auto* changerClass = ClassOf("worldRuntimeSystemEntityAppearanceChanger");
    if (!engine || !engine->framework || !engine->framework->unk18 || !changerClass)
        return nullptr;
    auto* systems = reinterpret_cast<RED4ext::Handle<RED4ext::IScriptable>*>(engine->framework->unk18);
    auto* changer = systems[34].instance;
    return SafeIsA(changer, changerClass) ? changer : nullptr;
}
} // namespace

// ---------------------------------------------------------------------------------------------------------------------

Red::Handle<Red::IScriptable> LocalPlayerHandle()
{
    Red::ScriptGameInstance game;
    Red::Handle<Red::IScriptable> system;
    if (!Red::CallStatic("ScriptGameInstance", "GetPlayerSystem", system, game) || !system)
        return {};
    Red::Handle<Red::IScriptable> player;
    if (!Red::CallVirtual(system.instance, "GetLocalPlayerMainGameObject", player) || !player)
    {
        if (!Red::CallVirtual(system.instance, "GetLocalPlayerControlledGameObject", player) || !player)
            return {};
    }
    return player;
}

Looks& Looks::Get()
{
    static auto* instance = new Looks(); // never destroyed (engine handles in flight at DLL unload)
    return *instance;
}

void Looks::SetOptions(const LookOptions& aOptions)
{
    std::scoped_lock lock(m_mutex);
    m_options = aOptions;
}

LookOptions Looks::Options() const
{
    std::scoped_lock lock(m_mutex);
    return m_options;
}

bool Looks::CaptureLocal(const Red::Handle<Red::IScriptable>& aPlayer, bool aFemale, Look& aOut, std::string& aError)
{
    aOut = {};
    aOut.female = aFemale;
    aError.clear();
    if (!aPlayer)
    {
        aError = "no V";
        return false;
    }

    // Items: what sits in V's look slots (TransactionSystem.GetItemInSlot, ItemObject.GetItemID).
    const auto transactions = TransactionSystem();
    auto* getItemInSlot = FunctionOf(transactions.instance, "GetItemInSlot");
    Red::Handle<Red::IScriptable> player = aPlayer;
    for (const uint64_t slot : LookSlots())
    {
        RED4ext::TweakDBID slotID(slot);
        Red::Handle<Red::IScriptable> itemObject;
        std::string error;
        if (!CallFilled(transactions.instance, getItemInSlot, &itemObject, {{nullptr, &player}, {"TweakDBID", &slotID}},
                        {}, error))
        {
            aError = "GetItemInSlot: " + error;
            break;
        }
        if (!itemObject)
            continue;
        RED4ext::ItemID itemID{};
        if (!Red::CallVirtual(itemObject.instance, "GetItemID", itemID) || !itemID.tdbid.IsValid())
            continue;
        // The record's name only: the upper bits are an offset into this game's TweakDB.
        aOut.items.push_back({slot, TweakDbName(itemID.tdbid.value)});
    }

    // Customization state, serialized (CyberpunkMP: NetworkService::SendSpawnRequest).
    const auto serialize = Address<SerializeStateFn>(kSerializeState);
    std::string stateError;
    const auto state = serialize ? LocalCustomizationState(stateError) : RED4ext::Handle<RED4ext::IScriptable>{};
    if (!serialize)
        stateError = "the game's customization serializer wasn't found (address " + std::to_string(kSerializeState) + ")";
    if (state)
    {
        ByteWriter writer;
        serialize(state.instance, &writer);
        aOut.customization = std::move(writer.bytes);
    }
    if (!stateError.empty())
        aError += (aError.empty() ? "" : "; ") + stateError;

    m_lastCaptureSize = aOut.customization.size();
    {
        std::scoped_lock lock(m_mutex);
        if (aError != m_lastCaptureError && !aError.empty())
            COOP_LOG_WARN("look: reading your V's look: %s", aError.c_str());
        m_lastCaptureError = aError;
    }
    return !aOut.customization.empty() || !aOut.items.empty();
}

std::string Looks::Apply(const Red::Handle<Red::IScriptable>& aEntity, const Look& aLook)
{
    static auto* objectClass = ClassOf("gameObject");
    static auto* puppetStateClass = ClassOf("gamePuppetPS");
    if (!aEntity || !objectClass || !aEntity->GetType()->IsA(objectClass))
        return "not a game object";
    const auto options = Options();
    std::ostringstream report;
    report << aEntity->GetType()->GetName().ToString() << " (" << (aLook.female ? "female" : "male") << ")";

    // 1. Third person: the byte after hasQuickHackBegunUpload in gamePuppetPS (CyberpunkMP: "checked during item
    //    adding process for &TPP").
    if (options.thirdPerson)
    {
        COOP_LOG_INFO("look: marking the body third person");
        auto& state = reinterpret_cast<RED4ext::game::Object*>(aEntity.instance)->persistentState;
        if (SafeIsA(state.instance, puppetStateClass))
        {
            reinterpret_cast<RED4ext::game::PuppetPS*>(state.instance)->unk72[0] = 1;
            report << "; third person";
        }
        else
        {
            report << "; third person: no puppet state";
        }
    }

    // 2. Items.
    std::string itemReport;
    if (options.items)
        ApplyItems(aEntity, aLook, itemReport);
    else
        itemReport = "items off";
    report << "; " << itemReport;

    // 3. Customization (face, hair, body).
    std::string customizationReport;
    if (options.customization)
        ApplyCustomization(aEntity, aLook, customizationReport);
    else
        customizationReport = "customization off";
    report << "; " << customizationReport;

    ++m_applied;
    const auto text = report.str();
    COOP_LOG_INFO("look: %s", text.c_str());
    {
        std::scoped_lock lock(m_mutex);
        m_lastReport = text;
    }
    return text;
}

bool Looks::ApplyItems(const Red::Handle<Red::IScriptable>& aEntity, const Look& aLook, std::string& aReport)
{
    const auto transactions = TransactionSystem();
    auto* giveItem = FunctionOf(transactions.instance, "GiveItem");
    auto* addItemToSlot = FunctionOf(transactions.instance, "AddItemToSlot");
    if (!giveItem || !addItemToSlot)
    {
        aReport = "items: TransactionSystem.GiveItem/AddItemToSlot not found";
        return false;
    }
    const auto appearanceName = Address<ItemAppearanceNameFn>(kItemAppearanceName);
    auto* tweakDB = RED4ext::TweakDB::Get();

    int added = 0;
    int failed = 0;
    std::string lastError;
    Red::Handle<Red::IScriptable> entity = aEntity;
    for (const auto& item : ThirdPersonLookItems(aLook.items, aLook.female))
    {
        RED4ext::TweakDBID slotID(item.slot);
        RED4ext::TweakDBID itemTdbid(item.item);
        RED4ext::ItemID itemID{};
        if (!Red::CallStatic("gameItemID", "FromTDBID", itemID, itemTdbid) || !itemID.tdbid.IsValid())
        {
            ++failed;
            lastError = "ItemID.FromTDBID failed";
            continue;
        }

        // The item's appearance on this body (CyberpunkMP: the record's appearanceName plus the suffix the game
        // computes for the owner, e.g. &TPP and the gender), when the game's function for it is found.
        CName garment;
        if (appearanceName && tweakDB)
        {
            Red::Handle<Red::IScriptable> record;
            CName base;
            if (tweakDB->TryGetRecord(itemTdbid, record) && record
                && tweakDB->TryGetValue(RED4ext::TweakDBID(itemTdbid, ".appearanceName"), base))
            {
                RED4ext::CString suffix;
                appearanceName(&suffix, entity, entity, record, itemID);
                garment = CName((std::string(base.ToString()) + suffix.c_str()).c_str());
            }
        }

        const std::string name = RecordName(item.item);
        COOP_LOG_INFO("look: GiveItem + AddItemToSlot(%s, %s%s%s)", RecordName(item.slot).c_str(), name.c_str(),
                      garment ? " as " : "", garment ? garment.ToString() : "");
        int32_t quantity = 1;
        bool given = false;
        std::string error;
        if (!CallFilled(transactions.instance, giveItem, &given, {{nullptr, &entity}, {"gameItemID", &itemID},
                                                                  {"Int32", &quantity}},
                        {}, error)
            || !given)
        {
            ++failed;
            lastError = "GiveItem " + name + ": " + (error.empty() ? "returned false" : error);
            continue;
        }
        bool placed = false;
        bool highPriority = true; // CyberpunkMP's AddItemToSlotContext default
        if (!CallFilled(transactions.instance, addItemToSlot, &placed,
                        {{nullptr, &entity}, {"TweakDBID", &slotID}, {"gameItemID", &itemID}},
                        {{"highPriority", "Bool", &highPriority}, {"garmentAppearanceName", "CName", &garment}},
                        error)
            || !placed)
        {
            ++failed;
            lastError = "AddItemToSlot " + name + ": " + (error.empty() ? "returned false" : error);
            continue;
        }
        ++added;
    }
    std::ostringstream out;
    out << "items: " << added << " put on";
    if (failed > 0)
        out << ", " << failed << " failed (last: " << lastError << ")";
    if (!appearanceName)
        out << " (garment names: the game's function wasn't found, the game picks them)";
    aReport = out.str();
    return failed == 0;
}

bool Looks::ApplyCustomization(const Red::Handle<Red::IScriptable>& aEntity, const Look& aLook, std::string& aReport)
{
    if (aLook.customization.empty())
    {
        aReport = "customization: none sent";
        return false;
    }
    const auto serialize = Address<SerializeStateFn>(kSerializeState);
    const auto schedule = Address<ScheduleChangesFn>(kScheduleAppearanceChanges);
    if (!serialize || !schedule)
    {
        aReport = std::string("customization: the game's ") + (serialize ? "appearance changer" : "serializer")
                + " function wasn't found";
        return false;
    }
    auto* changer = AppearanceChanger();
    if (!changer)
    {
        aReport = "customization: the world's appearance changer wasn't found";
        return false;
    }

    // A new state, read from the bytes.
    COOP_LOG_INFO("look: reading the customization state (%u bytes)", static_cast<unsigned>(aLook.customization.size()));
    RED4ext::Handle<RED4ext::IScriptable> state;
    if (const auto create = Address<CreateStateFn>(kCreateState))
        create(&state);
    if (!state)
    {
        if (auto* stateClass = ClassOf("gameuiCharacterCustomizationState"))
            state = Red::MakeScriptedHandle<RED4ext::IScriptable>(stateClass);
    }
    if (!state)
    {
        aReport = "customization: no state could be made";
        return false;
    }
    ByteReader reader(aLook.customization);
    serialize(state.instance, &reader);

    // The third-person parts, as CyberpunkMP picks them (genitals pop through clothes; "character_customization"
    // would be all of them).
    COOP_LOG_INFO("look: collecting head, body and arms customization");
    auto* custom = reinterpret_cast<CustomizationState*>(state.instance);
    RED4ext::DynArray<AppearanceKey> keys;
    for (const char* group : {"TPP", "face", "hairs"})
        custom->GetHeadCustomization(CName(group), true, keys);
    if (!aLook.female)
        custom->GetHeadCustomization(CName("beards"), true, keys);
    for (const char* group : {"TPP_Body", "breast", "lifted_feet", "flat_feet"})
        custom->GetBodyCustomization(CName(group), true, keys);
    for (const char* group : {"holstered_default", "nails"})
        custom->GetArmsCustomization(CName(group), true, keys);
    if (keys.Size() == 0)
    {
        aReport = "customization: the state gave no parts";
        return false;
    }

    COOP_LOG_INFO("look: scheduling %u appearance change(s)", static_cast<unsigned>(keys.Size()));
    RED4ext::WeakHandle<RED4ext::IScriptable> weak = aEntity;
    KeySpan<AppearanceKey> none{nullptr, nullptr};
    KeySpan<AppearanceKey> wanted{&keys[0], &keys[0] + keys.Size()};
    // Keeps the state and the body alive until the change is done (CyberpunkMP's callback does the same).
    const std::function<void()> done = [state, entity = aEntity]() {};
    schedule(changer, weak, &none, &wanted, done, 0);
    aReport = "customization: " + std::to_string(keys.Size()) + " part(s) scheduled";
    return true;
}

std::string Looks::Status() const
{
    std::scoped_lock lock(m_mutex);
    std::ostringstream out;
    out << "looks (CyberpunkMP): third person " << (m_options.thirdPerson ? "on" : "off") << ", items "
        << (m_options.items ? "on" : "off") << ", customization " << (m_options.customization ? "on" : "off")
        << "; game functions: serializer " << (Resolve(kSerializeState) ? "found" : "MISSING") << ", appearance changer "
        << (Resolve(kScheduleAppearanceChanges) ? "found" : "MISSING") << ", item appearance "
        << (Resolve(kItemAppearanceName) ? "found" : "missing") << "; your V's state " << m_lastCaptureSize.load()
        << " bytes; looks applied " << m_applied.load() << "\n";
    if (!m_lastCaptureError.empty())
        out << "  reading your look: " << m_lastCaptureError << "\n";
    if (!m_lastReport.empty())
        out << "  last: " << m_lastReport << "\n";
    return out.str();
}
} // namespace coop::plugin
