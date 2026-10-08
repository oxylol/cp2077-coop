#include "plugin/AnimCapture.hpp"

#include <Windows.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <utility>

#include <RED4ext/Scripting/Natives/entIComponent.hpp>
#include <RED4ext/Scripting/OpcodeHandlers.hpp>

#include "core/Log.hpp"

namespace coop::plugin
{
using Handler = void (*)(void*, RED4ext::CStackFrame&, void*, RED4ext::rtti::IType*);

struct AnimCapture::Hook
{
    RED4ext::CBaseFunction* func = nullptr;
    Handler original = nullptr;
    Handler* entry = nullptr; // the handler table entry we replaced
    AnimInputKind kind = AnimInputKind::Float;
    int ownerParam = -1;      // a GameObject parameter, if any: whose animation this is
    // Whose call it is when no owner parameter says so: the object the method is called on (an entity), the
    // component's entity, or the state machine's execution owner.
    enum class Context : uint8_t { None, Entity, Component, ScriptInterface } context = Context::None;
    bool eventSink = false;   // Entity.QueueEvent: the event says whether it is an animation input
    int nameParam = -1;
    int valueParam = -1;
    std::string name;         // Class::Function
    std::atomic<uint64_t> calls{0};
    std::atomic<uint64_t> captured{0};
    std::atomic<uint64_t> notLocal{0};
    std::atomic<uint64_t> unreadable{0};
    std::atomic<uint64_t> ignored{0}; // event sink: events that aren't animation inputs
};

namespace
{
constexpr size_t kMaxHooks = 32;
constexpr size_t kMaxParams = 8;
constexpr size_t kParamStorage = 64;
constexpr size_t kMaxQueuedEvents = 128;
constexpr size_t kMaxRecordLines = 200'000;

// Where the game feeds animation inputs from scripts, by function name. The kind follows from the name.
//
// Group 1, all hooked together (round G showed the controller's own natives get no calls from scripts):
// * the player state machine's script interface (gamestateMachineGameScriptInterface.SetAnimationParameter*,
//   PushAnimationEvent): how V's locomotion, weapon and other state machines feed V's animation;
// * Entity.QueueEvent: the game's AnimationControllerComponent helpers (ApplyFeature, SetInputFloat, PushEvent, ...)
//   are script functions that queue AnimInputSetter* / AnimExternalEvent events on the entity, so the queue is
//   where those inputs pass;
// * the controller's natives, in case a script calls them directly.
// Group 2 (CD PROJEKT's replication leftovers) is only used if none of group 1 exists: the game's "...ToReplicate"
// helpers call a replication function next to the queue, so hooking both would count those inputs twice.
struct Candidate
{
    const char* name;
    AnimInputKind kind;
    const char* ownerClass; // the function must belong to this class (or one derived from it); nullptr = any
    int group;
    bool eventSink = false;
};
constexpr const char* kController = "entAnimationControllerComponent";
constexpr const char* kStateMachine = "gamestateMachineGameScriptInterface";
constexpr Candidate kCandidates[] = {
    {"SetAnimationParameterFeature", AnimInputKind::Feature, kStateMachine, 1},
    {"SetAnimationParameterFloat", AnimInputKind::Float, kStateMachine, 1},
    {"SetAnimationParameterInt", AnimInputKind::Int, kStateMachine, 1},
    {"SetAnimationParameterBool", AnimInputKind::Bool, kStateMachine, 1},
    {"SetAnimationParameterVector", AnimInputKind::Vector, kStateMachine, 1},
    {"PushAnimationEvent", AnimInputKind::Event, kStateMachine, 1},
    {"QueueEvent", AnimInputKind::Event, "entEntity", 1, true},
    {"ApplyFeature", AnimInputKind::Feature, kController, 1},
    {"SetInputFloat", AnimInputKind::Float, kController, 1},
    {"SetInputInt", AnimInputKind::Int, kController, 1},
    {"SetInputBool", AnimInputKind::Bool, kController, 1},
    {"SetInputVector", AnimInputKind::Vector, kController, 1},
    {"PushEvent", AnimInputKind::Event, kController, 1},
    {"ReplicateAnimFeature", AnimInputKind::Feature, nullptr, 2},
    {"ReplicateAnimEvent", AnimInputKind::Event, nullptr, 2},
    {"ReplicateInputFloat", AnimInputKind::Float, nullptr, 2},
    {"ReplicateInputBool", AnimInputKind::Bool, nullptr, 2},
    {"ReplicateInputInt", AnimInputKind::Int, nullptr, 2},
    {"ReplicateInputVector", AnimInputKind::Vector, nullptr, 2},
};

std::array<AnimCapture::Hook, kMaxHooks> g_hooks;
size_t g_hookCount = 0;
bool g_installed = false;
std::string g_tableInfo; // how the native handler table was found (or why not)
std::vector<std::string> g_scriptOnly; // candidates that exist but are script functions (not capturable this way)
std::atomic<bool> g_enabled{true};

struct Consumer
{
    std::map<uint64_t, AnimInput> latest;
    std::vector<AnimInput> events;
    bool active = true;
};

std::mutex g_mutex; // guards everything below
Consumer g_network;
Consumer g_mirror{{}, {}, false};
uint64_t g_totalCaptured = 0;
bool g_recording = false;
std::chrono::steady_clock::time_point g_recordStart;
std::chrono::steady_clock::time_point g_recordEnd;
std::filesystem::path g_recordFile;
std::ostringstream g_record;
size_t g_recordLines = 0;

template<size_t Index>
void Detour(void* aContext, RED4ext::CStackFrame& aFrame, void* aResult, RED4ext::rtti::IType* aResultType)
{
    auto& hook = g_hooks[Index];
    if (g_enabled.load(std::memory_order_relaxed))
        AnimCapture::Get().OnCall(hook, aContext, aFrame);
    hook.original(aContext, aFrame, aResult, aResultType);
}

template<size_t... I>
constexpr std::array<Handler, sizeof...(I)> MakeDetours(std::index_sequence<I...>)
{
    return {&Detour<I>...};
}
constexpr auto kDetours = MakeDetours(std::make_index_sequence<kMaxHooks>{});

// --- the engine's native function handler table -----------------------------------------------------------------
//
// Each native script function has a registration index into a table of handlers. RED4ext.SDK names the table's
// address (CBaseFunction_Handlers); round F showed that reading it as "address of a pointer to the table", the
// way the SDK does, gives null on 2.31. So both readings are checked: the right one maps the native functions of a
// well-known class (gameObject) to code inside the game's executable.

struct ImageRange
{
    uintptr_t begin = 0;
    uintptr_t end = 0;
    [[nodiscard]] bool Contains(const void* aPtr) const
    {
        const auto value = reinterpret_cast<uintptr_t>(aPtr);
        return value >= begin && value < end;
    }
};

ImageRange GameImage()
{
    ImageRange range;
    auto* base = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
    if (!base)
        return range;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    range.begin = reinterpret_cast<uintptr_t>(base);
    range.end = range.begin + nt->OptionalHeader.SizeOfImage;
    return range;
}

bool Readable(const void* aPtr, size_t aSize)
{
    if (!aPtr)
        return false;
    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery(aPtr, &info, sizeof(info)) || info.State != MEM_COMMIT)
        return false;
    if (info.Protect & (PAGE_NOACCESS | PAGE_GUARD))
        return false;
    const auto end = reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize;
    return reinterpret_cast<uintptr_t>(aPtr) + aSize <= end;
}

// How many of the sample functions' handlers this table puts inside the game's code.
int ScoreTable(Handler* aTable, const std::vector<RED4ext::CBaseFunction*>& aSample, const ImageRange& aImage)
{
    if (!aTable)
        return 0;
    int score = 0;
    for (auto* func : aSample)
    {
        const auto index = func->GetRegIndex();
        auto* entry = aTable + index;
        if (Readable(entry, sizeof(Handler)) && *entry && aImage.Contains(reinterpret_cast<const void*>(*entry)))
            ++score;
    }
    return score;
}

Handler* FindHandlerTable()
{
    auto* gameObject = RED4ext::CRTTISystem::Get()->GetClass(RED4ext::CName("gameObject"));
    if (!gameObject)
    {
        g_tableInfo = "class gameObject not found";
        return nullptr;
    }
    std::vector<RED4ext::CBaseFunction*> sample;
    for (auto* func : gameObject->funcs)
    {
        if (func->flags.isNative && sample.size() < 64)
            sample.push_back(func);
    }
    if (sample.empty())
    {
        g_tableInfo = "gameObject has no native functions to check the table with";
        return nullptr;
    }

    RED4ext::UniversalRelocPtr<Handler*> reloc(RED4ext::Detail::AddressHashes::CBaseFunction_Handlers);
    Handler** address = reloc.GetAddr();
    Handler* asPointer = Readable(address, sizeof(Handler*)) ? *address : nullptr;
    auto* asArray = reinterpret_cast<Handler*>(address);

    const auto image = GameImage();
    const int pointerScore = ScoreTable(asPointer, sample, image);
    const int arrayScore = ScoreTable(asArray, sample, image);
    std::ostringstream info;
    info << "handler table: as pointer " << pointerScore << "/" << sample.size() << ", as array " << arrayScore << "/"
         << sample.size();
    const int needed = static_cast<int>(sample.size()) * 3 / 4;
    Handler* table = nullptr;
    if (pointerScore >= needed && pointerScore >= arrayScore)
    {
        table = asPointer;
        info << " -> pointer";
    }
    else if (arrayScore >= needed)
    {
        table = asArray;
        info << " -> array";
    }
    else
    {
        info << " -> neither fits, capture off";
    }
    g_tableInfo = info.str();
    return table;
}

void WriteEntry(Handler* aEntry, Handler aValue)
{
    DWORD old = 0;
    if (VirtualProtect(aEntry, sizeof(Handler), PAGE_READWRITE, &old))
    {
        *aEntry = aValue;
        DWORD ignored = 0;
        VirtualProtect(aEntry, sizeof(Handler), old, &ignored);
    }
    else
    {
        *aEntry = aValue; // already writable (heap)
    }
}

const char* NameText(RED4ext::CName aName)
{
    const char* text = aName.ToString();
    return text ? text : "?";
}

RED4ext::CClass* ClassOf(const char* aName)
{
    return RED4ext::CRTTISystem::Get()->GetClass(RED4ext::CName(aName));
}

bool IsHandle(RED4ext::rtti::IType* aType)
{
    const auto meta = aType->GetType();
    return meta == RED4ext::rtti::ERTTIType::Handle || meta == RED4ext::rtti::ERTTIType::WeakHandle;
}

RED4ext::CClass* HandleClass(RED4ext::rtti::IType* aType)
{
    RED4ext::rtti::IType* inner = nullptr;
    if (aType->GetType() == RED4ext::rtti::ERTTIType::Handle)
        inner = static_cast<RED4ext::CRTTIHandleType*>(aType)->GetInnerType();
    else if (aType->GetType() == RED4ext::rtti::ERTTIType::WeakHandle)
        inner = static_cast<RED4ext::CRTTIWeakHandleType*>(aType)->GetInnerType();
    return inner && inner->GetType() == RED4ext::rtti::ERTTIType::Class ? static_cast<RED4ext::CClass*>(inner) : nullptr;
}

// The object a handle parameter points to (nullptr if empty or expired).
RED4ext::IScriptable* HandleInstance(RED4ext::rtti::IType* aType, void* aStorage)
{
    if (aType->GetType() == RED4ext::rtti::ERTTIType::Handle)
        return reinterpret_cast<RED4ext::Handle<RED4ext::IScriptable>*>(aStorage)->instance;
    if (aType->GetType() == RED4ext::rtti::ERTTIType::WeakHandle)
    {
        auto* weak = reinterpret_cast<RED4ext::WeakHandle<RED4ext::IScriptable>*>(aStorage);
        return weak->Expired() ? nullptr : weak->instance;
    }
    return nullptr;
}

// The local V, set every frame from the player system (CoopSystem::OnTick). A class check isn't enough: bodies
// spawned from PlayerPuppet records (TPP_Player, the photo mode puppet) are PlayerPuppets too, and their inputs
// would be captured as V's (round I). Until the player is known, the class check stands in.
std::atomic<RED4ext::IScriptable*> g_localPlayer{nullptr};

bool IsLocalPlayer(RED4ext::IScriptable* aObject)
{
    if (!aObject)
        return false;
    if (auto* player = g_localPlayer.load(std::memory_order_relaxed))
        return aObject == player;
    static auto* playerClass = ClassOf("PlayerPuppet");
    return playerClass && aObject->GetType()->IsA(playerClass);
}

// Who a call on aContext is for (see Hook::Context); nullptr if unknown.
RED4ext::IScriptable* ContextOwner(AnimCapture::Hook::Context aKind, void* aContext)
{
    auto* context = static_cast<RED4ext::IScriptable*>(aContext);
    if (!context)
        return nullptr;
    switch (aKind)
    {
    case AnimCapture::Hook::Context::Entity: return context;
    case AnimCapture::Hook::Context::Component:
        return reinterpret_cast<RED4ext::IScriptable*>(static_cast<RED4ext::ent::IComponent*>(aContext)->owner);
    case AnimCapture::Hook::Context::ScriptInterface:
    {
        static auto* property = []() -> RED4ext::CProperty* {
            auto* cls = ClassOf("gamestateMachineGameScriptInterface");
            if (!cls)
                return nullptr;
            auto* found = cls->GetProperty(RED4ext::CName("executionOwner"));
            return found ? found : cls->GetProperty(RED4ext::CName("owner"));
        }();
        if (!property || (property->type->GetType() != RED4ext::rtti::ERTTIType::Handle
                          && property->type->GetType() != RED4ext::rtti::ERTTIType::WeakHandle))
            return nullptr;
        return HandleInstance(property->type, property->GetValuePtr<void>(context));
    }
    default: return nullptr;
    }
}

// --- reading and writing property values -------------------------------------------------------------------------

bool ReadValue(RED4ext::rtti::IType* aType, const void* aPtr, AnimValue& aOut)
{
    using RED4ext::CName;
    using RED4ext::rtti::ERTTIType;
    const auto meta = aType->GetType();
    const auto name = aType->GetName();
    if (meta == ERTTIType::Fundamental)
    {
        if (name == CName("Float"))
            aOut = AnimValue::FromFloat(*static_cast<const float*>(aPtr));
        else if (name == CName("Double"))
            aOut = AnimValue::FromFloat(static_cast<float>(*static_cast<const double*>(aPtr)));
        else if (name == CName("Bool"))
            aOut = AnimValue::FromBool(*static_cast<const bool*>(aPtr));
        else if (name == CName("Int32"))
            aOut = AnimValue::FromInt(*static_cast<const int32_t*>(aPtr));
        else if (name == CName("Uint32"))
            aOut = AnimValue::FromInt(*static_cast<const uint32_t*>(aPtr));
        else if (name == CName("Int16"))
            aOut = AnimValue::FromInt(*static_cast<const int16_t*>(aPtr));
        else if (name == CName("Uint16"))
            aOut = AnimValue::FromInt(*static_cast<const uint16_t*>(aPtr));
        else if (name == CName("Int8"))
            aOut = AnimValue::FromInt(*static_cast<const int8_t*>(aPtr));
        else if (name == CName("Uint8"))
            aOut = AnimValue::FromInt(*static_cast<const uint8_t*>(aPtr));
        else if (name == CName("Int64"))
            aOut = AnimValue::FromInt(*static_cast<const int64_t*>(aPtr));
        else if (name == CName("Uint64"))
            aOut = AnimValue::FromInt(static_cast<int64_t>(*static_cast<const uint64_t*>(aPtr)));
        else
            return false;
        return true;
    }
    if (meta == ERTTIType::Name)
    {
        aOut = AnimValue::FromName(static_cast<const CName*>(aPtr)->hash);
        return true;
    }
    if (meta == ERTTIType::Enum)
    {
        int64_t value = 0;
        switch (static_cast<RED4ext::CEnum*>(aType)->actualSize)
        {
        case 1: value = *static_cast<const int8_t*>(aPtr); break;
        case 2: value = *static_cast<const int16_t*>(aPtr); break;
        case 4: value = *static_cast<const int32_t*>(aPtr); break;
        case 8: value = *static_cast<const int64_t*>(aPtr); break;
        default: return false;
        }
        aOut = AnimValue::FromInt(value);
        return true;
    }
    if (meta == ERTTIType::Class && (name == CName("Vector4") || name == CName("Quaternion")))
    {
        const auto* f = static_cast<const float*>(aPtr);
        aOut = AnimValue::FromVector(f[0], f[1], f[2], f[3]);
        return true;
    }
    if (meta == ERTTIType::Class && name == CName("Vector3"))
    {
        const auto* f = static_cast<const float*>(aPtr);
        aOut = AnimValue::FromVector(f[0], f[1], f[2], 0.0f);
        return true;
    }
    return false;
}

bool WriteValue(RED4ext::rtti::IType* aType, void* aPtr, const AnimValue& aValue)
{
    using RED4ext::CName;
    using RED4ext::rtti::ERTTIType;
    const auto meta = aType->GetType();
    const auto name = aType->GetName();
    const auto i = aValue.i;
    const auto f = aValue.f[0];
    if (meta == ERTTIType::Fundamental)
    {
        if (name == CName("Float"))
            *static_cast<float*>(aPtr) = aValue.type == AnimValueType::Float ? f : static_cast<float>(i);
        else if (name == CName("Double"))
            *static_cast<double*>(aPtr) = aValue.type == AnimValueType::Float ? f : static_cast<double>(i);
        else if (name == CName("Bool"))
            *static_cast<bool*>(aPtr) = aValue.type == AnimValueType::Float ? f != 0.0f : i != 0;
        else if (name == CName("Int32"))
            *static_cast<int32_t*>(aPtr) = static_cast<int32_t>(i);
        else if (name == CName("Uint32"))
            *static_cast<uint32_t*>(aPtr) = static_cast<uint32_t>(i);
        else if (name == CName("Int16"))
            *static_cast<int16_t*>(aPtr) = static_cast<int16_t>(i);
        else if (name == CName("Uint16"))
            *static_cast<uint16_t*>(aPtr) = static_cast<uint16_t>(i);
        else if (name == CName("Int8"))
            *static_cast<int8_t*>(aPtr) = static_cast<int8_t>(i);
        else if (name == CName("Uint8"))
            *static_cast<uint8_t*>(aPtr) = static_cast<uint8_t>(i);
        else if (name == CName("Int64"))
            *static_cast<int64_t*>(aPtr) = i;
        else if (name == CName("Uint64"))
            *static_cast<uint64_t*>(aPtr) = static_cast<uint64_t>(i);
        else
            return false;
        return true;
    }
    if (meta == ERTTIType::Name)
    {
        *static_cast<CName*>(aPtr) = CName(static_cast<uint64_t>(i));
        return true;
    }
    if (meta == ERTTIType::Enum)
    {
        switch (static_cast<RED4ext::CEnum*>(aType)->actualSize)
        {
        case 1: *static_cast<int8_t*>(aPtr) = static_cast<int8_t>(i); return true;
        case 2: *static_cast<int16_t*>(aPtr) = static_cast<int16_t>(i); return true;
        case 4: *static_cast<int32_t*>(aPtr) = static_cast<int32_t>(i); return true;
        case 8: *static_cast<int64_t*>(aPtr) = i; return true;
        default: return false;
        }
    }
    if (meta == ERTTIType::Class && (name == CName("Vector4") || name == CName("Quaternion")))
    {
        std::memcpy(aPtr, aValue.f, sizeof(float) * 4);
        return true;
    }
    if (meta == ERTTIType::Class && name == CName("Vector3"))
    {
        std::memcpy(aPtr, aValue.f, sizeof(float) * 3);
        return true;
    }
    return false;
}

// Every readable property of an AnimFeature object, its own and its parents'.
void ReadFeature(RED4ext::IScriptable* aFeature, AnimInput& aOut)
{
    auto* cls = aFeature->GetType();
    aOut.featureClass = cls->name.hash;
    static const RED4ext::CName stopAt[] = {RED4ext::CName("animAnimFeature"), RED4ext::CName("IScriptable"),
                                            RED4ext::CName("ISerializable")};
    for (auto* c = cls; c; c = c->parent)
    {
        bool stop = false;
        for (const auto& name : stopAt)
            stop = stop || c->name == name;
        if (stop)
            break;
        for (auto* prop : c->props)
        {
            if (aOut.props.size() >= static_cast<size_t>(kMaxAnimProps))
                return;
            AnimValue value;
            if (ReadValue(prop->type, prop->GetValuePtr<void>(aFeature), value))
                aOut.props.push_back({prop->name.hash, value});
        }
    }
}

// An event queued on an entity, read as an animation input: AnimInputSetter* (key + value; the game's
// AnimationControllerComponent helpers queue these) or AnimExternalEvent (name). Returns false if the event is
// something else; aOk says whether a relevant one could be read.
bool ReadEvent(RED4ext::IScriptable* aEvent, AnimInput& aOut, bool& aOk)
{
    using RED4ext::CName;
    static auto* setter = ClassOf("entAnimInputSetter");
    static auto* external = ClassOf("entAnimExternalEvent");
    aOk = false;
    if (!aEvent)
        return false;
    auto* cls = aEvent->GetType();
    if (external && cls->IsA(external))
    {
        auto* name = cls->GetProperty(CName("name"));
        if (!name || name->type->GetType() != RED4ext::rtti::ERTTIType::Name)
            return true;
        aOut.kind = AnimInputKind::Event;
        aOut.name = name->GetValuePtr<CName>(aEvent)->hash;
        aOk = true;
        return true;
    }
    if (!setter || !cls->IsA(setter))
        return false;
    auto* key = cls->GetProperty(CName("key"));
    auto* value = cls->GetProperty(CName("value"));
    if (!key || !value || key->type->GetType() != RED4ext::rtti::ERTTIType::Name)
        return false;
    aOut.name = key->GetValuePtr<CName>(aEvent)->hash;
    void* valuePtr = value->GetValuePtr<void>(aEvent);
    if (IsHandle(value->type))
    {
        auto* feature = HandleInstance(value->type, valuePtr);
        if (!feature)
            return true;
        aOut.kind = AnimInputKind::Feature;
        ReadFeature(feature, aOut);
        aOk = true;
        return true;
    }
    if (value->type->GetName() == CName("Quaternion"))
        return false; // no quaternion inputs on the wire yet
    if (!ReadValue(value->type, valuePtr, aOut.value))
        return true;
    switch (aOut.value.type)
    {
    case AnimValueType::Float: aOut.kind = AnimInputKind::Float; break;
    case AnimValueType::Int: aOut.kind = AnimInputKind::Int; break;
    case AnimValueType::Bool: aOut.kind = AnimInputKind::Bool; break;
    case AnimValueType::Vector: aOut.kind = AnimInputKind::Vector; break;
    default: return false;
    }
    aOk = true;
    return true;
}

std::string ValueText(const AnimValue& aValue)
{
    std::ostringstream out;
    switch (aValue.type)
    {
    case AnimValueType::Float: out << aValue.f[0]; break;
    case AnimValueType::Int: out << aValue.i; break;
    case AnimValueType::Bool: out << (aValue.i ? "true" : "false"); break;
    case AnimValueType::Name: out << NameText(RED4ext::CName(static_cast<uint64_t>(aValue.i))); break;
    case AnimValueType::Vector:
        out << "(" << aValue.f[0] << ", " << aValue.f[1] << ", " << aValue.f[2] << ", " << aValue.f[3] << ")";
        break;
    default: out << "?"; break;
    }
    return out.str();
}

const char* KindText(AnimInputKind aKind)
{
    switch (aKind)
    {
    case AnimInputKind::Feature: return "feature";
    case AnimInputKind::Float: return "float";
    case AnimInputKind::Int: return "int";
    case AnimInputKind::Bool: return "bool";
    case AnimInputKind::Vector: return "vector";
    case AnimInputKind::Event: return "event";
    default: return "?";
    }
}

void FinishRecordingLocked()
{
    if (!g_recording)
        return;
    g_recording = false;
    std::ofstream file(g_recordFile, std::ios::trunc);
    file << "# Cp2077Coop animation inputs captured from the local V (" << g_recordLines << " lines)\n";
    file << "# seconds  function  kind  name  [feature class]  value / properties\n";
    file << g_record.str();
    g_record.str({});
    COOP_LOG_INFO("animation recording written: %s (%zu lines)", g_recordFile.string().c_str(), g_recordLines);
}

void Push(Consumer& aConsumer, const AnimInput& aInput)
{
    if (!aConsumer.active)
        return;
    if (aInput.kind == AnimInputKind::Event)
    {
        if (aConsumer.events.size() < kMaxQueuedEvents)
            aConsumer.events.push_back(aInput);
    }
    else
    {
        aConsumer.latest[aInput.Key()] = aInput;
    }
}

void Drain(Consumer& aConsumer, std::vector<AnimInput>& aOut)
{
    aOut.clear();
    for (auto& [key, input] : aConsumer.latest)
        aOut.push_back(std::move(input));
    // The same event twice in a row within one frame came in through two capture points (e.g. a state machine's
    // PushAnimationEvent and the event queue): keep one.
    for (const auto& event : aConsumer.events)
    {
        if (!aOut.empty() && aOut.back().kind == AnimInputKind::Event && aOut.back().name == event.name)
            continue;
        aOut.push_back(event);
    }
    aConsumer.latest.clear();
    aConsumer.events.clear();
}
} // namespace

AnimCapture& AnimCapture::Get()
{
    static AnimCapture instance;
    return instance;
}

void AnimCapture::Install()
{
    if (g_installed)
        return;
    g_installed = true;

    auto* rtti = RED4ext::CRTTISystem::Get();
    auto* gameObject = ClassOf("gameObject");
    auto* component = ClassOf("entIComponent");
    auto* entity = ClassOf("entEntity");
    auto* stateMachine = ClassOf(kStateMachine);
    if (!rtti || !gameObject || !component)
    {
        g_tableInfo = "RTTI classes not found";
        COOP_LOG_ERROR("animation capture: RTTI classes not found, capture off");
        return;
    }
    Handler* table = FindHandlerTable();
    COOP_LOG_INFO("animation capture: %s", g_tableInfo.c_str());
    if (!table)
        return;

    RED4ext::DynArray<RED4ext::CClass*> classes;
    rtti->GetClasses(nullptr, classes, nullptr, true);

    auto tryHook = [&](RED4ext::CClass* aClass, RED4ext::CBaseFunction* aFunc, const Candidate& aCandidate)
    {
        const std::string fullName = std::string(NameText(aClass->name)) + "::" + aCandidate.name;
        if (!aFunc->flags.isNative)
        {
            g_scriptOnly.push_back(fullName);
            return;
        }
        if (g_hookCount >= kMaxHooks || aFunc->params.Size() > kMaxParams)
            return;
        for (size_t i = 0; i < g_hookCount; ++i)
        {
            if (g_hooks[i].func == aFunc)
                return; // inherited: the same function seen through a derived class
        }
        Handler* entry = &table[aFunc->GetRegIndex()];
        if (!Readable(entry, sizeof(Handler)) || !*entry)
            return;

        auto& hook = g_hooks[g_hookCount]; // a slot an earlier, rejected candidate may have half filled
        hook.ownerParam = -1;
        hook.nameParam = -1;
        hook.valueParam = -1;
        hook.func = aFunc;
        hook.kind = aCandidate.kind;
        hook.name = fullName;
        hook.eventSink = aCandidate.eventSink;
        hook.context = AnimCapture::Hook::Context::None;
        if (!aFunc->flags.isStatic)
        {
            if (aClass->IsA(component))
                hook.context = AnimCapture::Hook::Context::Component;
            else if (entity && aClass->IsA(entity))
                hook.context = AnimCapture::Hook::Context::Entity;
            else if (stateMachine && aClass->IsA(stateMachine))
                hook.context = AnimCapture::Hook::Context::ScriptInterface;
        }
        for (uint32_t i = 0; i < aFunc->params.Size(); ++i)
        {
            auto* type = aFunc->params[i]->type;
            if (IsHandle(type) && hook.ownerParam < 0)
            {
                auto* inner = HandleClass(type);
                if (inner && inner->IsA(gameObject))
                {
                    hook.ownerParam = static_cast<int>(i);
                    continue;
                }
            }
            if (type->GetType() == RED4ext::rtti::ERTTIType::Name && hook.nameParam < 0)
                hook.nameParam = static_cast<int>(i);
        }
        if (hook.eventSink)
        {
            // QueueEvent(evt): the event handle is the only thing read.
            hook.ownerParam = -1;
            for (uint32_t i = 0; i < aFunc->params.Size() && hook.valueParam < 0; ++i)
            {
                if (IsHandle(aFunc->params[i]->type))
                    hook.valueParam = static_cast<int>(i);
            }
            if (hook.valueParam < 0 || hook.context != AnimCapture::Hook::Context::Entity)
                return;
        }
        else if (hook.nameParam < 0)
        {
            return;
        }
        if (!hook.eventSink && aCandidate.kind != AnimInputKind::Event
            && static_cast<uint32_t>(hook.nameParam + 1) < aFunc->params.Size())
            hook.valueParam = hook.nameParam + 1;
        if (!hook.eventSink && aCandidate.kind != AnimInputKind::Event && hook.valueParam < 0)
            return;

        hook.original = *entry;
        hook.entry = entry;
        WriteEntry(entry, kDetours[g_hookCount]);
        static const char* contextText[] = {"", ", owner = the entity", ", owner = the component's entity",
                                            ", owner = the state machine's execution owner"};
        COOP_LOG_INFO("animation capture: routed %s (owner param %d, name %d, value %d%s%s)", fullName.c_str(),
                      hook.ownerParam, hook.nameParam, hook.valueParam,
                      contextText[static_cast<int>(hook.context)], hook.eventSink ? ", event queue" : "");
        ++g_hookCount;
    };

    for (int group = 1; group <= 2 && g_hookCount == 0; ++group)
    {
        for (auto* cls : classes)
        {
            for (const auto& candidate : kCandidates)
            {
                if (candidate.group != group)
                    continue;
                if (candidate.ownerClass)
                {
                    auto* owner = ClassOf(candidate.ownerClass);
                    if (!owner || !cls->IsA(owner))
                        continue;
                }
                const RED4ext::CName name(candidate.name);
                for (auto* func : cls->funcs)
                {
                    if (func->shortName == name)
                        tryHook(cls, func, candidate);
                }
                for (auto* func : cls->staticFuncs)
                {
                    if (func->shortName == name)
                        tryHook(cls, func, candidate);
                }
            }
        }
    }
    COOP_LOG_INFO("animation capture: %zu capture point(s), %zu script-only candidate(s)", g_hookCount,
                  g_scriptOnly.size());
}

void AnimCapture::Uninstall()
{
    for (size_t i = 0; i < g_hookCount; ++i)
    {
        auto& hook = g_hooks[i];
        if (hook.entry && hook.original)
            WriteEntry(hook.entry, hook.original);
        hook.entry = nullptr;
    }
    g_hookCount = 0;
}

void AnimCapture::SetEnabled(bool aEnabled)
{
    g_enabled = aEnabled;
}

bool AnimCapture::Enabled() const
{
    return g_enabled;
}

void AnimCapture::OnCall(Hook& aHook, void* aContext, RED4ext::CStackFrame& aFrame)
{
    ++aHook.calls;

    // Whose call it is, decided before reading any argument when no owner parameter says so: the event queue sees
    // every entity's events, and every other character's are passed on at once.
    if (aHook.ownerParam < 0 && aHook.context != Hook::Context::None)
    {
        if (!IsLocalPlayer(ContextOwner(aHook.context, aContext)))
        {
            ++aHook.notLocal;
            return;
        }
    }

    const auto& params = aHook.func->params;
    const uint32_t count = params.Size();
    for (uint32_t i = 0; i < count; ++i)
    {
        if (params[i]->type->GetSize() > kParamStorage || params[i]->type->GetAlignment() > 16)
        {
            ++aHook.unreadable;
            return;
        }
    }

    // Evaluate the arguments into our own storage, then rewind the frame so the original reads them again.
    alignas(16) uint8_t storage[kMaxParams][kParamStorage];
    char* const savedCode = aFrame.code;
    void* const savedData = aFrame.data;
    auto* const savedDataType = aFrame.dataType;
    const auto savedParam = aFrame.currentParam;
    const auto savedDirect = aFrame.useDirectData;
    for (uint32_t i = 0; i < count; ++i)
    {
        params[i]->type->Construct(storage[i]);
        aFrame.data = nullptr;
        aFrame.dataType = nullptr;
        aFrame.useDirectData = false;
        aFrame.currentParam++;
        const auto opcode = static_cast<uint8_t>(*(aFrame.code++));
        RED4ext::OpcodeHandlers::Run(opcode, static_cast<RED4ext::IScriptable*>(aFrame.context), &aFrame, storage[i],
                                     nullptr);
    }
    aFrame.code = savedCode;
    aFrame.data = savedData;
    aFrame.dataType = savedDataType;
    aFrame.currentParam = savedParam;
    aFrame.useDirectData = savedDirect;

    // Only the local V's animation goes out.
    bool local = true;
    if (aHook.ownerParam >= 0)
    {
        auto* owner = HandleInstance(params[aHook.ownerParam]->type, storage[aHook.ownerParam]);
        if (!owner && aHook.context != Hook::Context::None)
            owner = ContextOwner(aHook.context, aContext); // optional owner left out: the caller's own
        local = IsLocalPlayer(owner);
    }
    else if (aHook.context == Hook::Context::None && aContext)
    {
        static auto* gameObject = ClassOf("gameObject");
        auto* context = static_cast<RED4ext::IScriptable*>(aContext);
        if (gameObject && context->GetType()->IsA(gameObject))
            local = IsLocalPlayer(context);
    }

    if (!local)
    {
        ++aHook.notLocal;
    }
    else
    {
        AnimInput input;
        input.kind = aHook.kind;
        bool ok = false;
        bool relevant = true;
        if (aHook.eventSink)
        {
            relevant = ReadEvent(HandleInstance(params[aHook.valueParam]->type, storage[aHook.valueParam]), input, ok);
        }
        else
        {
            input.name = reinterpret_cast<RED4ext::CName*>(storage[aHook.nameParam])->hash;
            ok = true;
            if (aHook.kind != AnimInputKind::Event)
            {
                auto* type = params[aHook.valueParam]->type;
                void* value = storage[aHook.valueParam];
                if (aHook.kind == AnimInputKind::Feature)
                {
                    auto* feature = HandleInstance(type, value);
                    ok = feature != nullptr;
                    if (ok)
                        ReadFeature(feature, input);
                }
                else
                {
                    ok = ReadValue(type, value, input.value);
                }
            }
        }
        if (!relevant)
        {
            ++aHook.ignored;
        }
        else if (ok)
        {
            ++aHook.captured;
            Store(input, aHook.name.c_str());
        }
        else
        {
            ++aHook.unreadable;
        }
    }

    for (uint32_t i = 0; i < count; ++i)
        params[i]->type->Destruct(storage[i]);
}

void AnimCapture::Store(const AnimInput& aInput, const char* aFunction)
{
    std::scoped_lock lock(g_mutex);
    ++g_totalCaptured;
    Push(g_network, aInput);
    Push(g_mirror, aInput);

    if (!g_recording)
        return;
    const auto now = std::chrono::steady_clock::now();
    if (now >= g_recordEnd || g_recordLines >= kMaxRecordLines)
    {
        FinishRecordingLocked();
        return;
    }
    const double seconds = std::chrono::duration<double>(now - g_recordStart).count();
    g_record << seconds << "  " << aFunction << "  " << KindText(aInput.kind) << "  "
             << NameText(RED4ext::CName(aInput.name));
    if (aInput.kind == AnimInputKind::Feature)
    {
        g_record << "  " << NameText(RED4ext::CName(aInput.featureClass)) << " {";
        for (const auto& prop : aInput.props)
            g_record << " " << NameText(RED4ext::CName(prop.name)) << "=" << ValueText(prop.value);
        g_record << " }";
    }
    else if (aInput.kind != AnimInputKind::Event)
    {
        g_record << "  " << ValueText(aInput.value);
    }
    g_record << "\n";
    ++g_recordLines;
}

void AnimCapture::DrainForNetwork(std::vector<AnimInput>& aOut)
{
    std::scoped_lock lock(g_mutex);
    Drain(g_network, aOut);
}

void AnimCapture::DrainForMirror(std::vector<AnimInput>& aOut)
{
    std::scoped_lock lock(g_mutex);
    Drain(g_mirror, aOut);
}

void AnimCapture::SetLocalPlayer(RED4ext::IScriptable* aPlayer)
{
    g_localPlayer = aPlayer;
}

void AnimCapture::SetMirrorActive(bool aActive)
{
    std::scoped_lock lock(g_mutex);
    g_mirror.active = aActive;
    g_mirror.latest.clear();
    g_mirror.events.clear();
}

void AnimCapture::StartRecording(float aSeconds, const std::filesystem::path& aFile)
{
    std::scoped_lock lock(g_mutex);
    FinishRecordingLocked();
    g_recording = true;
    g_recordStart = std::chrono::steady_clock::now();
    g_recordEnd = g_recordStart + std::chrono::milliseconds(static_cast<int64_t>(aSeconds * 1000.0f));
    g_recordFile = aFile;
    g_record.str({});
    g_recordLines = 0;
}

int AnimCapture::DumpFunctions(const std::filesystem::path& aFile) const
{
    auto* rtti = RED4ext::CRTTISystem::Get();
    if (!rtti)
        return 0;
    static const char* patterns[] = {"Anim", "Replicat", "Locomotion"};
    auto matches = [](const char* aName)
    {
        for (const auto* pattern : patterns)
        {
            if (aName && std::strstr(aName, pattern))
                return true;
        }
        return false;
    };
    auto describe = [](std::ostream& aOut, const char* aOwner, RED4ext::CBaseFunction* aFunc, bool aStatic)
    {
        aOut << aOwner << "::" << NameText(aFunc->shortName) << (aFunc->flags.isNative ? "  native" : "  script")
             << (aStatic ? " static" : "") << "  (";
        for (uint32_t i = 0; i < aFunc->params.Size(); ++i)
        {
            auto* param = aFunc->params[i];
            aOut << (i ? ", " : "") << NameText(param->name) << ": " << NameText(param->type->GetName())
                 << (param->flags.isOptional ? " opt" : "");
        }
        aOut << ")";
        if (aFunc->returnType)
            aOut << " -> " << NameText(aFunc->returnType->type->GetName());
        aOut << "\n";
    };

    std::ofstream file(aFile, std::ios::trunc);
    int count = 0;
    RED4ext::DynArray<RED4ext::CClass*> classes;
    rtti->GetClasses(nullptr, classes, nullptr, true);
    for (auto* cls : classes)
    {
        const char* owner = NameText(cls->name);
        for (auto* func : cls->funcs)
        {
            if (matches(NameText(func->shortName)))
            {
                describe(file, owner, func, false);
                ++count;
            }
        }
        for (auto* func : cls->staticFuncs)
        {
            if (matches(NameText(func->shortName)))
            {
                describe(file, owner, func, true);
                ++count;
            }
        }
    }
    RED4ext::DynArray<RED4ext::CBaseFunction*> globals;
    rtti->GetGlobalFunctions(globals);
    for (auto* func : globals)
    {
        if (matches(NameText(func->shortName)))
        {
            describe(file, "(global)", func, true);
            ++count;
        }
    }
    return count;
}

std::string AnimCapture::Status() const
{
    std::ostringstream out;
    if (!g_installed)
        return "animation capture: not started yet (starts with the game session)";
    out << "animation capture: " << (g_enabled ? "on" : "off") << ", " << g_hookCount << " capture point(s)";
    if (!g_tableInfo.empty())
        out << " (" << g_tableInfo << ")";
    {
        std::scoped_lock lock(g_mutex);
        out << ", " << g_totalCaptured << " input(s) captured";
        if (g_recording)
            out << ", RECORDING (" << g_recordLines << " lines)";
    }
    out << "\n";
    for (size_t i = 0; i < g_hookCount; ++i)
    {
        const auto& hook = g_hooks[i];
        out << "  " << hook.name << ": " << hook.calls.load() << " calls, " << hook.captured.load() << " from you, "
            << hook.notLocal.load() << " other, " << hook.unreadable.load() << " unreadable";
        if (hook.eventSink)
            out << ", " << hook.ignored.load() << " not animation";
        out << "\n";
    }
    if (!g_scriptOnly.empty())
    {
        out << "  script functions (not captured):";
        for (const auto& name : g_scriptOnly)
            out << " " << name;
        out << "\n";
    }
    return out.str();
}

// ---------------------------------------------------------------------------------------------------------------------

namespace
{
std::atomic<int> g_applyVia{static_cast<int>(AnimApplyVia::Events)};

// Builds an AnimFeature object of the captured class with the captured property values.
Red::Handle<Red::IScriptable> MakeFeature(const AnimInput& aInput)
{
    static auto* featureBase = ClassOf("animAnimFeature");
    auto* cls = RED4ext::CRTTISystem::Get()->GetClass(RED4ext::CName(aInput.featureClass));
    if (!cls || (featureBase && !cls->IsA(featureBase)))
        return {};
    auto feature = Red::MakeScriptedHandle<Red::IScriptable>(cls);
    if (!feature)
        return {};
    for (const auto& prop : aInput.props)
    {
        auto* property = cls->GetProperty(RED4ext::CName(prop.name));
        if (property)
            WriteValue(property->type, property->GetValuePtr<void>(feature.instance), prop.value);
    }
    return feature;
}

// The way the game's own AnimationControllerComponent helpers do it: an AnimInputSetter* / AnimExternalEvent
// queued on the entity.
int ApplyViaEvents(Red::IScriptable* aEntity, const std::vector<AnimInput>& aInputs, std::string* aError)
{
    int applied = 0;
    for (const auto& input : aInputs)
    {
        const char* className = nullptr;
        switch (input.kind)
        {
        case AnimInputKind::Feature: className = "entAnimInputSetterAnimFeature"; break;
        case AnimInputKind::Float: className = "entAnimInputSetterFloat"; break;
        case AnimInputKind::Int: className = "entAnimInputSetterInt"; break;
        case AnimInputKind::Bool: className = "entAnimInputSetterBool"; break;
        case AnimInputKind::Vector: className = "entAnimInputSetterVector"; break;
        case AnimInputKind::Event: className = "entAnimExternalEvent"; break;
        default: break;
        }
        auto* cls = className ? ClassOf(className) : nullptr;
        if (!cls)
        {
            if (aError)
                *aError = std::string("event class missing: ") + (className ? className : "?");
            continue;
        }
        auto event = Red::MakeScriptedHandle<Red::IScriptable>(cls);
        if (!event)
            continue;
        bool ok = true;
        if (input.kind == AnimInputKind::Event)
        {
            auto* name = cls->GetProperty(RED4ext::CName("name"));
            ok = name != nullptr;
            if (ok)
                *name->GetValuePtr<RED4ext::CName>(event.instance) = RED4ext::CName(input.name);
        }
        else
        {
            auto* key = cls->GetProperty(RED4ext::CName("key"));
            auto* value = cls->GetProperty(RED4ext::CName("value"));
            ok = key && value;
            if (ok)
            {
                *key->GetValuePtr<RED4ext::CName>(event.instance) = RED4ext::CName(input.name);
                if (input.kind == AnimInputKind::Feature)
                {
                    auto feature = MakeFeature(input);
                    ok = feature && value->type->GetType() == RED4ext::rtti::ERTTIType::Handle;
                    if (ok)
                        *value->GetValuePtr<Red::Handle<Red::IScriptable>>(event.instance) = feature;
                }
                else
                {
                    ok = WriteValue(value->type, value->GetValuePtr<void>(event.instance), input.value);
                }
            }
        }
        if (ok && Red::CallVirtual(aEntity, "QueueEvent", event))
            ++applied;
    }
    return applied;
}
} // namespace

void SetAnimApplyVia(AnimApplyVia aVia)
{
    g_applyVia = static_cast<int>(aVia);
}

AnimApplyVia GetAnimApplyVia()
{
    return static_cast<AnimApplyVia>(g_applyVia.load());
}

int ApplyAnimInputs(Red::IScriptable* aEntity, const std::vector<AnimInput>& aInputs, std::string* aError)
{
    if (!aEntity)
        return 0;
    if (GetAnimApplyVia() == AnimApplyVia::Events)
        return ApplyViaEvents(aEntity, aInputs, aError);

    Red::Handle<Red::IScriptable> controller;
    if (!Red::CallVirtual(aEntity, "GetAnimationControllerComponent", controller) || !controller)
    {
        if (aError)
            *aError = "the puppet has no animation controller";
        return 0;
    }

    int applied = 0;
    for (const auto& input : aInputs)
    {
        Red::CName name(input.name);
        bool ok = false;
        switch (input.kind)
        {
        case AnimInputKind::Float:
        {
            float value = input.value.f[0];
            ok = Red::CallVirtual(controller.instance, "SetInputFloat", name, value);
            break;
        }
        case AnimInputKind::Int:
        {
            auto value = static_cast<int32_t>(input.value.i);
            ok = Red::CallVirtual(controller.instance, "SetInputInt", name, value);
            break;
        }
        case AnimInputKind::Bool:
        {
            bool value = input.value.i != 0;
            ok = Red::CallVirtual(controller.instance, "SetInputBool", name, value);
            break;
        }
        case AnimInputKind::Vector:
        {
            Red::Vector4 value;
            value.X = input.value.f[0];
            value.Y = input.value.f[1];
            value.Z = input.value.f[2];
            value.W = input.value.f[3];
            ok = Red::CallVirtual(controller.instance, "SetInputVector", name, value);
            break;
        }
        case AnimInputKind::Event:
        {
            ok = Red::CallVirtual(controller.instance, "PushEvent", name);
            break;
        }
        case AnimInputKind::Feature:
        {
            auto feature = MakeFeature(input);
            if (!feature)
                break;
            ok = Red::CallVirtual(controller.instance, "ApplyFeature", name, feature);
            break;
        }
        default: break;
        }
        if (ok)
            ++applied;
    }
    return applied;
}
} // namespace coop::plugin
