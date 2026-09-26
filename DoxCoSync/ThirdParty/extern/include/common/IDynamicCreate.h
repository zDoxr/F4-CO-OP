#pragma once

#include <map>
#include "common/IDataStream.h"
#include "common/IErrors.h"


#define ENABLE_IDYNAMICCREATE 0

#if ENABLE_IDYNAMICCREATE

#define GetDynType(name)	(&(##name##::__DYN_DynamicType))


#define DYNAMIC_DECLARE(name)															\
public:																					\
	class __DYN_##name##_DynamicType : public IDynamicType								\
	{																					\
		public:																			\
			__DYN_##name##_DynamicType() { }											\
			~__DYN_##name##_DynamicType() { }											\
																						\
			virtual IDynamic *	Create(void)	{ return new name; }					\
			virtual char *		GetName(void)	{ return #name; }						\
			virtual IDynamic *	Instantiate(IDataStream * stream);						\
	};																					\
																						\
	static __DYN_##name##_DynamicType	__DYN_DynamicType;								\
	virtual IDynamicType * __DYN_GetDynamicType(void) { return &__DYN_DynamicType; }	\
																						\
	friend __DYN_##name##_DynamicType;


#define DYNAMIC_DEFINE(name) name##::__DYN_##name##_DynamicType name##::__DYN_DynamicType;


#define DYNAMIC_INSTANTIATE_HANDLER(name)		IDynamic * name##::__DYN_##name##_DynamicType::Instantiate(IDataStream * stream) { name * object = new name;
#define END_DYNAMIC_INSTANTIATE_HANDLER			return object; }


#define NO_DYNAMIC_INSTANTIATE_HANDLER(name)	DYNAMIC_INSTANTIATE_HANDLER(name) { HALT("attempted to instantiate " #name); } END_DYNAMIC_INSTANTIATE_HANDLER


#define CAST(ptr, type)	_DynamicCast <type>(ptr);

class IDynamicType;


class IDynamic
{
	public:
				IDynamic()	{ }
		virtual ~IDynamic()	{ }

		virtual IDynamicType *	__DYN_GetDynamicType(void) = 0;
};


class IDynamicType
{
	public:
				IDynamicType()	{ }
		virtual ~IDynamicType() { }

		virtual IDynamic *	Create(void) = 0;
		virtual char *		GetName(void) = 0;

		virtual IDynamic *	Instantiate(IDataStream * stream) = 0;
};


template <typename T>
T * _DynamicCast(IDynamic * ptr)
{
	if(ptr && (&T::__DYN_DynamicType == ptr->__DYN_GetDynamicType()))
		return static_cast<T *>(ptr);

	return NULL;
}


class IClassRegistry
{
	public:
				IClassRegistry();
				~IClassRegistry();

		static void	RegisterClassInfo(UInt32 id, IDynamicType * typeInfo);

		static IDynamicType *	LookupClassInfo(UInt32 id);
		static IDynamicType *	LookupClassInfo(char * name);

		static IDynamic *		Create(UInt32 id)	{ IDynamicType * info = LookupClassInfo(id); return info ? info->Create() : NULL; }
		static IDynamic *		Create(char * name)	{ IDynamicType * info = LookupClassInfo(name); return info ? info->Create() : NULL; }

		static IDynamic *		Instantiate(UInt32 id, IDataStream * stream)	{ IDynamicType * info = LookupClassInfo(id); return info ? info->Instantiate(stream) : NULL; }
		static IDynamic *		Instantiate(char * name, IDataStream * stream)	{ IDynamicType * info = LookupClassInfo(name); return info ? info->Instantiate(stream) : NULL; }

		static char *			GetName(UInt32 id)	{ IDynamicType * info = LookupClassInfo(id); return info ? info->GetName() : NULL; }

	private:
		typedef std::map <UInt32, IDynamicType *>	ClassRegistryType;
		static ClassRegistryType	theClassRegistry;
};

#endif
