#include "BSString.h"
#include <cstring>
#include <cstdlib>

// =========================
// BSString implementation
// =========================

BSString::BSString() : m_data(nullptr)
{
}

BSString::BSString(const char* str)
{
    Set(str);
}

BSString::BSString(const BSString& rhs)
{
    Set(rhs.m_data);
}

BSString::~BSString()
{
    if (m_data)
        free(m_data);
}

BSString& BSString::operator=(const BSString& rhs)
{
    if (this != &rhs)
        Set(rhs.m_data);
    return *this;
}

BSString& BSString::operator=(const char* str)
{
    Set(str);
    return *this;
}

bool BSString::Set(const char* str)
{
    if (m_data)
    {
        free(m_data);
        m_data = nullptr;
    }

    if (!str)
        return true;

    size_t len = strlen(str);
    m_data = (char*)malloc(len + 1);
    memcpy(m_data, str, len + 1);

    return true;
}

// =========================
// StringCache::Ref impl
// =========================

namespace StringCache
{
    Ref::Ref() : m_data(nullptr)
    {
    }

    Ref::Ref(const Ref& rhs)
    {
        if (rhs.m_data)
        {
            size_t len = strlen(rhs.m_data);
            m_data = (char*)malloc(len + 1);
            memcpy(m_data, rhs.m_data, len + 1);
        }
        else {
            m_data = nullptr;
        }
    }

    Ref::Ref(const char* str)
    {
        if (str)
        {
            size_t len = strlen(str);
            m_data = (char*)malloc(len + 1);
            memcpy(m_data, str, len + 1);
        }
        else {
            m_data = nullptr;
        }
    }

    Ref::~Ref()
    {
        if (m_data)
            free(m_data);
    }

    Ref& Ref::operator=(const Ref& rhs)
    {
        if (this != &rhs)
        {
            if (m_data)
                free(m_data);

            if (rhs.m_data)
            {
                size_t len = strlen(rhs.m_data);
                m_data = (char*)malloc(len + 1);
                memcpy(m_data, rhs.m_data, len + 1);
            }
            else {
                m_data = nullptr;
            }
        }
        return *this;
    }

    Ref& Ref::operator=(const char* str)
    {
        if (m_data)
            free(m_data);

        if (str)
        {
            size_t len = strlen(str);
            m_data = (char*)malloc(len + 1);
            memcpy(m_data, str, len + 1);
        }
        else {
            m_data = nullptr;
        }

        return *this;
    }
}
