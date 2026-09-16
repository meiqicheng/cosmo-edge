// Owns the memory pool and publishes its context for mem::GetMemoryPool().

#pragma once

#include <memory>

namespace cosmo::mem {
class MemoryPoolMng;
}

namespace cosmo::service {

class MemoryPoolServiceImpl final {
public:
    MemoryPoolServiceImpl();
    ~MemoryPoolServiceImpl();

    MemoryPoolServiceImpl(const MemoryPoolServiceImpl&)            = delete;
    MemoryPoolServiceImpl& operator=(const MemoryPoolServiceImpl&) = delete;

private:
    std::unique_ptr<cosmo::mem::MemoryPoolMng> pool_;
};

}  // namespace cosmo::service
