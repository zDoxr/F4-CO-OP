#include "F4MP_Main.h"

namespace f4mp
{
    // ---------------------------------------------------------
    // These functions simply forward to the static singleton.
    // ---------------------------------------------------------

    // (Optional) explicit initialization from somewhere else
    // call:  f4mp::F4MP_Main::Init();

   // implementation
    // already inside the header as inline/static functions.

    // But we CAN explicitly instantiate the singleton to
    // ensure proper linking in some compilers:
    F4MP_Main& GetMainInstance()
    {
        return F4MP_Main::Get();
    }

} // namespace f4mp
