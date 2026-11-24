#pragma once
#include "TrafficScope.h"
#include <memory>
#include <set>

class ScopeCapture
{
public:
    ScopeCapture();
    ~ScopeCapture();
    traffic_scope::Counts sample(const std::set<unsigned long>& indexes, bool allInterfaces);
    bool active() const;
private:
    struct Impl;
    std::unique_ptr<Impl> state;
};

