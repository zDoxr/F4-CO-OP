#include "F4MP_Main.h"

namespace f4mp
{
    
    F4MP_Main& GetMainInstance()
    {
        return F4MP_Main::Get();
    }
}
