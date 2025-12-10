#pragma once

#include "Utilities.h"
#include <string>

class BSString
{
public:
    BSString();
    BSString(const char* str);
    BSString(const BSString& rhs);
    ~BSString();

    BSString& operator=(const BSString& rhs);
    BSString& operator=(const char* str);

    bool Set(const char* str);
    const char* c_str() const { return m_data; }
    bool empty() const { return (!m_data || !m_data[0]); }

private:
    char* m_data;   
};

// Basic wrapper used by F4SE for string interning
namespace StringCache
{
    class Ref
    {
    public:
        Ref();
        Ref(const Ref& rhs);
        Ref(const char* str);
        ~Ref();

        Ref& operator=(const Ref& rhs);
        Ref& operator=(const char* str);

        const char* c_str() const { return m_data ? m_data : ""; }

    private:
        char* m_data;
    };
}
