#pragma once

#include <map>

template <typename t_key, typename t_data>
class IRangeMap
{
public:
	struct Entry
	{
		bool	Contains(t_key addr, t_key base)
		{
			return (addr >= base) && (addr <= (base + length - 1));
		}

		t_key	length;
		t_data	data;
	};

	typedef std::map <t_key, Entry>			EntryMapType;
	typedef typename EntryMapType::iterator	Iterator;

	IRangeMap()
	{

	}

	virtual ~IRangeMap()
	{

	}

	void	Clear(void)
	{
		m_entries.clear();
	}

	t_data *	Add(t_key start, t_key length)
	{
		t_data	* result = NULL;
		Entry	* entry = NULL;

		t_key	end = start + length - 1;

		if(end >= start)
		{

			if(m_entries.empty())
			{
				entry = &m_entries[start];
			}
			else
			{


				EntryMapType::iterator	iter = m_entries.lower_bound(start);


				if(iter == m_entries.begin())
				{

					if(iter->first > end)
					{

						entry = &m_entries[start];
					}
				}
				else
				{


					if((iter == m_entries.end()) || (iter->first > end))
					{

						EntryMapType::iterator	preIter = iter;
						preIter--;

						t_key	preEnd = preIter->first + preIter->second.length - 1;

						if(preEnd < start)
						{

							EntryMapType::iterator	newEntry = m_entries.insert(preIter, EntryMapType::value_type(start, Entry()));
							entry = &newEntry->second;
						}
					}
				}
			}
		}


		if(entry)
		{
			entry->length = length;

			result = &entry->data;
		}

		return result;
	}

	t_data *	Lookup(t_key addr, t_key * base = NULL, t_key * length = NULL)
	{
		t_data	* result = NULL;

		EntryMapType::iterator	iter = LookupIter(addr);
		if(iter != m_entries.end())
		{
			if(base) *base = iter->first;
			if(length) *length = iter->second.length;

			result = &iter->second.data;
		}

		return result;
	}

	bool	Erase(t_key addr, t_key * base = NULL, t_key * length = NULL)
	{
		bool result = false;

		EntryMapType::iterator	iter = LookupIter(addr);
		if(iter != m_entries.end())
		{
			if(base) *base = iter->first;
			if(length) *length = iter->second.length;

			m_entries.erase(iter);

			result = true;
		}

		return result;
	}

	t_key	GetDataRangeLength(t_data * data)
	{
		Entry	* entry = reinterpret_cast <Entry *>(reinterpret_cast <UInt8 *>(data) - offsetof(Entry, data));

		return entry->length;
	}

	typename EntryMapType::iterator	LookupIter(t_key addr)
	{
		EntryMapType::iterator	result = m_entries.end();

		if(!m_entries.empty())
		{

			EntryMapType::iterator	iter = m_entries.lower_bound(addr);


			if(iter == m_entries.end())
			{

				--iter;

				if(iter->second.Contains(addr, iter->first))
				{
					result = iter;
				}
			}

			else if(iter->first > addr)
			{

				if(iter != m_entries.begin())
				{
					--iter;

					if(iter->second.Contains(addr, iter->first))
					{
						result = iter;
					}
				}
			}
			else
			{

				result = iter;
			}
		}

		return result;
	}

	typename EntryMapType::iterator	Begin(void)
	{
		return m_entries.begin();
	}

	typename EntryMapType::iterator	End(void)
	{
		return m_entries.end();
	}

private:
	EntryMapType	m_entries;
};
