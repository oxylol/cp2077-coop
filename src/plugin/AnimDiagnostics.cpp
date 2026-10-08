#include "plugin/AnimDiagnostics.hpp"

#include <fstream>
#include <iomanip>
#include <sstream>

#include <RED4ext/ResourceReference.hpp>
#include <RED4ext/Scripting/Natives/Generated/anim/AnimSetup.hpp>
#include <RED4ext/Scripting/Natives/Generated/anim/AnimSetupEntry.hpp>
#include <RED4ext/Scripting/Natives/Generated/ent/AnimGraphResourceContainerEntry.hpp>
#include <RED4ext/Scripting/Natives/entEntity.hpp>
#include <RED4ext/Scripting/Natives/entIComponent.hpp>

namespace coop::plugin
{
namespace
{
using RED4ext::CName;
using RED4ext::rtti::ERTTIType;

const char* Text(CName aName)
{
    const char* text = aName.ToString();
    return text ? text : "?";
}

RED4ext::CClass* ClassOf(const char* aName)
{
    return RED4ext::CRTTISystem::Get()->GetClass(CName(aName));
}

// The object a handle (or weak handle) value points to.
RED4ext::ISerializable* Pointee(RED4ext::rtti::IType* aType, void* aValue)
{
    if (!aType || !aValue)
        return nullptr;
    if (aType->GetType() == ERTTIType::Handle)
        return reinterpret_cast<RED4ext::Handle<RED4ext::ISerializable>*>(aValue)->instance;
    if (aType->GetType() == ERTTIType::WeakHandle)
    {
        auto* weak = reinterpret_cast<RED4ext::WeakHandle<RED4ext::ISerializable>*>(aValue);
        return weak->Expired() ? nullptr : weak->instance;
    }
    return nullptr;
}

// A property of an object, by name (searching parents too), or nullptr.
RED4ext::CProperty* Prop(RED4ext::ISerializable* aObject, const char* aName)
{
    return aObject ? aObject->GetType()->GetProperty(CName(aName)) : nullptr;
}

// A property value as text, for the simple types a graph variable has.
std::string ValueText(RED4ext::CProperty* aProp, void* aObject)
{
    if (!aProp)
        return "-";
    void* ptr = aProp->GetValuePtr<void>(aObject);
    const auto name = aProp->type->GetName();
    std::ostringstream out;
    if (name == CName("Float"))
        out << *static_cast<float*>(ptr);
    else if (name == CName("Int32"))
        out << *static_cast<int32_t*>(ptr);
    else if (name == CName("Bool"))
        out << (*static_cast<bool*>(ptr) ? "true" : "false");
    else if (name == CName("CName"))
        out << Text(*static_cast<CName*>(ptr));
    else if (name == CName("Vector4"))
    {
        const float* v = static_cast<float*>(ptr);
        out << "(" << v[0] << ", " << v[1] << ", " << v[2] << ", " << v[3] << ")";
    }
    else
        out << "<" << Text(name) << ">";
    return out.str();
}

// Calls aVisit(element pointer, element type) for each element of an array property.
template<typename F>
void ForEachElement(RED4ext::CProperty* aProp, void* aObject, F&& aVisit)
{
    if (!aProp || aProp->type->GetType() != ERTTIType::Array)
        return;
    auto* arrayType = static_cast<RED4ext::CRTTIBaseArrayType*>(aProp->type);
    void* array = aProp->GetValuePtr<void>(aObject);
    const uint32_t length = arrayType->GetLength(array);
    for (uint32_t i = 0; i < length && i < 2000; ++i)
        aVisit(arrayType->GetElement(array, i), arrayType->GetInnerType());
}

void DumpGraph(std::ostream& aOut, RED4ext::ISerializable* aGraph)
{
    aOut << "    graph class " << Text(aGraph->GetType()->name) << "\n";

    // Variables: graph.variables -> container with one array per type.
    auto* variablesProp = Prop(aGraph, "variables");
    auto* container = variablesProp ? Pointee(variablesProp->type, variablesProp->GetValuePtr<void>(aGraph)) : nullptr;
    if (!container)
    {
        aOut << "    (no variables container)\n";
    }
    else
    {
        static const char* lists[] = {"floatVariables", "boolVariables", "intVariables", "vectorVariables",
                                      "quaternionVariables", "transformVariables"};
        for (const char* list : lists)
        {
            auto* listProp = Prop(container, list);
            if (!listProp)
                continue;
            int count = 0;
            std::ostringstream items;
            ForEachElement(listProp, container,
                           [&](void* aElement, RED4ext::rtti::IType* aType)
                           {
                               auto* variable = Pointee(aType, aElement);
                               if (!variable)
                                   return;
                               ++count;
                               items << "      " << ValueText(Prop(variable, "name"), variable);
                               if (auto* def = Prop(variable, "default"))
                                   items << "  default " << ValueText(def, variable);
                               if (auto* min = Prop(variable, "min"))
                                   items << "  min " << ValueText(min, variable);
                               if (auto* max = Prop(variable, "max"))
                                   items << "  max " << ValueText(max, variable);
                               items << "\n";
                           });
            aOut << "    " << list << ": " << count << "\n" << items.str();
        }
    }

    // AnimFeature slots: name and the AnimFeature class each expects.
    auto* featuresProp = Prop(aGraph, "animFeatures");
    int features = 0;
    std::ostringstream items;
    ForEachElement(featuresProp, aGraph,
                   [&](void* aElement, RED4ext::rtti::IType* aType)
                   {
                       if (aType->GetType() != ERTTIType::Class)
                           return;
                       auto* cls = static_cast<RED4ext::CClass*>(aType);
                       auto* name = cls->GetProperty(CName("name"));
                       auto* className = cls->GetProperty(CName("className"));
                       ++features;
                       items << "      ";
                       if (name && name->type->GetName() == CName("CName"))
                           items << Text(*name->GetValuePtr<CName>(aElement));
                       if (className && className->type->GetName() == CName("CName"))
                           items << "  " << Text(*className->GetValuePtr<CName>(aElement));
                       items << "\n";
                   });
    aOut << "    animFeatures: " << features << "\n" << items.str();
}
std::string Hex(uint64_t aValue)
{
    std::ostringstream out;
    out << std::hex << std::setw(16) << std::setfill('0') << aValue;
    return out.str();
}

// The animation sets an animated component or animation setup extension brings (round M: which ones the cutscene
// lookalike lacks). Path hashes only: the same hash on two bodies is the same set.
void DumpAnimSetup(std::ostream& aOut, RED4ext::IScriptable* aComponent)
{
    auto* prop = aComponent->GetType()->GetProperty(CName("animations"));
    if (!prop || prop->type->GetName() != CName("animAnimSetup"))
        return;
    const auto* setup = prop->GetValuePtr<RED4ext::anim::AnimSetup>(aComponent);
    auto list = [&](const char* aLabel, const RED4ext::DynArray<RED4ext::anim::AnimSetupEntry>& aEntries)
    {
        aOut << "    " << aLabel << " animsets: " << aEntries.Size() << "\n";
        for (const auto& entry : aEntries)
            aOut << "      " << Hex(entry.animSet.path.hash) << "  priority " << static_cast<int>(entry.priority)
                 << ", " << entry.variableNames.Size() << " variable name(s)\n";
    };
    list("gameplay", setup->gameplay);
    list("cinematic", setup->cinematics);
}
} // namespace

std::string DumpAnimGraphs(Red::IScriptable* aEntity, const std::string& aLabel, const std::filesystem::path& aFile)
{
    static auto* entityClass = ClassOf("entEntity");
    static auto* animatedClass = ClassOf("entAnimatedComponent");
    if (!aEntity || !entityClass || !animatedClass || !aEntity->GetType()->IsA(entityClass))
        return "not an entity";

    std::ofstream out(aFile, std::ios::trunc);
    out << "# animation graphs of " << aLabel << " (" << Text(aEntity->GetType()->name) << ")\n";
    auto* entity = reinterpret_cast<RED4ext::ent::Entity*>(aEntity);
    int components = 0;
    int graphs = 0;
    static auto* extensionClass = ClassOf("entAnimationSetupExtensionComponent");
    static auto* containerClass = ClassOf("entAnimGraphResourceContainer");
    for (auto& component : entity->components)
    {
        if (!component)
            continue;
        if (extensionClass && component->GetType()->IsA(extensionClass))
        {
            out << "== " << Text(component->GetType()->name) << " " << Text(component->name) << "\n";
            DumpAnimSetup(out, component.instance);
            continue;
        }
        if (containerClass && component->GetType()->IsA(containerClass))
        {
            out << "== " << Text(component->GetType()->name) << " " << Text(component->name) << "\n";
            auto* table = component->GetType()->GetProperty(CName("animGraphLookupTable"));
            if (table && table->type->GetType() == ERTTIType::Array)
            {
                const auto* entries = table->GetValuePtr<RED4ext::DynArray<RED4ext::ent::AnimGraphResourceContainerEntry>>(
                    component.instance);
                for (const auto& entry : *entries)
                    out << "    " << Text(entry.graphName) << "  " << Hex(entry.animGraphResource.path.hash) << "\n";
            }
            continue;
        }
        if (!component->GetType()->IsA(animatedClass))
            continue;
        ++components;
        out << "== " << Text(component->GetType()->name) << " " << Text(component->name) << "\n";
        if (auto* rigProp = component->GetType()->GetProperty(CName("rig"));
            rigProp && rigProp->type->GetType() == ERTTIType::ResourceReference)
            out << "    rig path hash " << Hex(rigProp->GetValuePtr<RED4ext::ResourceReference<>>(component.instance)->path.hash)
                << "\n";
        DumpAnimSetup(out, component.instance);
        auto* graphProp = component->GetType()->GetProperty(CName("graph"));
        if (!graphProp || graphProp->type->GetType() != ERTTIType::ResourceReference)
        {
            out << "    (no graph reference)\n";
            continue;
        }
        auto* ref = graphProp->GetValuePtr<RED4ext::ResourceReference<>>(component.instance);
        out << "    graph path hash " << std::hex << std::setw(16) << std::setfill('0') << ref->path.hash << std::dec
            << std::setfill(' ') << (ref->IsLoaded() ? ", loaded" : ", NOT loaded") << "\n";
        if (!ref->IsLoaded())
            continue;
        auto* graph = ref->token->resource.instance;
        if (!graph)
            continue;
        ++graphs;
        DumpGraph(out, graph);
    }
    std::ostringstream summary;
    summary << aLabel << ": " << components << " animated component(s), " << graphs << " loaded graph(s) -> "
            << aFile.string();
    return summary.str();
}
} // namespace coop::plugin
