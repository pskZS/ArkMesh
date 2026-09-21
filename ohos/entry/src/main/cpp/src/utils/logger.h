#ifndef LOGGER_H
#define LOGGER_H

#include <string>
#include <hilog/log.h>

namespace arkmesh {

// Log levels
enum class LogLevel {
    DEBUG = 0,
    INFO = 1,
    WARN = 2,
    ERROR = 3
};

// Application-defined log domain for ArkMesh native logs
static constexpr unsigned int ARKMESH_LOG_DOMAIN = 0x0001;

// Simple logger class that routes to HarmonyOS HiLog
class Logger {
public:
    static Logger& getInstance() {
        static Logger instance;
        return instance;
    }

    void setLogLevel(LogLevel level) {
        currentLevel = level;
    }

    LogLevel getLogLevel() const {
        return currentLevel;
    }

    void log(LogLevel level, const std::string& tag, const std::string& message) {
        if (level < currentLevel) {
            return;
        }

        ::LogLevel hilogLevel;
        switch (level) {
            case LogLevel::DEBUG:
                hilogLevel = LOG_DEBUG;
                break;
            case LogLevel::INFO:
                hilogLevel = LOG_INFO;
                break;
            case LogLevel::WARN:
                hilogLevel = LOG_WARN;
                break;
            case LogLevel::ERROR:
                hilogLevel = LOG_ERROR;
                break;
            default:
                hilogLevel = LOG_INFO;
                break;
        }

        OH_LOG_Print(LOG_APP, hilogLevel, ARKMESH_LOG_DOMAIN, tag.c_str(),
                     "%{public}s", message.c_str());
    }

private:
    Logger() = default;
    LogLevel currentLevel = LogLevel::INFO;
};

// Convenience macros
#define LOGD(tag, msg) arkmesh::Logger::getInstance().log(arkmesh::LogLevel::DEBUG, tag, msg)
#define LOGI(tag, msg) arkmesh::Logger::getInstance().log(arkmesh::LogLevel::INFO, tag, msg)
#define LOGW(tag, msg) arkmesh::Logger::getInstance().log(arkmesh::LogLevel::WARN, tag, msg)
#define LOGE(tag, msg) arkmesh::Logger::getInstance().log(arkmesh::LogLevel::ERROR, tag, msg)

} // namespace arkmesh

#endif // LOGGER_H
