#include <EZAuto/ConfigManager.h>

#include <fstream>
#include <sstream>
#include <algorithm>
#include <iostream>
#include <cctype>

ConfigManager::ConfigManager()
    : defaultMode_(ImeMode::Chinese)
    , switchMethod_(SwitchMethod::Shift) {
}

void ConfigManager::setDefaults() {
    defaultMode_ = ImeMode::Chinese;
    switchMethod_ = SwitchMethod::Shift;
    rules_ = {
        // Terminals
        {"cmd.exe", ImeMode::English},
        {"powershell.exe", ImeMode::English},
        {"windowsterminal.exe", ImeMode::English},
        {"conemu.exe", ImeMode::English},
        {"conemuc64.exe", ImeMode::English},
        {"mintty.exe", ImeMode::English},
        {"putty.exe", ImeMode::English},
        {"bash.exe", ImeMode::English},
        {"wsl.exe", ImeMode::English},

        // IDEs & Editors
        {"code.exe", ImeMode::English},
        {"devenv.exe", ImeMode::English},
        {"idea64.exe", ImeMode::English},
        {"clion64.exe", ImeMode::English},
        {"pycharm64.exe", ImeMode::English},
        {"webstorm64.exe", ImeMode::English},
        {"rider64.exe", ImeMode::English},
        {"goland64.exe", ImeMode::English},
        {"datagrip64.exe", ImeMode::English},
        {"notepad++.exe", ImeMode::English},
        {"vim.exe", ImeMode::English},
        {"nvim.exe", ImeMode::English},
        {"nvim-qt.exe", ImeMode::English},

        // Dev tools
        {"git.exe", ImeMode::English},
        {"ssh.exe", ImeMode::English},
        {"githubdesktop.exe", ImeMode::English},
    };
}

bool ConfigManager::load(const std::string& configPath) {
    std::ifstream file(configPath);
    if (!file.is_open()) {
        std::cerr << "ConfigManager: Cannot open " << configPath << std::endl;
        setDefaults();
        return false;
    }

    std::stringstream ss;
    ss << file.rdbuf();
    std::string content = ss.str();
    file.close();

    if (content.empty()) {
        std::cerr << "ConfigManager: Empty config file" << std::endl;
        setDefaults();
        return false;
    }

    size_t pos = 0;
    try {
        skipWhitespace(content, pos);
        if (pos >= content.size() || content[pos] != '{') {
            setDefaults();
            return false;
        }
        if (!parseObject(content, pos)) {
            setDefaults();
            return false;
        }
    } catch (...) {
        std::cerr << "ConfigManager: Failed to parse config.json" << std::endl;
        setDefaults();
        return false;
    }

    // If no rules were loaded, use defaults
    if (rules_.empty()) {
        setDefaults();
    }

    return true;
}

ImeMode ConfigManager::getTargetMode(const std::string& processName, bool isPassword) const {
    // Password fields always use English
    if (isPassword) {
        return ImeMode::English;
    }

    // Look up process name in rules
    std::string lowerName = processName;
    std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    auto it = rules_.find(lowerName);
    if (it != rules_.end()) {
        return it->second;
    }

    return defaultMode_;
}

// ===================== Minimal JSON Parser =====================

void ConfigManager::skipWhitespace(const std::string& s, size_t& pos) {
    while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\t' || s[pos] == '\n' || s[pos] == '\r')) {
        ++pos;
    }
}

std::string ConfigManager::parseString(const std::string& s, size_t& pos) {
    skipWhitespace(s, pos);
    if (pos >= s.size() || s[pos] != '"') {
        throw std::runtime_error("Expected '\"' at position " + std::to_string(pos));
    }
    ++pos; // skip opening quote

    std::string result;
    while (pos < s.size() && s[pos] != '"') {
        if (s[pos] == '\\' && pos + 1 < s.size()) {
            ++pos; // skip backslash
            switch (s[pos]) {
                case '"':  result += '"';  break;
                case '\\': result += '\\'; break;
                case '/':  result += '/';  break;
                case 'n':  result += '\n'; break;
                case 'r':  result += '\r'; break;
                case 't':  result += '\t'; break;
                default:   result += s[pos]; break;
            }
        } else {
            result += s[pos];
        }
        ++pos;
    }

    if (pos >= s.size()) {
        throw std::runtime_error("Unterminated string");
    }
    ++pos; // skip closing quote
    return result;
}

bool ConfigManager::parseObject(const std::string& s, size_t& pos) {
    ++pos; // skip '{'
    skipWhitespace(s, pos);

    while (pos < s.size() && s[pos] != '}') {
        std::string key = parseString(s, pos);
        skipWhitespace(s, pos);

        // Expect ':'
        if (pos >= s.size() || s[pos] != ':') {
            return false;
        }
        ++pos;
        skipWhitespace(s, pos);

        if (pos >= s.size()) return false;

        if (s[pos] == '{') {
            // Nested object - currently only "rules" uses this
            if (key == "rules") {
                ++pos; // skip '{'
                skipWhitespace(s, pos);
                while (pos < s.size() && s[pos] != '}') {
                    std::string ruleKey = parseString(s, pos);
                    // Normalize to lowercase
                    std::transform(ruleKey.begin(), ruleKey.end(), ruleKey.begin(),
                                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

                    skipWhitespace(s, pos);
                    if (pos >= s.size() || s[pos] != ':') return false;
                    ++pos;
                    skipWhitespace(s, pos);

                    std::string ruleValue = parseString(s, pos);
                    rules_[ruleKey] = stringToMode(ruleValue);

                    skipWhitespace(s, pos);
                    if (pos < s.size() && s[pos] == ',') ++pos;
                    skipWhitespace(s, pos);
                }
                if (pos < s.size() && s[pos] == '}') ++pos;
            } else {
                // Skip unknown nested objects
                int depth = 1;
                ++pos;
                while (pos < s.size() && depth > 0) {
                    if (s[pos] == '{') ++depth;
                    else if (s[pos] == '}') --depth;
                    ++pos;
                }
            }
        } else if (s[pos] == '"') {
            // String value
            std::string value = parseString(s, pos);
            if (key == "default_mode") {
                defaultMode_ = stringToMode(value);
            } else if (key == "switch_method") {
                switchMethod_ = stringToSwitchMethod(value);
            }
        } else {
            // Skip other value types (numbers, booleans, etc.)
            while (pos < s.size() && s[pos] != ',' && s[pos] != '}') {
                ++pos;
            }
        }

        skipWhitespace(s, pos);
        if (pos < s.size() && s[pos] == ',') ++pos;
        skipWhitespace(s, pos);
    }

    if (pos < s.size() && s[pos] == '}') ++pos;
    return true;
}

ImeMode ConfigManager::stringToMode(const std::string& modeStr) {
    std::string lower = modeStr;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (lower == "english" || lower == "en") {
        return ImeMode::English;
    }
    return ImeMode::Chinese;
}

SwitchMethod ConfigManager::stringToSwitchMethod(const std::string& methodStr) {
    std::string lower = methodStr;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (lower == "ctrl_space") return SwitchMethod::CtrlSpace;
    if (lower == "tsf") return SwitchMethod::TSF;
    return SwitchMethod::Shift;
}
