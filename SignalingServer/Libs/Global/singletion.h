#ifndef SINGLETON_H
#define SINGLETON_H

#include <mutex>
#include <memory>
#include <iostream>
#include <type_traits>

template<typename T>
class Singleton
{
protected:
    Singleton() = default;                                                          // 保护构造，禁止外部构造
    Singleton(const Singleton<T>&) = delete;                                        // 禁止拷贝构造
    Singleton<T>& operator=(const Singleton<T>&) = delete;                          // 禁止赋值拷贝

    static std::shared_ptr<T> m_pInstance;

public:
    static std::shared_ptr<T> Instance()
    {
        static std::once_flag flag;
        std::call_once(flag, []()
            {
                try
                {
                    // 纯 C++ 类的默认处理路径
                    m_pInstance = std::shared_ptr<T>(new T);
                }
                catch (...)
                {
                    std::cerr << "Failed to create singleton instance." << std::endl;
                }
            });
        return m_pInstance;
    }

    void PrintAddress()
    {
        std::cout << "Singleton<T> address: " << m_pInstance.get() << std::endl;
    }

    virtual ~Singleton<T>()
    {
        std::cout << "Singleton<T> is destroyed" << std::endl;
    }
};

template<typename T>
std::shared_ptr<T> Singleton<T>::m_pInstance = nullptr;

#endif // SINGLETON_H