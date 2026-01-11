#include "PapyrusVM.h"
#include "GameObjects.h"

RelocPtr <GameVM *> g_gameVM(0x030E0208);
// Fallout 4 v1.11.191
RelocAddr<_PlaceAtMe_Native>
PlaceAtMe_Native(0x00A1FCA0);


bool VirtualMachine::HasStack(UInt32 stackId)
{
	BSReadLocker locker(&stackLock);
	return m_allStacks.Find(&stackId) != NULL;
}
