#pragma once

#include <memory>
#include <string>
#include <gtest/gtest.h>
#include <Astra/Component/ComponentRegistry.hpp>
#include <Astra/Core/TypeContext.hpp>
#include <Astra/Core/TypeID.hpp>

namespace Astra::TestSupport
{
    // ComponentRegistry::InstallOwned refuses a foreign TypeContext; install the
    // module default for the scope and restore whatever slot was there before
    // (ComponentModule tests assert the slot starts null).
    class ScopedDefaultTypeContext
    {
    public:
        ScopedDefaultTypeContext() : m_previous(Astra::Detail::CurrentTypeContextSlot())
        {
            Astra::SetTypeContext(&Astra::DefaultTypeContext());
        }
        ~ScopedDefaultTypeContext() { Astra::SetTypeContext(m_previous); }
        ScopedDefaultTypeContext(const ScopedDefaultTypeContext&) = delete;
        ScopedDefaultTypeContext& operator=(const ScopedDefaultTypeContext&) = delete;

    private:
        Astra::TypeContext* m_previous;
    };

    // Installs T's registry-built descriptor with `storage` overridden, so ONE
    // C++ type can be Table in one ComponentRegistry and Sparse in another: the
    // only way a single test binary reproduces "the storage class changed between
    // save and load" (spec s8.3). The trait is unchanged, so tests drive an
    // overridden type only through by-ID / by-hash APIs, never the typed API.
    // Call it BEFORE constructing a Registry over `creg` or loading into it.
    template<typename T>
    inline Astra::ComponentID InstallWithStorage(Astra::ComponentRegistry& creg, Astra::Storage storage)
    {
        static int s_image = 0;   // stand-in module token, one per T
        const Astra::ComponentID id = Astra::TypeID<T>::Value();
        Astra::ComponentDescriptor desc = Astra::ComponentRegistry::MakeDescriptor<T>(id, nullptr);
        desc.storage = storage;
        const std::string name(Astra::TypeID<T>::Name());
        desc.name = name.c_str();   // InstallOwned copies it before returning
        const uint32_t owner = creg.OpenModuleId("StorageOverride");
        EXPECT_NE(creg.InstallOwned(id, owner, desc, &s_image), Astra::InstallResult::Refused);
        return id;
    }
}
