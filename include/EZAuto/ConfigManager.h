#pragma once

#include "Types.h"

#include <string>
#include <unordered_map>

class ConfigManager {
public:
    ConfigManager();

    bool load(const std::string& configPath);
    void setDefaults();

    ImeMode getDefaultMode() const { return defaultMode_; }
    SwitchMethod getSwitchMethod() const { return switchMethod_; }
    ImeMode getTargetMode(const std::string& processName, bool isPassword) const;
    const std::unordered_map<std::string, ImeMode>& getRules() const { return rules_; }

private:
    ImeMode defaultMode_;
    SwitchMethod switchMethod_;
    std::unordered_map<std::string, ImeMode> rules_;

    void skipWhitespace(const std::string& s, size_t& pos);
    std::string parseString(const std::string& s, size_t& pos);
    bool parseObject(const std::string& s, size_t& pos,
                     ImeMode& outDefaultMode, SwitchMethod& outSwitchMethod,
                     std::unordered_map<std::string, ImeMode>& outRules);
    static ImeMode stringToMode(const std::string& modeStr);
    static SwitchMethod stringToSwitchMethod(const std::string& methodStr);
};
