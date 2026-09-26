#include "f4se/GameWorkshop.h"
#include "f4se/GameAPI.h"

RelocAddr <PowerUtils::_UpdateMovingWirelessItem> PowerUtils::UpdateMovingWirelessItem(0x0038E990); // Usually paired with LinkPower
// this was inlined everywhere
//RelocAddr <_GetObjectAtConnectPoint> GetObjectAtConnectPoint(0x001FF360); // Acquires objects that are touching attach points
RelocAddr <TerminalUtils::_EstablishTerminalLinks> TerminalUtils::EstablishTerminalLinks(0x0038F820);
RelocAddr <SplineUtils::_ConnectSpline> SplineUtils::ConnectSpline(0x0038DCC0);
RelocAddr <SplineUtils::_UpdateSpline> SplineUtils::UpdateSpline(0x0038D830);

// 
RelocAddr <Workshop::_ScrapReference> Workshop::ScrapReference(0x00393C10);

RelocPtr<BSPointerHandle<TESObjectREFR>> Workshop::hCurrentWorkshop(0x030EC618);

// UPDATE 2026-06-17: re-verified via IDA on 1.11.221 -- 0x00385400 skips the
// function's real prologue (mov r11,rsp; push rsi; push rdi; sub rsp,0x68;
// mov rsi,[rcx+0xB8]), landing 0x10 bytes into the body at a clean
// instruction boundary ("xor edi,edi") that nonetheless depends on all of
// that setup already having happened -- entering here would read garbage
// where rsi should hold the loaded member, and corrupt the stack on return
// (push/pop count mismatch). Real function start confirmed at 0x3853F0.
RelocAddr<Workshop::_FindNearestValidWorkshop> Workshop::FindNearestValidWorkshop(0x003853F0);

namespace BSConnectPoint
{
	class Parent :
		public BSIntrusiveRefCounted
	{
	public:
		// members
		BSAutoFixedString parentName;	// 08
		BSAutoFixedString name;			// 10
		NiQuaternion rotation;			// 18
		NiPoint3 position;				// 28
		float scale;					// 34

		void Release()
		{
			if(!InterlockedDecrement(&refCount))
			{
				this->~Parent();
				Heap_Free(this);
			}
		}
	};
	STATIC_ASSERT(sizeof(Parent) == 0x38);
}

enum SnappedReferencePointStatusEnum : std::int32_t
{
	kNoReference = 0,
	kNoSnapPoint,
	kSnapPointFound,
	kNonReferenceHit,
	kCount
};

struct SnappedReferencePointStatus
{
public:
	~SnappedReferencePointStatus()
	{
		// in clib this would be easy, kludge it up here
		if(foundSnapPoint)
			foundSnapPoint->Release();
	}

	// members
	SnappedReferencePointStatusEnum status{ SnappedReferencePointStatusEnum::kCount };	// 00
	BSConnectPoint::Parent * foundSnapPoint{ nullptr };									// 08 - BSTSmartPointer
};

STATIC_ASSERT(sizeof(SnappedReferencePointStatus) == 0x10);

TESObjectREFR * GetSnappedReferenceImpl(const TESObjectREFR & a_refr, const NiPoint3 & a_connectPointWS, const bhkWorld & a_physicsWorld, SnappedReferencePointStatus & a_status, float a_radius)
{
	using func_t = decltype(&GetSnappedReferenceImpl);
	RelocAddr <func_t> func(0x003BE0E0);
	return func(a_refr, a_connectPointWS, a_physicsWorld, a_status, a_radius);
}

TESObjectREFR * GetObjectAtConnectPoint(const TESObjectREFR & a_refr, NiPoint3 & a_connectPointWS, const bhkWorld & a_physicsWorld, float a_radius)
{
	SnappedReferencePointStatus status;
	return GetSnappedReferenceImpl(a_refr, a_connectPointWS, a_physicsWorld, status, a_radius);
}
