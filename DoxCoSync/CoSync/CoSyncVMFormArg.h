#pragma once

// Round 141: runtime-typed Papyrus form-argument packing.
//
// f4se's VMVariable::Set<T*> packs handles using the STATIC type's kTypeID
// (PapyrusArgs.h: PackValue<T**> -> PackHandle(dst, *src, BaseType::kTypeID)).
// Every CoSync call site passed forms as plain `TESForm*`, whose kTypeID is 0
// ("special-case") — so AddItem/EquipItem/UnequipItem args were packed under
// VM type 0 instead of the form's real class. Live symptom across all
// 2026-07-05 sessions: every no-arg Papyrus call on proxies demonstrably
// works (Disable/Enable rebuilt bodies), while every form-arg call
// (AddItem/EquipItem for weapons AND outfits) executes with zero visible
// effect. Packing with the form's RUNTIME type is correct in either case —
// if type-0 packing actually worked this is a no-op, if it didn't this is
// the fix.
//
// Usage:  VMVariable v;  CoSyncVM::SetFormArg(v, form);  args.Push(&v);

#include "GameForms.h"
#include "PapyrusArgs.h"

namespace CoSyncVM
{
    namespace detail
    {
        template <UInt32 FT>
        class TypedForm : public TESForm
        {
        public:
            enum { kTypeID = FT };
        };

        template <UInt32 FT>
        inline void SetTyped(VMVariable& var, TESForm* form)
        {
            auto* typed = static_cast<TypedForm<FT>*>(form);
            var.Set(&typed);
        }
    }

    // Packs `form` into `var` under the form's RUNTIME VM class. Falls back
    // to the legacy base-Form packing for types not listed (add as needed).
    inline void SetFormArg(VMVariable& var, TESForm* form)
    {
        if (!form)
        {
            TESForm* nullForm = nullptr;
            var.Set(&nullForm);
            return;
        }

        switch (form->formType)
        {
        case kFormType_WEAP: detail::SetTyped<kFormType_WEAP>(var, form); return;
        case kFormType_ARMO: detail::SetTyped<kFormType_ARMO>(var, form); return;
        case kFormType_ALCH: detail::SetTyped<kFormType_ALCH>(var, form); return;
        case kFormType_AMMO: detail::SetTyped<kFormType_AMMO>(var, form); return;
        case kFormType_MISC: detail::SetTyped<kFormType_MISC>(var, form); return;
        case kFormType_BOOK: detail::SetTyped<kFormType_BOOK>(var, form); return;
        case kFormType_KEYM: detail::SetTyped<kFormType_KEYM>(var, form); return;
        case kFormType_NOTE: detail::SetTyped<kFormType_NOTE>(var, form); return;
        case kFormType_FACT: detail::SetTyped<kFormType_FACT>(var, form); return;
        case kFormType_AVIF: detail::SetTyped<kFormType_AVIF>(var, form); return;
        case kFormType_IDLE: detail::SetTyped<kFormType_IDLE>(var, form); return;
        // ObjectReference (placed static ref, map markers) and Actor — needed
        // when passing refs as ObjectReference parameters to Papyrus natives
        // like ObjectReference.MoveTo(ObjectReference akTarget, ...).
        case kFormType_REFR: detail::SetTyped<kFormType_REFR>(var, form); return;
        case kFormType_ACHR: detail::SetTyped<kFormType_ACHR>(var, form); return;
        default:
        {
            TESForm* f = form;
            var.Set(&f);
            return;
        }
        }
    }
}
