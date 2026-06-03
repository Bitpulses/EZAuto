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

    // Determine target IME mode for a given process and control
    ImeMode getTargetMode(const std::string& processName, bool isPassword) const;

    // Get debug info
    const std::unordered_map<std::string, ImeMode>& getRules() const { return rules_; }

private:
    ImeMode defaultMode_;
    SwitchMethod switchMethod_;
    std::unordered_map<std::string, ImeMode> rules_;  // lowercase process name -> mode

    // Minimal JSON parsing helpers
    void skipWhitespace(const std::string& s, size_t& pos);
    std::string parseString(const std::string& s, size_t& pos);
    bool parseObject(const std::string& s, size_t& pos);
    static ImeMode stringToMode(const std::string& modeStr);
    static SwitchMethod stringToSwitchMethod(const std::string& methodStr);
};
