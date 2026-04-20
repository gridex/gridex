//
// MCPToolRegistry.cpp
//

#include "MCPToolRegistry.h"

namespace DBModels
{
    MCPToolRegistry::MCPToolRegistry() = default;

    void MCPToolRegistry::registerTool(std::shared_ptr<MCPTool> tool)
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (tool) tools_[tool->name()] = std::move(tool);
    }

    void MCPToolRegistry::unregisterTool(const std::string& name)
    {
        std::lock_guard<std::mutex> lk(mtx_);
        tools_.erase(name);
    }

    std::shared_ptr<MCPTool> MCPToolRegistry::get(const std::string& name) const
    {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = tools_.find(name);
        return it != tools_.end() ? it->second : nullptr;
    }

    std::vector<MCPToolDefinition> MCPToolRegistry::definitions() const
    {
        std::lock_guard<std::mutex> lk(mtx_);
        std::vector<MCPToolDefinition> out;
        out.reserve(tools_.size());
        for (const auto& [_, t] : tools_) out.push_back(t->definition());
        return out;
    }
}
